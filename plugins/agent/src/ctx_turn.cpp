// turn 状态机（plugin/agent 的核心切片）。
// begin_turn 开轮 → dispatch_loop 分发工具（确认类挂起提案/异步登记归流/同步直执）
// → append_results_and_continue 按序回灌并进下一轮 → finish_turn 收口（迟滞执行
// 上下文压缩）。失败/取消的统一收口 fail_turn/cleanup_turn_ 也在本文件。

#include "agent_plugin.hpp"

#include <algorithm>  // std::clamp：recall 分页参数收敛
#include <fstream>    // std::ofstream：raw 外置归档



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
    " .venv 里的 python/-m pip 安装与运行）——完全访问模式下工作区外不可写，"
    "装到用户级目录会失败或不可见；"
    "6) 大而独立的子工作（聚焦检索/代码审查/批量机械修改）可用 task 派子代理"
    "并行处理：prompt 必须自包含（子代理看不到本对话），子代理只跑只读或明确"
    "授权的改动；其报告须经验证再采纳，不可盲信；"
    "7) 用中文简明汇报。";

}  // namespace

// 本槽事件订阅（slot 路由约定：slot = 会话槽 turn_no，见 contract/vc_events.hpp）。
// 主会话槽的 run 事件重播到广播通道——webui/selftest 的消费路径迁移前后一致；
// 重播在状态推进之前（保持迁移前 transcript 里 run_exit 先于工具结果卡的顺序）。
// 子会话槽不重播：run 与 LLM 事件都只在归属槽可见，泄漏由"订阅才可达"结构性排除。
void AgentCtx::subscribe_turn_slot_() {
    slot_subs_.clear();   // 上 turn 槽已无事件源（run 均归流收口后才收口 turn）
    const std::uint64_t slot = static_cast<std::uint64_t>(turn_no_);
    slot_subs_.push_back(host_.on_slot(vc_api::run_line, slot, [this](const void* p) {
        const auto* e = static_cast<const vc_api::RunLine*>(p);
        if (AgentCtx::AsyncRun* run = find_run(e->run_id);
            run != nullptr && e->text != nullptr)
            // stderr 行打标进合并输出——结果里 stdout/stderr 来源可辨（命令失败
            // 归因不靠猜）；UI 的 run_line 流保持原样，不受此标记影响
            run->output += (e->stream == 1 ? "[stderr] " : "") +
                           std::string(e->text) + "\n";
        if (!subagent_) host_.emit(vc_api::run_line, p);
    }));
    slot_subs_.push_back(host_.on_slot(vc_api::run_exit, slot, [this](const void* p) {
        const auto* e = static_cast<const vc_api::RunExit*>(p);
        AgentCtx::AsyncRun* run = find_run(e->run_id);
        if (run == nullptr) return;  // 非本 turn 的 run（取消后的迟到孤儿）：静默
        run->exited = true;
        run->exit_code = e->exit_code;
        if (!subagent_) host_.emit(vc_api::run_exit, p);
        try_finish_tools();  // 收口门槛（子代理在途/decision 挂起）各 ctx 自查
    }));
    if (!subagent_) return;  // 主会话 LLM 事件走广播（slot=0）；子会话需要本槽订阅
    slot_subs_.push_back(host_.on_slot(vc_api::llm_done, slot, [this](const void* p) {
        on_done(*static_cast<const vc_api::LlmDone*>(p));
    }));
    slot_subs_.push_back(host_.on_slot(vc_api::llm_error, slot, [this](const void* p) {
        on_llm_error(*static_cast<const vc_api::LlmError*>(p));
    }));
    // 流式增量转工作记录视图（广播，落不了盘也不需要——实时过程归前端子代理视图）
    slot_subs_.push_back(host_.on_slot(vc_api::llm_delta, slot, [this](const void* p) {
        const auto* e = static_cast<const vc_api::LlmDelta*>(p);
        emit_subagent_(turn_no_, 1, nullptr, e->text != nullptr ? e->text : "");
    }));
}

// ---- turn 状态机 ------------------------------------------------------------

