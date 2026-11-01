// turn 状态机（plugin/agent 的核心切片）。
// begin_turn 开轮 → dispatch_loop 分发工具（确认类挂起提案/异步登记归流/同步直执）
// → append_results_and_continue 按序回灌并进下一轮 → finish_turn 收口（迟滞执行
// 上下文压缩，执行体在 src/ctx_compress.cpp）。失败/取消的统一收口
// fail_turn/cleanup_turn_ 也在本文件。

#include "agent_plugin.hpp"

namespace vc_agent {

namespace {

constexpr char kSystemPrompt[] =
    "你是 Ithaca——一个运行在终端里的编程代理。用户用自然语言下达任务，"
    "你通过 tools 列出的工具读写工作区文件、执行构建。规则："
    "1) 编辑直接用 apply 提交（write=整文件 / replace=精确块，find 默认必须恰好"
    "命中一次，replace_all=true 全部替换）。大段修改优先拆成多个 replace 分块提交"
    "——参数只携带变化块；write 仅用于新建文件或全文重写。系统会校验、落盘并把 "
    "diff 展示给用户，用户可随时撤销；"
    "2) 需要批准的操作（如运行命令）由宿主把关，被拒绝时把拒绝当作工具结果继续任务；"
    "3) 命令类工具是异步的：提交后先返回 run_id，构建/测试的输出与退出码随后"
    "作为工具结果回给你，读错误输出自我修复；"
    "4) 工具失败时读错误信息自我纠正；"
    "5) Python 依赖一律装进工作区内的虚拟环境（python -m venv .venv，之后用"
    " .venv 里的 python/-m pip 安装与运行）——依赖隔离且随工作区整体清理，"
    "装到用户级目录会污染用户环境且难以回收；"
    "6) 大而独立的子工作（聚焦检索/代码审查/批量机械修改）可用 task 派子代理"
    "并行处理：prompt 必须自包含（子代理看不到本对话），子代理只跑只读或明确"
    "授权的改动；其报告须经验证再采纳，不可盲信；"
    "7) 用中文简明汇报。";

}  // namespace

// 本槽事件订阅（slot 路由约定：slot = 会话槽 turn_no，见 contract/vc_events.hpp）。
// 多会话并行：LLM 事件族与 run 行/退出对主/子会话一律按槽单播（请求体 slot=turn_no）
// ——主会话槽在状态机消费之外**补 session 标重播**进广播通道（UI 按标路由到归属
// 会话的数据面；载荷字符串由 provider 的 Scratch 保活到本次派发结束，嵌套 emit
// 内仍有效）；子会话槽不重播——不可见性由"订阅才可达"保证。
void AgentCtx::subscribe_turn_slot_() {
    slot_subs_.clear();   // 上 turn 槽已无事件源（run 均归流收口后才收口 turn）
    const std::uint64_t slot = static_cast<std::uint64_t>(turn_no_);
    // done/error 的状态机推进推迟一拍（载荷深拷贝过 poster，与 dispatch_after_done_
    // 同款纪律）：收口可能级联 cleanup→flush→begin_turn→槽重订。内核派发是快照
    // 迭代（类型化/广播/slot 三路同律——栈内退订/重订本就内存安全），推迟一拍的
    // 依据是两条 ABI 纪律：载荷字符串调用期有效，过 poster 的状态机推进必须持
    // 拷贝；嵌套 emit 的跨订阅者因果序内核不内建（EventHub 头注释）——收口移出
    // 派发栈，兄弟订阅者先见完整事件再见证态后果。真实踩过: 0xc0000005（探针
    // 复现，本纪律修复；当时的"raw 通道非快照迭代"归因经内核复核不成立）。
    // 重播（UI 侧）保持同步——它不碰订阅面。turn_no_ 守卫丢弃重订后旧槽的迟到事件
    slot_subs_.push_back(host_.on_slot(vc_api::llm_done, slot, [this](const void* p) {
        const auto* e = static_cast<const vc_api::LlmDone*>(p);
        if (!subagent_) retag_emit_(vc_api::llm_done, e);
        const std::string aj = e->assistant_json != nullptr ? e->assistant_json : "";
        const std::string us = e->usage != nullptr ? e->usage : "";
        const auto beat = [this, turn = turn_no_, rid = e->request_id, aj, us] {
            if (turn_no_ != turn) return;   // 重订后旧槽迟到事件：丢弃
            const vc_api::LlmDone copy{rid, aj.c_str(), us.c_str(), 0, session_id_.c_str()};
            on_done(copy);
        };
        vckit::poster_defer(poster_, beat);
    }));
    slot_subs_.push_back(host_.on_slot(vc_api::llm_error, slot, [this](const void* p) {
        const auto* e = static_cast<const vc_api::LlmError*>(p);
        if (!subagent_) retag_emit_(vc_api::llm_error, e);
        const std::string msg = e->message != nullptr ? e->message : "";
        const auto beat = [this, turn = turn_no_, rid = e->request_id, code = e->code, msg] {
            if (turn_no_ != turn) return;   // 重订后旧槽迟到事件：丢弃
            const vc_api::LlmError copy{rid, code, msg.c_str(), 0, session_id_.c_str()};
            on_llm_error(copy);
        };
        vckit::poster_defer(poster_, beat);
    }));
    slot_subs_.push_back(host_.on_slot(vc_api::llm_delta, slot, [this](const void* p) {
        const auto* e = static_cast<const vc_api::LlmDelta*>(p);
        if (subagent_)
            return emit_subagent_(turn_no_, 1, nullptr, e->text != nullptr ? e->text : "");
        retag_emit_(vc_api::llm_delta, e);
    }));
    slot_subs_.push_back(host_.on_slot(vc_api::llm_reasoning, slot, [this](const void* p) {
        const auto* e = static_cast<const vc_api::LlmReasoning*>(p);
        if (!subagent_) retag_emit_(vc_api::llm_reasoning, e);
    }));
    slot_subs_.push_back(host_.on_slot(vc_api::run_line, slot, [this](const void* p) {
        const auto* e = static_cast<const vc_api::RunLine*>(p);
        if (AgentCtx::AsyncRun* run = find_run(e->run_id);
            run != nullptr && e->text != nullptr)
            // stderr 行打标进合并输出——结果里 stdout/stderr 来源可辨（命令失败
            // 归因不靠猜）；UI 的 run_line 流保持原样，不受此标记影响
            run->output += (e->stream == 1 ? "[stderr] " : "") +
                           std::string(e->text) + "\n";
        if (!subagent_) retag_emit_(vc_api::run_line, e);
    }));
    slot_subs_.push_back(host_.on_slot(vc_api::run_exit, slot, [this](const void* p) {
        const auto* e = static_cast<const vc_api::RunExit*>(p);
        AgentCtx::AsyncRun* run = find_run(e->run_id);
        if (run == nullptr) return;  // 非本 turn 的 run（取消后的迟到孤儿）：静默
        run->exited = true;
        run->exit_code = e->exit_code;
        if (!subagent_) retag_emit_(vc_api::run_exit, e);
        try_finish_tools();  // 收口门槛（子代理在途/decision 挂起）各 ctx 自查
    }));
    // 编辑应用/撤销：workspace 直接广播而它不识会话（工具 ABI 只传 turn_id）——
    // 本槽按 turn_id 认领并补标重播（session 非空 = 重播副本，跳过防重入环）
    slot_subs_.push_back(host_.on(vc_api::edit_applied, [this](const void* p) {
        const auto* e = static_cast<const vc_api::EditApplied*>(p);
        if (subagent_ || e->turn_id != turn_no_ || e->session != nullptr) return;
        retag_emit_(vc_api::edit_applied, e);
    }));
    slot_subs_.push_back(host_.on(vc_api::edit_undone, [this](const void* p) {
        const auto* e = static_cast<const vc_api::EditUndone*>(p);
        if (subagent_ || e->turn_id != turn_no_ || e->session != nullptr) return;
        retag_emit_(vc_api::edit_undone, e);
    }));
}

// ---- turn 状态机 ------------------------------------------------------------

void AgentCtx::begin_turn(const std::string& text, const std::string& attachments_json) {
    turn_no_ = next_turn_no();
    state_ = State::TurnActive;
    runs_.clear();
    compress_retries_ = 0;
    // 本槽事件订阅（先于 discover_tools——工具事件可能在本 turn 内即到）：
    // 上 turn 的槽订阅此时已无事件源，直接换订
    subscribe_turn_slot_();
    // 上下文策略表轮边界解析（先于 discover_tools——recall 能力位影响工具开合）
    resolve_ctx_strategy_();
    // 类型清单 turn 边界重载（热编辑即生效）：先于 discover_tools——task 自述
    // 要带上最新类型名。仅主会话（子代理不再派生，类型无用）
    if (depth_ == 0) load_agent_types_();
    if (!discover_tools())
        return fail_turn("没有可用的工具表（检查 ITHACA_AGENT_TOOLS）");
    // M6 session：恢复的会话保留历史（含 system），只在空会话时注入 system；
    // 子代理 = 类型 prompt 覆盖
    if (messages_.empty())
        messages_.push_back(json{{"role", "system"},
                                 {"content", system_prompt_.empty()
                                                 ? std::string(kSystemPrompt)
                                                 : system_prompt_}});
    // 多模态附件：图片把 user content 升为 OpenAI parts 数组（text + image_url
    // data_url 内联，随上下文持久——多轮对话的视觉历史）；未内联的附件（文档类、
    // 读取失败的图片）在 text 尾部拼路径注记，模型据此用 read 处理文件本体。
    // NOTE: 注记是模型语境的管道，只在这里注入——TurnInput.text 即用户消息原文
    // （webui 对话流直接渲染 transcript 文本，管道文本混进用户消息就是展示噪音）。
    // 纯文本消息保持字符串形态不变。
    json att = json::parse(attachments_json.empty() ? "[]" : attachments_json.c_str(),
                           nullptr, /*allow_exceptions=*/false);
    std::string text_with_note = text;
    json image_parts = json::array();
    if (att.is_array() && !att.empty()) {
        std::string note_paths;
        for (const auto& a : att) {
            if (!a.is_object()) continue;
            const std::string du = a.value("data_url", "");
            if (!du.empty())
                image_parts.push_back(json{{"type", "image_url"},
                                           {"image_url", json{{"url", du}}}});
            else
                note_paths += (note_paths.empty() ? "" : "、") + a.value("path", "");
        }
        if (!note_paths.empty())
            text_with_note += "\n\n（附件已上传至工作区，可用 read 读取：" + note_paths + "）";
    }
    json user_content = text_with_note;
    if (!image_parts.empty()) {
        image_parts.insert(image_parts.begin(),
                           json{{"type", "text"}, {"text", text_with_note}});
        user_content = std::move(image_parts);
    }
    messages_.push_back(json{{"role", "user"}, {"content", user_content}});
    // mission 任务目标卡（上下文压缩阶段一）：首轮建档后跨轮冻结——卡坐在
    // system 之后，逐轮重写会让其后整段历史前缀失效（缓存前缀纪律）；锚点在
    // 压缩折叠时吸收。子代理是单任务短命上下文，任务卡与 user 消息逐字重复，
    // 纯耗 token
    if (!subagent_) ensure_mission_();
    // 开轮即落盘（含本条 user 消息与任务卡）：持久化点若只在 turn 收口/压缩，
    // 进程在 turn 中途死掉时本条请求从历史蒸发，重放与恢复都不留痕（踩过: 
    // 子代理在途挂死 → 重启 → 「继续」被接进上一个任务的语境）。恢复后未应答
    // 的 user 消息是合法序列，下轮正常续接。子代理不持久化（session_path_ 为空）
    persist_session_();
    send_chat();
}

void AgentCtx::send_chat() {
    try {
        send_chat_impl();
    } catch (...) {
        fail_unexpected_();   // 请求体 dump/压缩预检抛出 ⇒ 终态收口，不悬停
    }
}

void AgentCtx::send_chat_impl() {
    // 子代理轮数护栏：先于发号判定——超限 fail_turn，不再产生新的在途请求
    if (subagent_ && ++rounds_ > kMaxChildRounds)
        return fail_turn("子代理超出轮数上限（" + std::to_string(kMaxChildRounds) + "）");
    // provider 每轮重新解析：同名服务被竞争顶掉时自然切换，agent 无感
    llm_ = host_.service<vc_api::LlmTable>(vc_api::llm);
    // 窗口/输出上限自述兜底：env / set_active 没给全（窗口未知或 max_output 未知）
    // ⇒ 向 provider info() 探一次（每个 provider 实例至多一次）。优先级：
    // env > set_active > info（env 显式覆盖最高；info 只补缺口）。
    if ((!ctx_window_env_ && ctx_window_ == 0 || ctx_max_output_ == 0) &&
        llm_ != nullptr && llm_->info != nullptr && llm_ != window_probed_llm_) {
        window_probed_llm_ = llm_;
        const char* info_json = nullptr;
        if (llm_->info(llm_->self, &info_json) == NOSTOS_OK && info_json != nullptr) {
            const json info = json::parse(info_json, nullptr, false);
            if (info.is_object()) {
                if (ctx_window_ == 0 && !ctx_window_env_) {
                    if (const long long w = info.value("context_window", 0LL); w > 0)
                        ctx_window_ = w;
                }
                if (ctx_max_output_ == 0)
                    ctx_max_output_ = info.value("max_output_tokens", 0LL);
                recompute_threshold_();   // 元数据到位 ⇒ 阈值按输出预留重算
            }
        }
    }
    inflight_ = next_chat_seq();
    // 请求体组装（折叠重建与首发共用一份口径）：slot/subagent 必须随体——预检
    // 折叠重建的请求同样要走槽路由（踩过: 重建体丢 slot ⇒ 响应按槽单播无人接收，
    // turn 静默悬挂且无任何错误痕迹；窗口小到预检折叠常触发时必现）
    const auto make_body = [&] {
        json b = {{"messages", messages_}, {"slot", turn_no_}};
        if (!tools_.empty()) b["tools"] = tools_;
        if (subagent_) b["subagent"] = true;
        return b;
    };
    body_buf_ = make_body().dump();
    if (llm_ == nullptr || llm_->chat_ex == nullptr)
        return fail_turn("LLM 不可用");
    // 发送前预检：请求体粗估超生效阈值 ⇒ 就地机械折叠后重建请求体再发——单
    // turn 几十轮工具往返在轮间主动瘦身，省一次 provider 400 往返；LLM 摘要仍只
    // 在 turn 边界。粗估宁高勿低：误折代价 = 上下文略瘦，可接受。子代理不压缩
    // ——折叠产物会破坏「报告 = 最后一条 assistant」的回灌语义。
    // 策略缝：表接管时门槛判定在策略内（signals 带请求体粗估），计划经
    // acquire_plan_；无表走内置阈值门槛 + plan_compress。预检恒机械（不能等 LLM）。
    if (!subagent_) {
        const long long est = estimate_prompt_tokens(body_buf_);
        const bool gate = ctx_tbl_ != nullptr && ctx_tbl_->plan != nullptr
                              ? true
                              : (session_threshold_ > 0 && est > session_threshold_);
        if (gate) {
            vcagent::CompressPlan plan = plan_now_("precheck", est);
            if (plan.valid) {
                pending_plan_ = plan;
                replace_with_digest_(plan.digest, "发送前预检");
                pending_plan_ = {};
                last_prompt_tokens_ = 0;   // 旧信号已失效（上下文已折叠），待下次回报
                body_buf_ = make_body().dump();
            }
        }
    }
    if (const nostos_status st = llm_->chat_ex(llm_->self, inflight_, body_buf_.c_str());
        st != NOSTOS_OK)
        return fail_turn("LLM 未受理（status=" + std::to_string(static_cast<int>(st)) + "）");
}

// 工具分发循环：跳过已完成项；确认类工具挂起等 decision；异步工具登记等归流；
// 全部完成 ⇒ 结果按序回灌 ⇒ 下一轮 LLM。
void AgentCtx::dispatch_loop() {
    while (dispatched_ < calls_.size()) {
        ToolCallCtx& ctx = calls_[dispatched_];
        if (ctx.done) {
            ++dispatched_;
            continue;
        }
        bool suspend = false;
        bool excepted = false;   // 异常兜底路径标记（catch 块内直接判，不反查串）
        try {
            suspend = dispatch_one_(ctx);
        } catch (const std::exception& e) {
            // 单条分发异常兜底：异常穿出循环会让状态机停在抛点 ⇒ send_chat 永不
            // 执行 ⇒ turn 悬停无终态（内核 ABI 边界救不了本状态机）。转错误结果
            // 回灌，LLM 据此自修或换路
            ctx.result = err_result(std::string("工具执行异常: ") + e.what());
            settle_(ctx, false);
            excepted = true;
        } catch (...) {
            ctx.result = err_result("工具执行异常（未知类型）");
            settle_(ctx, false);
            excepted = true;
        }
        if (excepted)
            host_.log(3, "ithaca.agent: 工具分发异常已兜底 turn #" +
                             std::to_string(turn_no_) + " tool=" + ctx.full_name);
        if (suspend) return;   // 挂起即返：跳过循环尾（返回值契约见 dispatch_one_）
    }
    // 分发完但仍未收口 ⇒ 看在途面：无任何挂起就地收口；有异步任务/在途子代理
    // 则等归流回调（run_exit 在审批窗口内提前到达时同样等齐，不提前收口）
    if (runs_.empty() && child_pends_.empty()) return append_results_and_continue();
    if (all_runs_exited_() && child_pends_.empty()) return try_finish_tools();
    const int pending = static_cast<int>(runs_.size() + child_pends_.size());
    notice_meta("等待 " + std::to_string(pending) + " 个后台任务/子代理完成…",
                "waiting", 0, pending);
}

// dispatch 推迟一拍（on_done 专用）：tool_call/tool_result 在 dispatch_one_ 内
// 同步 emit，而 llm_done 的订阅者不止本插件（webui 靠它落 assistant 收口记录）
// ——agent 先于 webui 装载时工具事件就落在收口记录之前。推迟一拍把「产生工具
// 事件」挪到 llm_done 对全部订阅者派发完成之后，事件序与因果序不再依赖插件装载
// 顺序（主会话与子代理同规）。安全性：主 ctx 与插件同生命周期（闭包由宿主
// dispose）；子代理走 owner 绑定——cleanup_turn_ 销毁前作废 owner，未执行闭包
// 连任务回收；turn_no_ 防旧闭包误入新 turn；poster 不可用 ⇒ 回退同步分发。
void AgentCtx::dispatch_after_done_() {
    if (poster_.post == nullptr) return dispatch_loop();
    const int turn = turn_no_;
    const auto body = [this, turn] {
        if (state_ == State::TurnActive && turn_no_ == turn) dispatch_loop();
    };
    const nostos_status st =
        subagent_ ? vckit::poster_submit_owner(poster_, this, body)
                  : vckit::poster_submit(poster_, body);
    if (st != NOSTOS_OK) dispatch_loop();
}

// 单条调用分发（dispatch_loop 的 try 保护体）：emit 调用卡 → 内置工具拦截 →
// 工具表执行/提案挂起。异常由调用方兜底转错误结果回灌。
bool AgentCtx::dispatch_one_(ToolCallCtx& ctx) {
    emit_tool_call(ctx.full_name, ctx.args);
    // 内置工具：context.recall（外置存储回查）——不经工具表，直接处理
    if (ctx.full_name == vcagent::kRecallTool) {
        ctx.result = exec_recall(ctx.args);
        settle_(ctx, true);
        return false;
    }
    // 内置工具：task（子代理派发）——不经工具表。spawn 失败 = 护栏/参数拒绝
    // （result 已写好，就地 emit 回灌）；成功 = 在途登记（begin_turn 同步失败
    // 的路径已在回调帧内收口，结果同帧写回）
    if (ctx.full_name == vcagent::kTaskTool) {
        if (!spawn_child_(dispatched_, ctx)) settle_(ctx, false);
        ++dispatched_;
        return false;
    }
    const Bound* b = find_bound(ctx.full_name);
    if (b == nullptr) {
        ctx.result = err_result("未知工具: " + ctx.full_name);
        settle_(ctx, false);
        return false;
    }
    // 提案判定：确认清单命中（suggest 全量 / auto-edit 的命令）或 auto-edit 下
    // apply 的 outside_root 预检命中（工作区外编辑升级为提案——判源 = 插件 dry
    // 结果的标记，jail 归属权在 workspace，agent 不复制在根判定）。
    // auto_all_（本轮免批）一票放行全部，含工作区外改动。
    bool propose = vcagent::needs_confirm(ctx.full_name, confirm_) && !auto_all_;
    std::string dry_out;
    json dry;
    bool dry_done = false;   // 预检已跑过 dry（结果在 dry_out/dry），提案段不再重跑
    if (!propose && !auto_all_ && mode_ == vcagent::kModeAutoEdit &&
        ctx.full_name == vcagent::kApplyTool) {
        json dry_args = ctx.args;
        dry_args["dry_run"] = true;
        dry_out = exec(*b, ctx.tool, dry_args);
        dry = json::parse(dry_out, nullptr, false);
        dry_done = true;
        if (dry.is_object() && dry.value("ok", true) == false) {
            // dry 失败与确认路径同规：错误经 tool_result 回灌 LLM 自修——真实
            // 执行会复现同一校验失败，直接回灌省一趟
            ctx.result = dry_out;
            ctx.done = true;
            emit_tool_result(ctx.full_name, false, ctx.result);
            append_results_and_continue();
            return true;   // 回灌已发车：挂起即返，循环尾不得再跑（会重复回灌）
        }
        propose = dry.value("outside_root", false);
    }
    if (propose) {
        // 泛化试算 + 提案（先置状态再 emit——嵌套派发会在本帧内回到
        // on_decision，状态没就位会被自己的守卫丢弃——M3 踩过）。
        if (!dry_done) {
            json dry_args = ctx.args;
            dry_args["dry_run"] = true;
            dry_out = exec(*b, ctx.tool, dry_args);
            dry = json::parse(dry_out, nullptr, false);
        }
        pending_.index = dispatched_;
        state_ = State::AwaitingDecision;
        // diff_text 只装行级 diff（原始结果 JSON 不可读——webui 截图自审抓到）；
        // 同文件多 op 已在 workspace 折叠，edits>1 时标注「N 处编辑」
        std::string diff_text;
        if (dry.is_object() && dry.contains("files") && dry["files"].is_array())
            for (const auto& f : dry["files"]) {
                if (f.contains("path")) {
                    std::string head =
                        "── " + f.value("path", std::string());
                    if (f.contains("edits") && f["edits"].is_number_integer() &&
                        f["edits"].get<int>() > 1)
                        head += "（" + std::to_string(f["edits"].get<int>()) + " 处编辑）";
                    diff_text += head + "\n";
                }
                if (f.contains("diff") && f["diff"].is_string())
                    diff_text += f["diff"].get<std::string>();
            }
        // dry 失败（ok:false）不进提案：错误经 tool_result 回灌 LLM 自修
        if (dry.is_object() && dry.value("ok", true) == false) {
            ctx.result = dry_out;
            ctx.done = true;
            emit_tool_result(ctx.full_name, false, ctx.result);
            append_results_and_continue();
            return true;   // 回灌已发车：挂起即返，循环尾不得再跑（会重复回灌）
        }
        // runner 类提案：would_run 单行卡（命令批准卡形态）
        if (diff_text.empty() && dry.is_object() && dry.contains("would_run") &&
            dry["would_run"].is_string())
            diff_text = "── " + ctx.full_name + "（命令批准）\n" +
                        dry["would_run"].get<std::string>() + "\n";
        if (diff_text.empty()) diff_text = dry_out;
        vc_agent::Scratch prop_scratch;
        const vc_api::EditProposed proposal{
            turn_no_, diff_text.c_str(), prop_scratch.hold(ctx.full_name),
            prop_scratch.hold(session_id_)};
        host_.emit(vc_api::edit_proposed, &proposal);
        return true;  // ← 死锁纪律：绝不同步等待；挂起即返，循环尾不得再跑
    }
    ctx.result = exec(*b, ctx.tool, ctx.args);
    settle_(ctx, tool_ok(ctx.result));
    // 异步长任务（runner.run）：登记 run_id——结果等 vc.run.exit 归流，
    // 本条消息的其余工具照常分发（runner 侧并发、run_id 归流）。
    if (const int rid = parse_run_id(ctx.result); rid > 0) {
        register_run(rid, dispatched_ - 1);
        notice_meta("后台任务 run#" + std::to_string(rid) + " 已启动",
                    "bg_run_started", rid);
    }
    return false;
}

// 单条调用统一收尾：结果卡 emit + 分发游标推进（调用点先写好 ctx.result）
void AgentCtx::settle_(ToolCallCtx& ctx, bool ok) {
    ctx.done = true;
    emit_tool_result(ctx.full_name, ok, ctx.result);
    ++dispatched_;
}

// task 工具的派发与收口（spawn_child_/on_child_finished_）在 src/ctx_child.cpp；
// 类型注册表在 src/agent_types.cpp。

void AgentCtx::append_results_and_continue() {
    try {
        append_results_and_continue_impl();
    } catch (...) {
        fail_unexpected_();   // 回灌 dump/外置抛出 ⇒ 终态收口，不悬停
    }
}

// 全部工具调用完成 ⇒ 结果按序回灌 ⇒ 下一轮 LLM。
// 上下文压缩阶段一：超内联上限（4KB）的成功/失败结果外置到 raw/，消息里只留
// 确定性模板摘要 + ref（锚点逐字节保留；失败 2KB 内保留详细错误，ref 兜底全量）
void AgentCtx::append_results_and_continue_impl() {
    for (std::size_t ci = 0; ci < calls_.size(); ++ci) {
        const ToolCallCtx& ctx = calls_[ci];
        std::string content = ctx.result;
        // 提案试算失败的提前回灌（dispatch_one_ 的 dry-fail 分支）发生在分发中途
        // ——本循环遍历全部 calls_，未分发项的 result 为空串。OpenAI 协议要求
        // assistant.tool_calls 的每个 id 都有配对 tool 消息：补点名"未执行"的
        // 错误结果（空串会被当成"执行了但无输出"误判）
        if (!ctx.done && content.empty())
            content = err_result("同批前序调用试算失败收口，本调用未执行");
        // 策略缝：表接管时收编（外置+最小记录一步）整个在策略内；无表走内置
        // 管线（单源 = vcagent::ingest_tool_result，ctx_basic 策略镜像共用）
        if (ctx_tbl_ != nullptr && ctx_tbl_->ingest_tool_result != nullptr) {
            content = strategy_ingest_(ctx.full_name, content, ci);
        } else {
            content = vcagent::ingest_tool_result(
                vcagent::tool_last(ctx.full_name), ctx.full_name, content,
                [&](const std::string&, const std::string& full, const std::string& body) {
                    return spool_raw(full, body);
                });
        }
        messages_.push_back(json{{"role", "tool"},
                                 {"tool_call_id", ctx.call_id},
                                 {"content", content}});
    }
    send_chat();
}

// raw/ 外置存储与 context.recall 回查在 src/ctx_compress.cpp。

void AgentCtx::try_finish_tools() {
    try {
        try_finish_tools_impl();
    } catch (...) {
        fail_unexpected_();   // run 结果组包（输出含进程原始字节）dump 抛出 ⇒ 终态收口
    }
}

// 收口门槛与 run 登记（三处判定/两处登记的收敛点，声明见 agent_plugin.hpp）
bool AgentCtx::all_runs_exited_() const {
    for (const auto& [rid, run] : runs_)
        if (!run.exited) return false;
    return true;
}

void AgentCtx::register_run(int run_id, int index) {
    runs_[run_id] = AsyncRun{static_cast<std::size_t>(index), "", false, 0};
}

// run.exit 到达后：所有分发完成且全部 run 归流 ⇒ 收口（正在等 decision 或有
// 在途子代理时不收口——前者延迟到 decision 恢复后的 dispatch_loop，后者由最后
// 完成的子代理回调触发同一门槛）
void AgentCtx::try_finish_tools_impl() {
    if (state_ != State::TurnActive) return;
    if (dispatched_ != calls_.size()) return;
    if (!child_pends_.empty()) return;
    if (runs_.empty()) return;  // 无异步任务：常规收口归 dispatch_loop，防双重 send_chat
    if (!all_runs_exited_()) return;
    for (const auto& [rid, run] : runs_) {
        ToolCallCtx& ctx = calls_[run.index];
        ctx.result = json{{"run_id", rid},
                          {"exit_code", run.exit_code},
                          {"output", run.output}}
                         .dump();
        ctx.done = true;
        emit_tool_result(ctx.full_name, run.exit_code == 0, ctx.result);
    }
    runs_.clear();
    // 工具消息必须按序回灌后再进下一轮——直接 send_chat 会留下"assistant 带
    // tool_calls 但无 tool 应答"的非法序列，真实 API 400
    append_results_and_continue();
}

// 确认清单 = 权限模式映射（permission.hpp 的纯函数，单测直接覆盖它）：
// suggest 逐条问 / auto-edit 命令逐条问 + 工作区外编辑走 outside_root 预检 /
// full-auto 全自动（把关责任在用户对模式的选择，沙箱默认关闭）；
// 未识别值按 auto-edit 兜底
void AgentCtx::rebuild_confirm_() {
    confirm_ = vcagent::mode_confirm(mode_);
}

void AgentCtx::finish_turn() {
    // 子代理收口：最后一条 assistant 正文即报告，经回调交父（不持久化/不压缩/
    // 不排队——子代理是单任务短命上下文，无 turn 边界后续）
    if (subagent_) {
        state_ = State::Idle;
        std::string report;
        for (auto it = messages_.rbegin(); it != messages_.rend(); ++it)
            if (it->is_object() && it->value("role", "") == "assistant") {
                report = it->value("content", "");
                break;
            }
        emit_subagent_(turn_no_, 4, nullptr, trim(report, 2000));   // 工作记录视图：完成卡
        if (on_finished_) on_finished_(true, report, rounds_);
        return;
    }
    // turn 完成只发 host log，不发 notice——webui 对话流不需要；token 统计
    // 已随 assistant 记录回传
    host_.log(2, "ithaca.agent: turn #" + std::to_string(turn_no_) + " 完成（tokens total " +
                     std::to_string(usage_total_) + "）");
    persist_session_();
    // 上下文压缩（阶段二两态）：判定实时——硬保护 0.95×W 独立前置，
    // 或会话阈值（迟滞）；执行只在 turn 边界。折叠两态：LLM 自总结（异步摘要请求）
    // → 机械 digest 兜底；硬保护强制机械折叠（摘要输出量不可控，也不等它）。
    if (maybe_compress_()) return;  // 已收口（机械）或摘要在途（收口在回包/失败分流）
    state_ = State::Idle;
    flush_queued_inputs_();
}

// ---- 上下文压缩（阈值/判定/摘要/折叠）在 src/ctx_compress.cpp ------

// agent 内部失败的统一收口（没有工具表 / LLM 不可用 / LLM 未受理等）：发
// llm_error——webui 由此清 in_flight + 落错误卡，transcript 有终态记录（重放
// 安全）。request_id=0 是「agent 自发」哨兵：自己订阅的 on_llm_error 靠
// request_id != inflight_ 忽略它，否则同步派发重入 cleanup 会把排队输入排空两次。
// 子代理失败静默：不发哨兵（会在父会话落错误卡并提前清 in_flight），失败原因
// 经完成回调作为 task 工具结果回灌，父 LLM 自行处置。
void AgentCtx::fail_turn(const std::string& why) {
    if (subagent_) {
        host_.log(3, "ithaca.agent: 子代理 turn #" + std::to_string(turn_no_) +
                         " 失败: " + why);
        state_ = State::Idle;
        emit_subagent_(turn_no_, 5, nullptr, trim(why, 500));   // 工作记录视图：失败卡
        if (on_finished_) on_finished_(false, why, rounds_);
        calls_.clear();
        runs_.clear();
        return;
    }
    const std::string msg = "turn #" + std::to_string(turn_no_) + " 失败: " + why;
    const vc_api::LlmError e{0, NOSTOS_ERR_FAILED, msg.c_str(), 0, session_id_.c_str()};
    host_.emit(vc_api::llm_error, &e);
    cleanup_turn_();
}

// turn 机器统一异常兜底（语义与 compress 清理的理由见 agent_plugin.hpp 声明处）
void AgentCtx::fail_unexpected_() noexcept {
    compress_inflight_ = false;
    compress_seq_ = -1;
    try {
        try {
            throw;
        } catch (const std::exception& e) {
            fail_turn(std::string("turn 内部异常收口: ") + e.what());
        } catch (...) {
            fail_turn("turn 内部异常收口（未知异常类型）");
        }
    } catch (...) {
        // fail_turn 链自身再出异常（级联清理/压缩判定等）：最低限度复位状态机
        // ——悬停 TurnActive 比任何半清现场都糟
        state_ = State::Idle;
        host_.log(3, "ithaca.agent: turn 异常收口链自身抛出——已强制回 Idle");
    }
}

// 状态机复位（不含对外事件——收口事件由调用方决定发不发）：持久化 + 清工具现场
// + 压缩判定（失败/取消收口同样过判定——上下文只增不减的反面）
// + 回 Idle + 排队输入 flush（栈深有界）。
void AgentCtx::cleanup_turn_(bool keep_partial) {
    if (keep_partial)
        settle_cancelled_calls_();   // 主流式取消：现场保留，注入合成结果恢复配对
    else
        // 先清悬挂工具尾巴再持久化：异常/失败收口时 messages_ 可能停在分发中途，
        // 带 tool_calls 的 assistant 没有全部应答就落盘，下次请求即 provider 400
        // 且每轮复现成错误循环
        truncate_dangling_tool_exchange_();
    // 子代理级联收场（取消/失败同走此门）：在途子代理停掉并随现场销毁——它们
    // 是本 turn 的派生，父已终态则子无意义。子自己派的后台进程不追杀（runner
    // 自有超时），log 留痕。置 inflight_=-1 防 provider 迟到的取消回包误入路由。
    // NOTE: 每个被杀子代理补发 phase-5 终态卡——回调已被禁用（父现场正被清），
    // 不补发则该子代理在 transcript 里永远「进行中」（踩过）
    if (!children_.empty()) {
        llm_ = host_.service<vc_api::LlmTable>(vc_api::llm);
        int killed = 0;
        for (auto& c : children_) {
            c->on_finished_ = nullptr;   // 级联期间禁回调：父现场正被清空
            if (c->state_ == State::TurnActive) {
                if (llm_ != nullptr && llm_->cancel != nullptr)
                    llm_->cancel(llm_->self, c->inflight_);
                c->inflight_ = -1;
                c->state_ = State::Idle;
                emit_subagent_(c->turn_no_, 5, nullptr, "已随主会话取消");
                ++killed;
            }
            if (!c->runs_.empty())
                host_.log(3, "ithaca.agent: 子代理 #" + std::to_string(c->turn_no_) +
                                 " 尚有 " + std::to_string(c->runs_.size()) +
                                 " 个后台进程在途——留待 runner 超时收尾");
        }
        // 子 ctx 随现场销毁：销毁前作废各自名下的未执行投递（dispatch 推迟一拍
        // 的 owner 绑定闭包）——迟到的旧闭包随任务一起回收，不落已析构对象。
        // 零值 poster（未领取）走 vckit 助手的 BAD_STATE 短路，无需预判
        for (auto& c : children_)
            vckit::poster_invalidate_owner(poster_, c.get());
        children_.clear();
        child_pends_.clear();
        // 逐子代理的取消卡已承载终态，聚合通告不再发（纯刷屏）
        host_.log(2, "ithaca.agent: 已取消 " + std::to_string(killed) + " 个在途子代理");
    }
    persist_session_();
    calls_.clear();
    runs_.clear();
    // 摘要在途：折叠计划已冻结（pending_plan_），收口由摘要回包/失败分流完成
    // （on_compress_done_/on_compress_error_ → apply_digest_）——此处只清现场，
    // 不动 state_/排队。抢跑 flush 会让新 turn 的请求与摘要回包的区间替换交叠，
    // 破坏「摘要在途时 turn 请求不在途」的不变式（顺序纪律）。
    if (compress_inflight_) return;
    if (maybe_compress_()) return;  // 命中判定：机械折叠收口，或摘要在途（异步收口）
    state_ = State::Idle;
    flush_queued_inputs_();
}

// 取消现场配对（主流式取消，语义见 agent_plugin.hpp 声明处）：未决调用注入
// 合成结果（形态与拒绝反馈一致），已完成的调用保留真实结果；随后统一回灌进
// 历史——assistant(tool_calls) 与 tool 消息恢复一一配对，下轮请求合法且带
// 中断现场（模型知道停在哪，可续不重做；前缀缓存也因此不烧）
void AgentCtx::settle_cancelled_calls_() {
    for (auto& c : calls_) {
        if (c.done) continue;
        c.result = err_result("已被用户取消");
        c.done = true;
    }
    for (const auto& c : calls_)
        messages_.push_back(json{{"role", "tool"},
                                 {"tool_call_id", c.call_id},
                                 {"content", c.result}});
}

// 排队输入 flush：队首开新 turn，其余等该 turn 收口时再取（栈深有界——不递归
// 弹空整队）。
void AgentCtx::flush_queued_inputs_() {
    if (queued_.empty()) return;
    const InputItem next = queued_.front();
    queued_.pop_front();
    begin_turn(next.text, next.attachments_json);
}

// 悬挂工具回合尾巴清理（语义见 agent_plugin.hpp 声明处）：tool 应答与其所属
// assistant 成对弹出，遇到完整消息（无 tool_calls 的 assistant / user / system）
// 即停——mission 卡与 digest 都是 system 角色，天然不受影响。
void AgentCtx::truncate_dangling_tool_exchange_() {
    while (!messages_.empty()) {
        const json& last = messages_.back();
        if (!last.is_object()) break;
        const std::string role = last.value("role", "");
        if (role == "tool" ||
            (role == "assistant" && last.contains("tool_calls") &&
             last["tool_calls"].is_array() && !last["tool_calls"].empty())) {
            messages_.erase(messages_.size() - 1);
            continue;
        }
        break;
    }
}

}  // namespace vc_agent
