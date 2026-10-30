// 子代理派发与收口（plugin/agent 的子代理运行时切片）。
// spawn_child_：task 内置工具的参数/护栏校验与子 ctx 装配（免批/静默/受限工具
// 面/不压缩不持久化），登记在途后由 begin_turn 事件驱动跑循环；on_child_finished_：
// 完成回调——用量并入父会话、报告写回 calls_、按 try_finish_tools 同一门统一
// 收口。类型注册表（.ithaca/agent/*.md 加载）在 src/agent_types.cpp。

#include "agent_plugin.hpp"

namespace vc_agent {

// task 工具：校验参数与护栏 → 起子 ctx（免批/静默/受限工具面）事件驱动跑循环
// → 登记在途。返回 false = 拒绝（result 已写好，调用方 emit 后继续分发）。
// 在途登记先于 begin_turn：begin_turn 同步失败（无工具表/LLM 不可用）会在本帧
// 内经回调走 on_child_finished_ 收口，登记晚了就丢挂起。
bool AgentCtx::spawn_child_(std::size_t index, ToolCallCtx& call) {
    const std::string desc = call.args.value("description", "");
    const std::string prompt = call.args.value("prompt", "");
    const std::string type_name = call.args.value("subagent_type", "general");
    if (prompt.empty() || desc.empty()) {
        call.result = err_result("task 缺少必填参数（description / prompt）");
        return false;
    }
    if (children_.size() >= kMaxChildren) {
        call.result = err_result("并发子代理已达上限（" + std::to_string(kMaxChildren) +
                                 "），等在途任务完成后再派发");
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
        call.result = err_result("未知子代理类型: " + type_name + "（可用: " + known + "）");
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
            call.result = err_result("类型 " + type_name +
                                     " 的工具白名单未命中任何可用工具集");
            return false;
        }
    }
    auto child = std::make_unique<AgentCtx>();
    child->host_ = host_;
    child->session_id_ = session_id_;   // 子代理事件的归属标随父（emit_subagent_ 走子自身）
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
    raw->begin_turn(prompt, "");
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
                      : err_result(report);
        c.done = true;
        emit_tool_result(c.full_name, ok, c.result);
        notice(ok ? "子代理完成（" + std::to_string(turns) + " 轮 · " +
                         std::to_string(child->usage_total_) + " tokens）"
                  : "子代理失败: " + trim(report, 120));
    } catch (...) {
        // 本回调运行在子代理的调用栈上：禁止 fail_turn/cleanup_turn_——那会析构
        // 栈上仍在执行的子 ctx（UB）。降级为本条 task 的错误结果走常规续段
        host_.log(3, "ithaca.agent: 子代理完成回调异常，已降级为该任务的错误结果");
        if (index < calls_.size() && !calls_[index].done) {
            calls_[index].result = err_result("子代理完成回调内部异常");
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

}  // namespace vc_agent