void AgentCtx::begin_turn(const std::string& text) {
    turn_no_ = next_turn_no();
    state_ = State::TurnActive;
    runs_.clear();
    compress_retries_ = 0;
    // 本槽事件订阅（先于 discover_tools——工具事件可能在本 turn 内即到）：
    // 上 turn 的槽订阅此时已无事件源，直接换订
    subscribe_turn_slot_();
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
    messages_.push_back(json{{"role", "user"}, {"content", text}});
    // mission 任务目标卡（上下文压缩阶段一）：turn 边界 = 重建点，整卡重写并
    // 固定在 system 之后——turn 内 append-only（缓存前缀纪律）。子代理是单任务
    // 短命上下文，任务卡与 user 消息逐字重复，纯耗 token
    if (!subagent_) rebuild_mission_();
    // 开轮即落盘（含本条 user 消息与任务卡）：持久化点原本只在 turn 收口/压缩
    // ——进程在 turn 中途死掉（崩溃/强杀/断电）时本条请求连同任务卡从历史蒸发，
    // 重放与恢复都不留痕，用户看到的是请求无声消失（踩过：子代理在途挂死 →
    // 重启 → 任务卡锚点整段跳过该请求，「继续」被接进上一个任务的语境）。
    // 恢复后未应答的 user 消息是合法序列，下轮正常续接。子代理不持久化
    // （session_path_ 为空，persist_session_ 自行早退）。
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
    json body = {{"messages", messages_}};
    if (!tools_.empty()) body["tools"] = tools_;
    // 子会话请求带路由键：provider 据此改走子档案（vc.llm.set_active 尾部字段）
    // 并把 LLM 事件按 slot 单播回发——不进广播通道，父会话天然不可见
    if (subagent_) body["slot"] = turn_no_;
    body_buf_ = body.dump();
    if (llm_ == nullptr || llm_->chat_ex == nullptr)
        return fail_turn("LLM 不可用");
    // 发送前预检（批次四-18/19）：请求体粗估超生效阈值 ⇒ 就地机械折叠后重建
    // 请求体再发——单 turn 几十轮工具往返在轮间主动瘦身（每轮 send_chat 都检），
    // 省一次 provider 400 往返；LLM 摘要仍只在 turn 边界。粗估宁高勿低：误折
    // 代价 = 上下文略瘦，可接受；折叠后旧信号置零待下次回报。子代理不压缩——
    // 折叠产物会破坏「报告 = 最后一条 assistant」的回灌语义
    if (!subagent_ && session_threshold_ > 0 &&
        estimate_prompt_tokens(body_buf_) > session_threshold_) {
        if (const vcagent::CompressPlan plan = vcagent::plan_compress(messages_, !raw_dir_.empty()); plan.valid) {
            pending_plan_ = plan;
            replace_with_digest_(plan.digest, "发送前预检");
            pending_plan_ = {};
            last_prompt_tokens_ = 0;   // 旧信号已失效（上下文已折叠），待下次回报
            json nb = {{"messages", messages_}};
            if (!tools_.empty()) nb["tools"] = tools_;
            body_buf_ = nb.dump();
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
            // 单条分发异常兜底：异常若穿出本循环，本 ctx 状态机会停在抛点
            // ⇒ send_chat 永不执行 ⇒ turn 悬停无终态（内核 ABI 边界只负责
            // 隔离订阅者与落日志，救不了本状态机）。转成错误结果回灌，LLM
            // 据此自修或换路。
            ctx.result = json{{"ok", false},
                              {"error", std::string("工具执行异常: ") + e.what()}}
                             .dump();
            ctx.done = true;
            emit_tool_result(ctx.full_name, false, ctx.result);
            ++dispatched_;
            excepted = true;
        } catch (...) {
            ctx.result = json{{"ok", false}, {"error", "工具执行异常（未知类型）"}}.dump();
            ctx.done = true;
            emit_tool_result(ctx.full_name, false, ctx.result);
            ++dispatched_;
            excepted = true;
        }
        if (excepted)
            host_.log(3, "ithaca.agent: 工具分发异常已兜底 turn #" +
                             std::to_string(turn_no_) + " tool=" + ctx.full_name);
        if (suspend) return;   // 挂起即返：跳过循环尾（旧 return 语义，见 dispatch_one_）
    }
    // 分发完但仍未收口 ⇒ 看在途面：无任何挂起就地收口；有异步任务/在途子代理
    // 则等归流回调（含 run_exit 在审批窗口内提前到达的场景 时机修正）
    if (runs_.empty() && child_pends_.empty()) return append_results_and_continue();
    if (all_runs_exited_() && child_pends_.empty()) return try_finish_tools();
    const int pending = static_cast<int>(runs_.size() + child_pends_.size());
    notice_meta("等待 " + std::to_string(pending) + " 个后台任务/子代理完成…",
                "waiting", 0, pending);
}

