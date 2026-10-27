// 事件处理器（plugin/agent 的状态机入口切片）。
// 全部在宿主线程执行（禁止同步等待——死锁纪律见 include/agent_plugin.hpp）：
// on_input 排队/开 turn；on_done 记账 tool_calls 并驱动分发；on_decision 执行
// 审批结果（含逐文件批准、取消）；on_llm_error 仅复位状态机。

#include "agent_plugin.hpp"

#include <set>  // std::set：逐文件批准的接受路径集合

namespace vc_agent {

// ---- 事件处理器（全部在宿主线程；禁止同步等待）-----------------------------
// 入口统一形态：try/catch 包体 → fail_unexpected_ 收口（语义见 agent_plugin.hpp）；
// 具体逻辑在 *_impl。

void AgentCtx::on_input(const char* text) {
    try {
        on_input_impl(text);
    } catch (...) {
        fail_unexpected_();
    }
}

void AgentCtx::on_input_impl(const char* text) {
    if (state_ != State::Idle) {
        queued_.push_back(text != nullptr ? text : "");
        notice("当前 turn 进行中，输入已排队（队列 " + std::to_string(queued_.size()) + "）");
        return;
    }
    begin_turn(text != nullptr ? text : "");
}

void AgentCtx::on_done(const vc_api::LlmDone& e) {
    try {
        on_done_impl(e);
    } catch (...) {
        fail_unexpected_();
    }
}

void AgentCtx::on_done_impl(const vc_api::LlmDone& e) {
    // 摘要请求回包分流（阶段二）：与 turn 请求共用 chat_ex 通道，按 request_id
    // 区分——摘要在途时 turn 请求不可能在途（摘要只在 turn 收口后发起）。
    if (compress_inflight_ && e.request_id == compress_seq_)
        return on_compress_done_(e);
    if (e.request_id != inflight_) return;  // 非在途请求的迟到事件：忽略
    json assistant = json::parse(e.assistant_json != nullptr ? e.assistant_json : "{}",
                                 nullptr, /*allow_exceptions=*/false);
    if (assistant.is_discarded() || !assistant.is_object())
        assistant = json{{"role", "assistant"}, {"content", ""}};
    messages_.push_back(assistant);  // 完整消息（含 tool_calls）进历史

    // token 计量与判定信号：usage 真实回报 ⇒ 累计（含缓存命中计量）+ 信号取真值；未回报 ⇒ 信号按请求体粗估兜底（宁高勿低）——
    // 粗估不进用量累计。两种来源都写入 last_prompt_tokens_，压缩判定不再失明。
    bool usage_real = false;
    if (e.usage != nullptr) {
        json u = json::parse(e.usage, nullptr, false);
        if (u.is_object()) {
            usage_prompt_ += u.value("prompt_tokens", 0LL);
            usage_completion_ += u.value("completion_tokens", 0LL);
            usage_total_ += u.value("total_tokens", 0LL);
            usage_cache_ += u.value("cache_tokens", 0LL);   // 缓存命中计量
            last_prompt_tokens_ = u.value("prompt_tokens", 0LL);  // 压缩判定的确定性信号
            usage_real = true;
        }
    }
    if (!usage_real) last_prompt_tokens_ = estimate_prompt_tokens(body_buf_);
    // 压缩后首 turn 的退避校验（阶段二）：摘要没压下来 ⇒ 阈值 ×1.25 抬升；封顶
    // 0.95×W——越过即由硬保护接管判定，阈值不再虚高（阈值一旦
    // 越过 W，should_compress 永不命中，压缩停摆回到爆窗）。仅真实 usage 参与
    // 比较——拿粗估信号退避 = 用垃圾调阈值。
    if (compress_verify_) {
        compress_verify_ = false;
        if (usage_real && session_threshold_ > 0 &&
            last_prompt_tokens_ >= session_threshold_) {
            session_threshold_ = session_threshold_ / 4 * 5;
            if (ctx_window_ > 0) {
                const long long cap = ctx_window_ / 20 * 19;
                if (session_threshold_ > cap) session_threshold_ = cap;
            }
            notice("上下文仍紧张——压缩阈值已上调至 " +
                   std::to_string(session_threshold_) + " tokens，建议开新会话");
        }
    }

    // turn 的结束条件 = assistant 消息**没有 tool_calls**（最终回答）；
    // 有 tool_calls ⇒ 逐个分发、结果回灌、再调 LLM——循环而非终结。
    calls_.clear();
    runs_.clear();
    dispatched_ = 0;
    if (assistant.contains("tool_calls") && assistant["tool_calls"].is_array()) {
        for (const auto& tc : assistant["tool_calls"]) {
            ToolCallCtx c;
            c.call_id = tc.value("id", std::string{});
            if (tc.contains("function") && tc["function"].is_object()) {
                c.full_name = tc["function"].value("name", std::string{});
                c.tool = tool_last(c.full_name);  // "workspace.read" → "read"（踩过：漏设 ⇒ 空工具名）
                const std::string a = tc["function"].value("arguments", std::string{});
                json parsed = json::parse(a, nullptr, false);
                if (parsed.is_object()) c.args = parsed;
            }
            calls_.push_back(std::move(c));
        }
    }
    if (calls_.empty()) return finish_turn();
    dispatch_loop();
}

void AgentCtx::on_llm_error(const vc_api::LlmError& e) {
    try {
        on_llm_error_impl(e);
    } catch (...) {
        fail_unexpected_();
    }
}

void AgentCtx::on_llm_error_impl(const vc_api::LlmError& e) {
    // 摘要请求失败分流（阶段二）：回退机械 digest——压缩永不卡死 turn 流
    if (compress_inflight_ && e.request_id == compress_seq_)
        return on_compress_error_(e);
    if (e.request_id != inflight_) return;  // 迟到错误 / agent 自发收口（rid=0）：忽略
    if (state_ == State::Idle) return;
    // 超窗自救：provider 报上下文超长 ⇒ 机械折叠后重发当前请求，
    // 而非报废 turn。仅 TurnActive 且无异步任务在途（错误对应的正是刚发出的
    // send_chat）；重发有 kMaxOverwindowResend 上界——折叠不够再超窗时最终仍走
    // 报废路径，不会打转。provider 文案归一（待办 15）后可收窄这里的匹配面。
    // 子代理不参与：折叠产物会破坏「报告 = 最后一条 assistant」的回灌语义
    if (!subagent_ && state_ == State::TurnActive && runs_.empty() &&
        compress_retries_ < kMaxOverwindowResend &&
        vcagent::is_context_overflow_error(e.message != nullptr ? e.message : "")) {
        const vcagent::CompressPlan plan = vcagent::plan_compress(messages_, !raw_dir_.empty());
        if (plan.valid) {
            ++compress_retries_;
            pending_plan_ = plan;
            forced_fold_resend_();
            return;
        }
    }
    // provider 的 llm_error 已由 webui 落错误卡并清 in_flight——agent 只复位
    // 状态机，不重发（重发 = 双重错误卡）。子代理的失败必须经完成回调回灌父
    // （fail_turn 静默分支），否则父的在途挂起永不归流
    if (subagent_)
        return fail_turn(e.message != nullptr ? e.message : "LLM 请求失败");
    cleanup_turn_();
}

void AgentCtx::on_decision(const vc_api::TurnDecision& d) {
    try {
        on_decision_impl(d);
    } catch (...) {
        fail_unexpected_();
    }
}

void AgentCtx::on_decision_impl(const vc_api::TurnDecision& d) {
    if (d.decision == 3) {  // undo：独立命令，不影响等待中的提案
        exec_undo();
        return;
    }
    if (d.decision == 4) {  // 取消当前 turn（Ctrl+C / /cancel；turn_id 忽略）
        if (state_ == State::Idle) return;   // 空转：无反馈即无操作（UI 已自行解锁）
        llm_ = host_.service<vc_api::LlmTable>(vc_api::llm);
        if (llm_ != nullptr && llm_->cancel != nullptr)
            llm_->cancel(llm_->self, inflight_);  // provider 静默终止在途请求（取消不是错误）
        // 收口事件由 agent 统一发（provider 各家都已取消静默）：transcript 由此
        // 有终态记录，重放不会把「进行中」复活；webui 已在 d=4 时先行解锁
        const std::string msg = "turn #" + std::to_string(turn_no_) + " 已被用户取消";
        const vc_api::LlmError e{0, NOSTOS_ERR_FAILED, msg.c_str()};
        host_.emit(vc_api::llm_error, &e);
        cleanup_turn_();
        return;
    }
    if (state_ != State::AwaitingDecision) return;

    if (d.decision == 2) auto_all_ = true;  // 本会话后续免批
    const bool approved = d.decision == 0 || d.decision == 2;
    ToolCallCtx& ctx = calls_[pending_.index];
    if (!approved) {
        // 拒绝反馈：用户留言随拒绝回灌，agent 据此调整
        std::string deny = "用户拒绝该操作";
        if (d.msg != nullptr && *d.msg != '\0')
            deny += std::string("：") + d.msg;
        ctx.result = json{{"ok", false}, {"error", deny}}.dump();
        ctx.done = true;
        ++dispatched_;
        emit_tool_result(ctx.full_name, false, ctx.result);
    } else {
        // 逐文件批准：paths_json 非空 ⇒ 只保留接受路径的 ops 落盘
        if (d.paths_json != nullptr && *d.paths_json != '\0') {
            try {
                const json keep = json::parse(std::string(d.paths_json), nullptr, true);
                if (keep.is_array() && ctx.args.is_object() &&
                    ctx.args.contains("ops") && ctx.args["ops"].is_array()) {
                    std::set<std::string> acc;
                    for (const auto& pth : keep)
                        if (pth.is_string()) acc.insert(pth.get<std::string>());
                    json kept = json::array();
                    for (const auto& o : ctx.args["ops"])
                        if (o.is_object() && acc.count(o.value("path", std::string())) > 0)
                            kept.push_back(o);
                    ctx.args["ops"] = kept;
                }
            } catch (const std::exception&) {
                // paths_json 损坏 ⇒ 按全量处理（不因 UI 层故障阻塞落盘）
            }
        }
        const Bound* b = find_bound(ctx.full_name);
        ctx.result = b ? exec(*b, ctx.tool, ctx.args)
                        : json{{"ok", false}, {"error", "工具表已消失"}}.dump();
        ctx.done = true;
        ++dispatched_;
        emit_tool_result(ctx.full_name, tool_ok(ctx.result), ctx.result);
        if (const int rid = parse_run_id(ctx.result); rid > 0)
            register_run(rid, pending_.index);
    }
    state_ = State::TurnActive;
    dispatch_loop();
}

}  // namespace vc_agent
