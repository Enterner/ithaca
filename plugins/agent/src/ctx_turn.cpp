// turn 状态机（plugin/agent 的核心切片）。
// begin_turn 开轮 → dispatch_loop 分发工具（确认类挂起提案/异步登记归流/同步直执）
// → append_results_and_continue 按序回灌并进下一轮 → finish_turn 收口（迟滞执行
// 上下文压缩）。失败/取消的统一收口 fail_turn/cleanup_turn_ 也在本文件。

#include "agent_plugin.hpp"

#include <fstream>  // std::ofstream：raw 外置归档

namespace vc_agent {

namespace {

constexpr char kSystemPrompt[] =
    "你是 Ithaca——一个运行在终端里的编程代理。用户用自然语言下达任务，"
    "你通过 tools 列出的工具读写工作区文件、执行构建。规则："
    "1) 编辑直接用 apply 提交（write=整文件 / replace=精确块，find 默认必须恰好"
    "命中一次，replace_all=true 全部替换）；系统会校验、落盘并把 diff 展示给用户，"
    "用户可随时撤销；"
    "2) 需要批准的操作（如运行命令）由宿主把关，被拒绝时把拒绝当作工具结果继续任务；"
    "3) 命令类工具是异步的：提交后先返回 run_id，构建/测试的输出与退出码随后"
    "作为工具结果回给你，读错误输出自我修复；"
    "4) 工具失败时读错误信息自我纠正；"
    "5) Python 依赖一律装进工作区内的虚拟环境（python -m venv .venv，之后用"
    " .venv 里的 python/-m pip 安装与运行）——完全访问模式下工作区外不可写，"
    "装到用户级目录会失败或不可见；"
    "6) 用中文简明汇报。";

}  // namespace

// ---- turn 状态机 ------------------------------------------------------------

void AgentCtx::begin_turn(const std::string& text) {
    turn_no_ = next_turn_no();
    state_ = State::TurnActive;
    runs_.clear();
    if (!discover_tools())
        return fail_turn("没有可用的工具表（检查 ITHACA_AGENT_TOOLS）");
    // M6 session：恢复的会话保留历史（含 system），只在空会话时注入 system
    if (messages_.empty())
        messages_.push_back(json{{"role", "system"}, {"content", kSystemPrompt}});
    messages_.push_back(json{{"role", "user"}, {"content", text}});
    // mission 任务目标卡（上下文压缩阶段一）：turn 边界 = 重建点，整卡重写并
    // 固定在 system 之后——turn 内 append-only（缓存前缀纪律）
    rebuild_mission_();
    send_chat();
}

void AgentCtx::send_chat() {
    // provider 每轮重新解析：同名服务被竞争顶掉时自然切换，agent 无感
    llm_ = host_.service<vc_api::LlmTable>(vc_api::llm);
    inflight_ = next_chat_seq();
    json body = {{"messages", messages_}};
    if (!tools_.empty()) body["tools"] = tools_;
    body_buf_ = body.dump();
    if (llm_ == nullptr || llm_->chat_ex == nullptr)
        return fail_turn("LLM 不可用");
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
        emit_tool_call(ctx.full_name, ctx.args);
        // 内置工具：context.recall（外置存储回查）——不经工具表，直接处理
        if (ctx.full_name == vcagent::kRecallTool) {
            ctx.result = exec_recall(ctx.args);
            ctx.done = true;
            emit_tool_result(ctx.full_name, true, ctx.result);
            ++dispatched_;
            continue;
        }
        const Bound* b = find_bound(ctx.full_name);
        if (b == nullptr) {
            ctx.result = json{{"ok", false}, {"error", "未知工具: " + ctx.full_name}}.dump();
            ctx.done = true;
            emit_tool_result(ctx.full_name, false, ctx.result);
            ++dispatched_;
            continue;
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
                return;
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
            return;  // ← 死锁纪律：绝不同步等待
        }
        ctx.result = exec(*b, ctx.tool, ctx.args);
        ctx.done = true;
        emit_tool_result(ctx.full_name, tool_ok(ctx.result), ctx.result);
        ++dispatched_;
        // 异步长任务（runner.run）：登记 run_id——结果等 vc.run.exit 归流，
        // 本条消息的其余工具照常分发（runner 侧并发、run_id 归流）。
        if (const int rid = parse_run_id(ctx.result); rid > 0) {
            runs_[rid] = AsyncRun{dispatched_ - 1, "", false, 0};
            notice("后台任务 run#" + std::to_string(rid) + " 已启动");
        }
    }
    // 分发完但仍未收口 ⇒ 看异步任务：全部已退出（含 run_exit 在审批窗口内
    // 提前到达的场景 时机修正）就地收口；否则等归流
    if (runs_.empty()) return append_results_and_continue();
    bool all_exited = true;
    for (const auto& [rid, run] : runs_)
        if (!run.exited) all_exited = false;
    if (all_exited) return try_finish_tools();
    notice("等待 " + std::to_string(runs_.size()) + " 个后台任务完成…");
}