// 单条调用分发（dispatch_loop 的 try 保护体）：emit 调用卡 → 内置工具拦截 →
// 工具表执行/提案挂起。异常由调用方兜底转错误结果回灌。
bool AgentCtx::dispatch_one_(ToolCallCtx& ctx) {
    emit_tool_call(ctx.full_name, ctx.args);
    // 内置工具：context.recall（外置存储回查）——不经工具表，直接处理
    if (ctx.full_name == vcagent::kRecallTool) {
        ctx.result = exec_recall(ctx.args);
        ctx.done = true;
        emit_tool_result(ctx.full_name, true, ctx.result);
        ++dispatched_;
        return false;
    }
    // 内置工具：task（子代理派发）——不经工具表。spawn 失败 = 护栏/参数拒绝
    // （result 已写好，就地 emit 回灌）；成功 = 在途登记（begin_turn 同步失败
    // 的路径已在回调帧内收口，结果同帧写回）
    if (ctx.full_name == vcagent::kTaskTool) {
        if (!spawn_child_(dispatched_, ctx)) {
            ctx.done = true;
            emit_tool_result(ctx.full_name, false, ctx.result);
        }
        ++dispatched_;
        return false;
    }
    const Bound* b = find_bound(ctx.full_name);
    if (b == nullptr) {
        ctx.result = json{{"ok", false}, {"error", "未知工具: " + ctx.full_name}}.dump();
        ctx.done = true;
        emit_tool_result(ctx.full_name, false, ctx.result);
        ++dispatched_;
        return false;
    }
    if (vcagent::needs_confirm(ctx.full_name, confirm_) && !auto_all_) {
        // 泛化试算 + 提案（先置状态再 emit——嵌套派发会在本帧内回到
        // on_decision，状态没就位会被自己的守卫丢弃——M3 踩过）。
        json dry_args = ctx.args;
        dry_args["dry_run"] = true;
        const std::string dry_out = exec(*b, ctx.tool, dry_args);
        pending_.tool = ctx.full_name;
        pending_.index = dispatched_;
        pending_.args = ctx.args;
        state_ = State::AwaitingDecision;
        // diff_text 只装行级 diff（原始结果 JSON 不可读——webui 截图自审抓到）；
        // 同文件多 op 已在 workspace 折叠，edits>1 时标注「N 处编辑」
        std::string diff_text;
        json dry = json::parse(dry_out, nullptr, false);
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
            return true;   // 回灌已发车：挂起即返，循环尾不得再跑（旧 return 语义）
        }
        // runner 类提案：would_run 单行卡（命令批准卡形态）
        if (diff_text.empty() && dry.is_object() && dry.contains("would_run") &&
            dry["would_run"].is_string())
            diff_text = "── " + ctx.full_name + "（命令批准）\n" +
                        dry["would_run"].get<std::string>() + "\n";
        if (diff_text.empty()) diff_text = dry_out;
        nostos::kit::Scratch prop_scratch;
        const vc_api::EditProposed proposal{
            turn_no_, diff_text.c_str(), prop_scratch.hold(ctx.full_name)};
        host_.emit(vc_api::edit_proposed, &proposal);
        return true;  // ← 死锁纪律：绝不同步等待；挂起即返（旧 return 语义）
    }
    ctx.result = exec(*b, ctx.tool, ctx.args);
    ctx.done = true;
    emit_tool_result(ctx.full_name, tool_ok(ctx.result), ctx.result);
    ++dispatched_;
    // 异步长任务（runner.run）：登记 run_id——结果等 vc.run.exit 归流，
    // 本条消息的其余工具照常分发（runner 侧并发、run_id 归流）。
    if (const int rid = parse_run_id(ctx.result); rid > 0) {
        register_run(rid, dispatched_ - 1);
        notice_meta("后台任务 run#" + std::to_string(rid) + " 已启动",
                    "bg_run_started", rid);
    }
    return false;
}

