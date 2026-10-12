// 事件处理器（plugin/agent 的状态机入口切片）。
// 全部在宿主线程执行（禁止同步等待——死锁纪律见 include/agent_plugin.hpp）：
// on_input 排队/开 turn；on_done 记账 tool_calls 并驱动分发；on_decision 执行
// 审批结果（含逐文件批准、取消）；on_llm_error 仅复位状态机。

#include "agent_plugin.hpp"

#include <set>  // std::set：逐文件批准的接受路径集合

namespace vc_agent {

// ---- 事件处理器（全部在宿主线程；禁止同步等待）-----------------------------

void AgentCtx::on_input(const char* text) {
    if (state_ != State::Idle) {
        queued_.push_back(text != nullptr ? text : "");
        notice("当前 turn 进行中，输入已排队（队列 " + std::to_string(queued_.size()) + "）");
        return;
    }
    begin_turn(text != nullptr ? text : "");
}

void AgentCtx::on_done(const vc_api::LlmDone& e) {
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

    // M6 token 统计：provider 的 usage（JSON 文本）累计进会话总量
    if (e.usage != nullptr) {
        json u = json::parse(e.usage, nullptr, false);
        if (u.is_object()) {
            usage_prompt_ += u.value("prompt_tokens", 0LL);
            usage_completion_ += u.value("completion_tokens", 0LL);
            usage_total_ += u.value("total_tokens", 0LL);
            last_prompt_tokens_ = u.value("prompt_tokens", 0LL);  // 压缩判定的确定性信号
        }
    }
    // 压缩后首 turn 的退避校验（阶段二）：摘要没压下来 ⇒ 阈值 ×1.25 抬升
    if (compress_verify_) {
        compress_verify_ = false;
        if (session_threshold_ > 0 && last_prompt_tokens_ >= session_threshold_) {
            session_threshold_ = session_threshold_ / 4 * 5;
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
                c.tool = tool_last(c.full_name);  // "workspace.read" → "read"（此前漏设 ⇒ 空工具名）
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
    // 摘要请求失败分流（阶段二）：回退机械 digest——压缩永不卡死 turn 流
    if (compress_inflight_ && e.request_id == compress_seq_)
        return on_compress_error_(e);
    if (e.request_id != inflight_) return;  // 迟到错误 / agent 自发收口（rid=0）：忽略
    if (state_ == State::Idle) return;
    // provider 的 llm_error 已由 webui 落错误卡并清 in_flight——agent 只复位
    // 状态机，不重发（重发 = 双重错误卡）
    cleanup_turn_();
}

void AgentCtx::on_decision(const vc_api::TurnDecision& d) {
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
            runs_[rid] = AsyncRun{pending_.index, "", false, 0};
    }
    state_ = State::TurnActive;
    dispatch_loop();
}

}  // namespace vc_agent