// 全部工具调用完成 ⇒ 结果按序回灌 ⇒ 下一轮 LLM。
// 上下文压缩阶段一：超内联上限（4KB）的成功/失败结果外置到 raw/，消息里只留
// 确定性模板摘要 + ref（锚点逐字节保留；失败 2KB 内保留详细错误，ref 兜底全量）
void AgentCtx::append_results_and_continue() {
    for (const auto& ctx : calls_) {
        std::string content = ctx.result;
        if (content.size() > vcagent::kInlineResultCap) {
            if (tool_last(ctx.full_name) == "recall") {
                // recall 的 content 本身就是外置归档：直接截断兜底即可
                content = vcws::trim_utf8(content, 2048) + "…（回查内容过长，已截断）";
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
    try {
        const std::string name = "t" + std::to_string(turn_no_) + "_" +
                                 std::to_string(++raw_seq_) + "_" +
                                 tool_last(tool) + ".json";
        std::ofstream f(fs::path(raw_dir_) / name, std::ios::binary | std::ios::trunc);
        if (!f) return {};
        f << result;
        return name;
    } catch (const std::exception&) {
        return {};
    }
}

// context.recall：ref 校验（纯文件名）→ 读归档 → provenance 包装
std::string AgentCtx::exec_recall(const json& args) {
    const std::string ref = args.value("ref", "");
    if (ref.empty() || raw_dir_.empty())
        return json{{"ok", false},
                    {"error", "ref 为空或本会话未启用外置存储（无 ITHACA_SESSION_FILE）"}}
            .dump();
    if (ref.find('/') != std::string::npos || ref.find('\\') != std::string::npos ||
        ref.find("..") != std::string::npos)
        return json{{"ok", false}, {"error", "ref 必须是裸文件名"}}.dump();
    std::ifstream in(fs::path(raw_dir_) / ref, std::ios::binary);
    if (!in) return json{{"ok", false}, {"error", "归档不存在: " + ref}}.dump();
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    return vcagent::recall_result(ref, content);
}

// run.exit 到达后：所有分发完成且全部 run 归流 ⇒ 收口（正在等 decision 时不收口，
// 收口延迟到 decision 恢复后的 dispatch_loop 的时机修正）
void AgentCtx::try_finish_tools() {
    if (state_ != State::TurnActive) return;
    if (dispatched_ != calls_.size()) return;
    if (runs_.empty()) return;  // 无异步任务：常规收口归 dispatch_loop，防双重 send_chat
    for (const auto& [rid, run] : runs_)
        if (!run.exited) return;
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

// 确认清单 = 权限模式映射：suggest 逐条问 / auto-edit 编辑免批 /
// full-auto 全自动（沙箱兜底在 runner）
void AgentCtx::rebuild_confirm_() {
    if (mode_ == "suggest") confirm_ = {"workspace.apply", "runner.run"};
    else if (mode_ == "full-auto") confirm_.clear();
    else confirm_ = {"runner.run"};
}

void AgentCtx::finish_turn() {
    // turn 完成只发 host log，不发 notice——webui 对话流不需要；token 统计
    // 已随 assistant 记录回传
    host_.log(2, "ithaca.agent: turn #" + std::to_string(turn_no_) + " 完成（tokens total " +
                     std::to_string(usage_total_) + "）");
    persist_session_();
    // 上下文压缩（迟滞批量执行，方案 §3.4B）：判定实时、执行只在 turn 边界；
    // 折叠区间 = 除最末段外的全部连续历史（含旧 digest 并入新 digest），执行后
    // 立即落盘。阶段三的覆盖规则/锚点白名单细化等测量后再上。
    if (vcagent::should_compress(last_prompt_tokens_, compress_threshold_)) {
        const vcagent::CompressPlan plan = vcagent::plan_compress(messages_);
        if (plan.valid) {
            const std::size_t before = messages_.size();
            json nm = json::array();
            bool digest_placed = false;
            for (std::size_t k = 0; k < messages_.size(); ++k) {
                // 先插 digest（到末段起点处——紧贴 mission 之后），再做跳过判断：
                // 跳过分支先 continue 会漏掉插入（踩过：digest 整条丢失）
                if (!digest_placed && k == plan.cut_begin) {
                    nm.push_back(plan.digest);
                    digest_placed = true;
                }
                if (k >= plan.cut_begin && k < plan.cut_end) continue;
                if (k < plan.cut_begin && k > 0 && vcagent::is_digest(messages_[k]))
                    continue;  // 旧 digest 已并入新 digest
                nm.push_back(messages_[k]);
            }
            if (!digest_placed) nm.push_back(plan.digest);
            messages_ = nm;
            notice("上下文已压缩：折叠 " + std::to_string(plan.segments_folded) +
                   " 段历史（消息 " + std::to_string(before) + " → " +
                   std::to_string(messages_.size()) + "；原始结果在 raw/ 可 recall）");
            persist_session_();
        }
    }
    state_ = State::Idle;
    if (!queued_.empty()) {
        const std::string next = queued_.front();
        queued_.pop_front();
        begin_turn(next);  // 排队的输入依次开新 turn（栈深有界）
    }
}

// agent 内部失败的统一收口（没有工具表 / LLM 不可用 / LLM 未受理等）：发
// llm_error——webui 由此清 in_flight + 落错误卡，transcript 有终态记录（重放
// 安全）。request_id=0 是「agent 自发」哨兵：自己订阅的 on_llm_error 靠
// request_id != inflight_ 忽略它，否则同步派发重入 cleanup 会把排队输入排空两次。
void AgentCtx::fail_turn(const std::string& why) {
    const std::string msg = "turn #" + std::to_string(turn_no_) + " 失败: " + why;
    const vc_api::LlmError e{0, NOSTOS_ERR_FAILED, msg.c_str()};
    host_.emit(vc_api::llm_error, &e);
    cleanup_turn_();
}

// 状态机复位（不含对外事件——收口事件由调用方决定发不发）：持久化 + 回
// Idle + 清工具现场 + 排队输入依次开新 turn（栈深有界）。
void AgentCtx::cleanup_turn_() {
    persist_session_();
    state_ = State::Idle;
    calls_.clear();
    runs_.clear();
    if (!queued_.empty()) {
        const std::string next = queued_.front();
        queued_.pop_front();
        begin_turn(next);
    }
}

}  // namespace vc_agent