// task 工具：校验参数与护栏 → 起子 ctx（免批/静默/受限工具面）事件驱动跑循环
// → 登记在途。返回 false = 拒绝（result 已写好，调用方 emit 后继续分发）。
// 在途登记先于 begin_turn：begin_turn 同步失败（无工具表/LLM 不可用）会在本帧
// 内经 on_finished_ 回调走 on_child_finished_ 收口，登记晚了就丢挂起。
bool AgentCtx::spawn_child_(std::size_t index, ToolCallCtx& call) {
    const std::string desc = call.args.value("description", "");
    const std::string prompt = call.args.value("prompt", "");
    const std::string type_name = call.args.value("subagent_type", "general");
    if (prompt.empty() || desc.empty()) {
        call.result =
            json{{"ok", false}, {"error", "task 缺少必填参数（description / prompt）"}}.dump();
        return false;
    }
    if (children_.size() >= kMaxChildren) {
        call.result = json{{"ok", false},
                           {"error", "并发子代理已达上限（" + std::to_string(kMaxChildren) +
                                         "），等在途任务完成后再派发"}}.dump();
        return false;
    }
    const AgentType* type = nullptr;
    for (const auto& t : types_)
        if (t.name == type_name) {
            type = &t;
            break;
        }
    if (type == nullptr) {
        std::string known;
        for (const auto& t : types_) {
            if (!known.empty()) known += " / ";
            known += t.name;
        }
        call.result = json{{"ok", false},
                           {"error", "未知子代理类型: " + type_name + "（可用: " + known + "）"}}
                          .dump();
        return false;
    }
    // 工具面：类型白名单（工具集名）过滤父清单；空 = 继承父全量
    std::vector<std::string> svcs;
    if (type->tools.empty()) {
        svcs = tool_svcs_;
    } else {
        for (const auto& want : type->tools)
            for (const auto& s : tool_svcs_)
                if (vcagent::toolset_of_svc(s) == want) {
                    svcs.push_back(s);
                    break;
                }
        if (svcs.empty()) {
            call.result = json{{"ok", false},
                               {"error", "类型 " + type_name +
                                             " 的工具白名单未命中任何可用工具集"}}.dump();
            return false;
        }
    }
    auto child = std::make_unique<AgentCtx>();
    child->host_ = host_;
    child->subagent_ = true;       // 免批 + 静默 + 不压缩不持久化 + 请求体带标志
    child->depth_ = depth_ + 1;
    child->system_prompt_ = type->prompt;
    child->tool_svcs_ = std::move(svcs);
    child->auto_all_ = true;       // 子代理免批：授权由主会话批准链路负责（task 卡用户可见）
    child->confirm_.clear();
    child->raw_dir_ = raw_dir_;      // 共享外置目录（R3：归档名带会话 stem 前缀）
    child->raw_prefix_ = raw_prefix_; // 前缀随目录走——子代理归档与父会话同命名空间
    AgentCtx* raw = child.get();
    child->on_finished_ = [this, index, raw](bool ok, const std::string& report, int turns) {
        on_child_finished_(index, raw, ok, report, turns);
    };
    children_.push_back(std::move(child));
    child_pends_.insert(index);
    raw->begin_turn(prompt);
    if (child_pends_.count(index) != 0)
        notice("子代理已派出 [" + type->name + "] " + desc);
    // 工作记录视图（落 transcript 不进上下文）：spawn 卡 + 主会话挂起条的数据源。
    // turn_id 带子的（与子 ctx 后续所有记录同号）——UI 按 turn_id 分组，带父号
    // 会让 spawn 卡掉进别的组、右栏点开是空（踩过：重启撞号后新 spawn 被旧
    // 完成卡标记为「已结束」）
    emit_subagent_(raw->turn_no_, 0, type->name.c_str(),
                   desc.empty() ? prompt : desc + "——" + trim(prompt, 120));
    host_.log(2, "ithaca.agent: 子代理 #" + std::to_string(raw->turn_no_) + " 已派出（" +
                     type->name + "）: " + desc);
    return true;
}

// 子代理完成回调（宿主线程；可能在子事件处理栈内同步执行）。收口门槛与
// try_finish_tools 同一门：全部 call 完成 + 无在途子 + 无在途 run。
void AgentCtx::on_child_finished_(std::size_t index, AgentCtx* child, bool ok,
                                  const std::string& report, int turns) {
    child_pends_.erase(index);
    // 父已收口（取消/失败级联）：子现场已清，迟到回调只摘挂起不写回
    if (state_ != State::TurnActive) return;
    try {
        // 子用量并入会话累计（计量口径同 on_done 的 usage 累计）
        usage_prompt_ += child->usage_prompt_;
        usage_completion_ += child->usage_completion_;
        usage_total_ += child->usage_total_;
        usage_cache_ += child->usage_cache_;
        ToolCallCtx& c = calls_[index];
        c.result = ok ? json{{"ok", true},
                             {"report", report},
                             {"turns", turns},
                             {"total_tokens", child->usage_total_}}
                            .dump()
                      : json{{"ok", false}, {"error", report}}.dump();
        c.done = true;
        emit_tool_result(c.full_name, ok, c.result);
        notice(ok ? "子代理完成（" + std::to_string(turns) + " 轮 · " +
                         std::to_string(child->usage_total_) + " tokens）"
                  : "子代理失败: " + trim(report, 120));
    } catch (...) {
        // 本回调运行在子代理的调用栈上：禁止 fail_turn/cleanup_turn_——那会析构
        // 栈上仍在执行的子 ctx（UB）。异常降级为本条 task 的错误结果走常规续段；
        // 续段原语（append/try_finish）自身已兜底终态
        host_.log(3, "ithaca.agent: 子代理完成回调异常，已降级为该任务的错误结果");
        if (index < calls_.size() && !calls_[index].done) {
            calls_[index].result =
                json{{"ok", false}, {"error", "子代理完成回调内部异常"}}.dump();
            calls_[index].done = true;
            emit_tool_result(calls_[index].full_name, false, calls_[index].result);
        }
    }
    // 其余子代理仍在途：等最后一个完成统一收口
    if (!child_pends_.empty()) return;
    // 同消息尚有未分发项：分发循环尾部收口（结果已写回，循环会跳过完成项）
    if (dispatched_ != calls_.size()) return;
    if (runs_.empty()) return append_results_and_continue();
    if (all_runs_exited_()) try_finish_tools();
}

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
    for (const auto& ctx : calls_) {
        std::string content = ctx.result;
        // 内容型工具内联豁免按工具分档（量级理由与实测依据见各 Cap 声明处）：
        // read 96KB（逐字素材一次进上下文）/ task 1KB（结论消费型，超限外置 +
        // 结论头）；其余工具超 kInlineResultCap 即外置
        const auto last_tool = tool_last(ctx.full_name);
        const bool content_inline =
            last_tool == "read" ? content.size() <= kContentInlineCap
          : last_tool == "task" ? content.size() <= kTaskInlineCap
          : false;
        if (content.size() > vcagent::kInlineResultCap && !content_inline) {
            if (tool_last(ctx.full_name) == "recall") {
                // recall 回包本身是 JSON——直接 trim 会劈在 content 字符串中间
                // 产生非法 JSON tool 消息（实地测试踩过：中文页 3B/字，4096 字节
                // 闸按字符数轻松撞上）。在 content 值内截断后重打包，保持合法 JSON
                const json rj = json::parse(content, nullptr, false);
                if (rj.is_object() && rj.contains("content") && rj["content"].is_string()) {
                    json fixed = rj;
                    fixed["content"] = vcws::trim_utf8(rj["content"].get<std::string>(), 1024) +
                                       "…（回查内容过长，已截断）";
                    content = fixed.dump();
                } else {
                    content = vcws::trim_utf8(content, 2048) +
                              "…（回查内容过长，已截断）";
                }
            } else {
                const std::string ref = spool_raw(ctx.full_name, content);
                if (!ref.empty()) {
                    const json r = json::parse(content, nullptr, false);
                    content = vcagent::compact_result(ctx.full_name, r, ref);
                } else {
                    content = vcws::trim_utf8(content, 2048) +
                              "…（超出内联上限且外置失败）";
                }
            }
        }
        messages_.push_back(json{{"role", "tool"},
                                 {"tool_call_id", ctx.call_id},
                                 {"content", content}});
    }
    send_chat();
}

// 原始工具结果外置：raw/<turn>_<seq>_<tool>.json；失败 ⇒ 空 ref（调用方内联截断）
std::string AgentCtx::spool_raw(const std::string& tool, const std::string& result) {
    if (raw_dir_.empty()) return {};
    // 序号进程级递增：主/子代理各有 turn_no_（都从 1 起）共享同一 raw/，实例内
    // 计数会撞名互相覆盖——全局计数 + 会话 stem 前缀双重隔离
    static std::atomic<unsigned long long> g_raw_seq{0};
    try {
        const std::string name = raw_prefix_ + "t" + std::to_string(turn_no_) + "_" +
                                 std::to_string(++g_raw_seq) + "_" +
                                 tool_last(tool) + ".json";
        std::ofstream f(fs::path(raw_dir_) / name, std::ios::binary | std::ios::trunc);
        if (!f) return {};
        f << result;
        return name;
    } catch (const std::exception&) {
        return {};
    }
}

// context.recall：ref 校验（纯文件名）→ 读归档 → provenance 包装；分页（批次三-8）：
// offset/limit 字节窗口（limit 缺省 2048、上限 4096 = 单条内联护栏），回包带
// total/next_offset/has_more——大归档分段读，不再"永远只看头 2KB"
std::string AgentCtx::exec_recall(const json& args) {
    const std::string ref = args.value("ref", "");
    if (ref.empty() || raw_dir_.empty())
        return json{{"ok", false},
                    {"error", "ref 为空，或本会话未启用外置归档（折叠历史不可回查）"}}
            .dump();
    if (ref.find('/') != std::string::npos || ref.find('\\') != std::string::npos ||
        ref.find("..") != std::string::npos)
        return json{{"ok", false}, {"error", "ref 必须是裸文件名"}}.dump();
    std::ifstream in(fs::path(raw_dir_) / ref, std::ios::binary);
    if (!in) return json{{"ok", false}, {"error", "归档不存在: " + ref}}.dump();
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    const long long total = static_cast<long long>(content.size());
    // 结构化回查：归档是工具结果的 JSON dump——entries 形态（list 归档）支持
    // filter（路径包含过滤）与 summary（按目录聚合计数），单页返回不切字节；
    // 其余形态回退下方字节窗口分页
    const bool summary = args.value("summary", false);
    const std::string filter = vcws::ascii_lower(args.value("filter", ""));
    if ((summary || !filter.empty()) && total > 0 && total < (8LL << 20)) {
        const json rj = json::parse(content, nullptr, false);
        if (rj.is_object() && rj.contains("entries") && rj["entries"].is_array()) {
            json filtered = json::array();
            std::size_t matched = 0;
            // 按 type 分类计数：目录条目冒充文件会让聚合数字说谎（实测：归档里
            // 的子目录条目被计成"(根) files:1"）；note（省略计数）是元数据，
            // 计入 matched 但不冒充文件/目录
            std::map<std::string, std::pair<long long, long long>> by_dir;   // 目录 → {文件数, 子目录数}
            for (const auto& e : rj["entries"]) {
                if (!e.is_object()) continue;
                const std::string p = e.value("path", "");
                if (!filter.empty() &&
                    vcws::ascii_lower(p).find(filter) == std::string::npos)
                    continue;
                ++matched;
                const std::size_t slash = p.rfind('/');
                const std::string bucket =
                    slash == std::string::npos ? "(根)" : p.substr(0, slash);
                const std::string type = e.value("type", "file");
                if (type == "dir") ++by_dir[bucket].second;
                else if (type == "file") ++by_dir[bucket].first;
                if (filtered.size() < 50) filtered.push_back(e);
            }
            json dirs = json::array();
            for (const auto& [d, c] : by_dir)
                dirs.push_back(json{{"dir", d}, {"files", c.first}, {"subdirs", c.second}});
            json summ = json{{"summary_of", ref},
                             {"total_entries", rj["entries"].size()},
                             {"matched", matched}};
            if (!filter.empty()) summ["matching"] = filtered;   // 前 50 条命中
            if (summary) summ["dirs"] = dirs;
            return vcagent::recall_result(ref, summ.dump(), total, 0, false, 0);
        }
        // 非 entries 形态：无结构可查，回退字节分页
    }
    long long offset = args.value("offset", 0LL);
    const long long limit = std::clamp(args.value("limit", 2048LL), 1LL, 4096LL);
    offset = std::clamp(offset, 0LL, total);
    // 双边界（起点对齐 + 尾部不劈码点）由 vcws::utf8_window 一次做完，产物直接
    // 进 recall_result 的 JSON——内容是 dump 产物（合法 UTF-8），窗口尾部自洽
    std::size_t off = static_cast<std::size_t>(offset);
    const std::string page =
        vcws::utf8_window(content, off, static_cast<std::size_t>(limit));
    const bool has_more = off + page.size() < static_cast<std::size_t>(total);
    return vcagent::recall_result(ref, page, total, static_cast<long long>(off),
                                  has_more,
                                  static_cast<long long>(off + page.size()));
}

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
// suggest 逐条问 / auto-edit 编辑免批 / full-auto 全自动（沙箱兜底在 runner）；
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

// ---- 上下文压缩阶段二 --------------

void AgentCtx::set_context_window_(long long w) {
    ctx_window_ = w;
    recompute_threshold_();   // 模型/供应商切换重置退避（窗口变了旧基数失效）
}

// 生效阈值（批次四-19）：W 与 max_output 已知 ⇒ W − max_output（输出预留——给
// 长 diff/长测试输出留位，避免贴着 0.9W 的请求被大响应顶爆）；仅 W 已知 ⇒
// 0.9×W（历史语义）；都未知 ⇒ 兜底阈值。下限 W/4：max_output 元数据异常巨大时
// 仍保住 25% 的 prompt 空间。
void AgentCtx::recompute_threshold_() {
    if (ctx_window_ <= 0) {
        session_threshold_ = compress_threshold_;
        return;
    }
    const long long reserved =
        ctx_max_output_ > 0 ? ctx_max_output_ : ctx_window_ / 10;
    const long long th = ctx_window_ - reserved;
    const long long floor_th = ctx_window_ / 4;
    session_threshold_ = th > floor_th ? th : floor_th;
}

// ---- 上下文压缩批次一 --------------------------------

// 硬保护线：0.95×W，*20/19 整数式避免除法截断（宁早勿晚）。W 未知 ⇒ 不启用
// ——判定交给会话阈值/兜底阈值路径。
bool AgentCtx::hard_window_() const {
    return ctx_window_ > 0 && last_prompt_tokens_ * 20 >= ctx_window_ * 19;
}

// 压缩判定共用入口：finish_turn 与 cleanup_turn_ 都走这里（失败/取消收口不漏
// 判定——否则反复取消/失败的长会话上下文只增不减）。计划不可折叠 ⇒ false，
// 调用方走原收口；若下一请求仍超窗，由 on_llm_error 的超窗自救接手。
bool AgentCtx::maybe_compress_() {
    if (subagent_) return false;  // 子代理总闸：单任务短命上下文，压缩语义（回 Idle/
                                  // 摘要异步收口/退避校验）全部不适用
    const bool hard = hard_window_();
    if (!hard && !vcagent::should_compress(last_prompt_tokens_, session_threshold_))
        return false;
    const vcagent::CompressPlan plan = vcagent::plan_compress(messages_, !raw_dir_.empty());
    if (!plan.valid) return false;
    if (!hard && summary_enabled_) {
        pending_plan_ = plan;
        request_digest_();  // 异步收口：摘要回包/失败在 on_done/on_llm_error 分流
        return true;
    }
    apply_digest_(plan.digest, false);  // apply_digest_ 内收口（回 Idle + flush 排队）
    return true;
}

// 摘要请求：无 tools 的单轮 chat_ex；素材 = 机械 digest 渲染（request 逐字 +
// calls 最小记录 + 全部 ref）——与兜底产物同源，摘要失败可直接落它。
void AgentCtx::request_digest_() {
    llm_ = host_.service<vc_api::LlmTable>(vc_api::llm);
    if (llm_ == nullptr || llm_->chat_ex == nullptr)
        return apply_digest_(pending_plan_.digest, false);
    compress_seq_ = next_chat_seq();
    compress_inflight_ = true;
    json body = {{"messages", json::array({
                                  json{{"role", "system"},
                                       {"content", std::string(vcagent::kSummarySystem)}},
                                  json{{"role", "user"},
                                       {"content", pending_plan_.digest.value("content", "")}}})}};
    body_buf_ = body.dump();   // 调用点均在无在途请求时（turn 收口后/请求已失败或
                               // 取消）——复用缓冲安全
    if (llm_->chat_ex(llm_->self, compress_seq_, body_buf_.c_str()) != NOSTOS_OK)
        apply_digest_(pending_plan_.digest, false);   // 未受理 ⇒ 机械兜底
}

void AgentCtx::on_compress_done_(const vc_api::LlmDone& e) {
    json a = json::parse(e.assistant_json != nullptr ? e.assistant_json : "{}", nullptr,
                         /*allow_exceptions=*/false);
    std::string s = a.is_object() && a.contains("content") && a["content"].is_string()
                        ? a["content"].get<std::string>() : std::string();
    if (s.empty()) return apply_digest_(pending_plan_.digest, false);
    // 超长截断（伤散文不伤回查链——外置索引由本函数在截断之后确定性重建）
    if (s.size() > 4096) s = vcws::trim_utf8(s, 4096) + "…";
    json digest = json{
        {"role", "system"},
        {"content", std::string(vcagent::kDigestMarker) +
                        "（历史 turn 已压缩：LLM 自总结。" +
                        std::string(vcagent::kDigestProvenance) +
                        // 口径与实际能力一致（与 plan_compress 头同源纪律）：
                        // raw 未启用时不得承诺 recall——模型会调必败工具
                        (raw_dir_.empty()
                             ? "本会话未启用外置归档，被折叠原文不可回查）\n"
                             : "工具原始结果外置于 raw/，可用 context_recall 回查）\n") +
                        s}};
    // 外置索引确定性重建（批次三-11）：ref 清单来自折叠计划的结构化记录（calls
    // 提取，非摘要散文、非文本扫描）——摘要怎么改写都不断回查链；旧「补齐 tail」
    // 机制退役（其 ref 对下轮扫描不可见的失真随索引方案消失）
    const std::string index = vcagent::index_section(pending_plan_.refs);
    if (!index.empty())
        digest["content"] = digest["content"].get<std::string>() + "\n" + index;
    apply_digest_(digest, true);
}

void AgentCtx::on_compress_error_(const vc_api::LlmError&) {
    apply_digest_(pending_plan_.digest, false);   // 摘要失败 ⇒ 机械兜底（计划里已备好）
}

// 折叠应用核心：区间替换 + 旧 digest 并入 + 持久化 + 通告。先插 digest 再跳过
// 的顺序纪律见阶段一（踩过：跳过分支先 continue 会漏掉插入，digest 整条丢失）。
void AgentCtx::replace_with_digest_(const json& digest_msg, const char* via) {
    // 折叠前原文归档（批次三-17）：cut 区间的完整消息（user/assistant 全文）写入
    // raw/——agent 视图丢掉的每一行都可 recall 回查；归档 ref 附进 digest 正文，
    // 模型在压缩后仍保有"被折叠历史"的入口
    json cut = json::array();
    for (std::size_t k = pending_plan_.cut_begin;
         k < pending_plan_.cut_end && k < messages_.size(); ++k)
        cut.push_back(messages_[k]);
    const std::string archive_ref = spool_raw("digest", cut.dump());
    json digest = digest_msg;
    if (!archive_ref.empty())
        digest["content"] = digest_msg.value("content", "") + "【折叠原文】" +
                            archive_ref + "（context_recall 可回查）\n";
    // 锚点保真在线观测（批次五-20）：折叠区逐字锚点在折叠产物中的覆盖率——
    // 32KB 降级会丢最旧段的调用链锚点，比率落 host log 供评测采集（批次五-12）
    if (pending_plan_.anchors_total > 0)
        host_.log(2, "ithaca.agent: 折叠锚点保真 " +
                         std::to_string(pending_plan_.anchors_total -
                                        pending_plan_.anchors_dropped) +
                         "/" + std::to_string(pending_plan_.anchors_total));
    const std::size_t before = messages_.size();
    json nm = json::array();
    bool digest_placed = false;
    for (std::size_t k = 0; k < messages_.size(); ++k) {
        // 先插 digest（到末段起点处——紧贴 mission 之后），再做跳过判断：
        // 跳过分支先 continue 会漏掉插入（踩过：digest 整条丢失）
        if (!digest_placed && k == pending_plan_.cut_begin) {
            nm.push_back(digest);
            digest_placed = true;
        }
        if (k >= pending_plan_.cut_begin && k < pending_plan_.cut_end) continue;
        if (k < pending_plan_.cut_begin && k > 0 && vcagent::is_digest(messages_[k]))
            continue;  // 旧 digest 已并入新 digest
        nm.push_back(messages_[k]);
    }
    if (!digest_placed) nm.push_back(digest);
    messages_ = nm;
    notice(std::string("上下文已压缩（") + via + "折叠：" +
           std::to_string(pending_plan_.segments_folded) + " 段历史，消息 " +
           std::to_string(before) + " → " + std::to_string(messages_.size()) +
           (raw_dir_.empty() ? "；本会话未启用外置归档" : "；原始结果在 raw/ 可 recall") +
           "）");
    persist_session_();
}

// 折叠应用共用出口（LLM 摘要 / 机械 digest）：核心替换 + 置退避校验 + 回 Idle
// + flush 排队输入。
void AgentCtx::apply_digest_(const json& digest_msg, bool via_llm) {
    replace_with_digest_(digest_msg, via_llm ? "LLM 自总结" : "机械");
    compress_inflight_ = false;
    compress_seq_ = -1;
    pending_plan_ = {};
    compress_verify_ = true;   // 下一 turn done 时校验是否压到位（没压下 ⇒ 阈值退避）
    state_ = State::Idle;
    flush_queued_inputs_();
}

// 超窗就地自救：provider 报上下文超长 ⇒ 按冻结计划机械折叠后
// 重发当前请求。与 apply_digest_ 的分野：turn 仍在途——保持 state_/排队不动、
// 不置 compress_verify_（退避校验是 turn 边界折叠的语义）；重发次数由调用方
// （on_llm_error）以 kMaxOverwindowResend 封顶。
void AgentCtx::forced_fold_resend_() {
    replace_with_digest_(pending_plan_.digest, "超窗强制");
    pending_plan_ = {};
    send_chat();
}

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
    const vc_api::LlmError e{0, NOSTOS_ERR_FAILED, msg.c_str()};
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
void AgentCtx::cleanup_turn_() {
    // 先清悬挂工具尾巴再持久化：异常/取消收口时 messages_ 可能停在分发中途，
    // 带 tool_calls 的 assistant 没有全部应答就落盘，下次请求即 provider 400
    // 且每轮复现成错误循环
    truncate_dangling_tool_exchange_();
    // 子代理级联收场（取消/失败同走此门）：在途子代理停掉并随现场销毁——它们
    // 是本 turn 的派生，父已终态则子无意义。子自己派的后台进程不追杀（runner
    // 自有超时），log 留痕。置 inflight_=-1 防 provider 迟到的取消回包误入路由。
    // NOTE: 每个被杀子代理补发 phase-5 终态卡——回调已被禁用（父现场正被清），
    // 不补发则 transcript 永无该子代理的终态记录，spawn 卡在重放与实时里永远
    // 「进行中」（踩过：取消后卡片挂进行中，刷新也复活不了）
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

// 排队输入 flush：队首开新 turn，其余等该 turn 收口时再取（栈深有界——不递归
// 弹空整队）。
void AgentCtx::flush_queued_inputs_() {
    if (queued_.empty()) return;
    const std::string next = queued_.front();
    queued_.pop_front();
    begin_turn(next);
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
