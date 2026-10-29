// 上下文策略缝（plugin/agent 的 vc.ctx.v1 单点解析切片）。
// 全部包装函数同一形态：表槽在场 → 调表（JSON 数据面）；表/槽缺失 → 调用方走
// 内置同名逻辑（本文件不复制内置实现——内置逻辑仍在原调用点，表恒空时零行为
// 变化）。表指针轮边界解析（begin_turn），轮内持有：策略只在轮边界切换。

#include "agent_plugin.hpp"

namespace vc_agent {

// 轮边界解析：表 + capabilities 自述（缓存 recall 能力位——discover_tools 据此
// 开合 context.recall 内置工具，避免每次发现工具都打 info）。
void AgentCtx::resolve_ctx_strategy_() {
    ctx_tbl_ = host_.service<vc_api::CtxTable>(vc_api::ctx);
    ctx_cap_recall_ = false;
    if (ctx_tbl_ == nullptr || ctx_tbl_->info == nullptr) return;
    const char* info_json = nullptr;
    if (ctx_tbl_->info(ctx_tbl_->self, &info_json) != NOSTOS_OK || info_json == nullptr)
        return;
    const json info = json::parse(info_json, nullptr, /*allow_exceptions=*/false);
    if (!info.is_object()) return;
    if (const auto caps = info.find("capabilities"); caps != info.end() && caps->is_array())
        for (const auto& c : *caps)
            if (c.is_string() && c.get<std::string>() == "recall") ctx_cap_recall_ = true;
}

// plan 出参 → agent 内部管线消费形态（CompressPlan）。action != fold / 区间非法 /
// digest 形态不对 ⇒ 无效计划（调用方按"不折叠"处理）。
static vcagent::CompressPlan plan_from_json(const json& j) {
    vcagent::CompressPlan p;
    if (!j.is_object() || j.value("action", std::string()) != "fold") return p;
    p.digest = j.value("digest_message", json::object());
    if (!p.digest.is_object()) return p;
    const long long cb = j.value("cut_begin", -1LL);
    const long long ce = j.value("cut_end", -1LL);
    if (cb < 0 || ce < cb) return p;
    p.cut_begin = static_cast<std::size_t>(cb);
    p.cut_end = static_cast<std::size_t>(ce);
    if (j.contains("refs") && j["refs"].is_array())
        for (const auto& r : j["refs"])
            if (r.is_string()) p.refs.push_back(r.get<std::string>());
    p.segments_folded = static_cast<std::size_t>(j.value("segments_folded", 0LL));
    p.anchors_total = static_cast<std::size_t>(j.value("anchors_total", 0LL));
    p.anchors_dropped = static_cast<std::size_t>(j.value("anchors_dropped", 0LL));
    p.valid = true;
    return p;
}

vcagent::CompressPlan AgentCtx::acquire_plan_(const char* reason, long long estimate) {
    if (ctx_tbl_ == nullptr || ctx_tbl_->plan == nullptr) return {};
    // 信号透传（原始值，判定口径在策略内）：真实/粗估 prompt tokens、用量、
    // 窗口与输出预留。NOTE: 信号取当前值快照——plan 同步返回，轮内状态在两次
    // 调用之间变化由下一次调用携带。
    json signals{{"last_prompt_tokens", last_prompt_tokens_},
                 {"usage_real", last_usage_real_},
                 {"usage", json{{"prompt_tokens", usage_prompt_},
                                 {"completion_tokens", usage_completion_},
                                 {"total_tokens", usage_total_},
                                 {"cache_tokens", usage_cache_}}},
                 {"window", ctx_window_},
                 {"max_output", ctx_max_output_}};
    if (estimate >= 0) signals["estimate_prompt_tokens"] = estimate;
    const json state{{"messages", messages_},
                     {"signals", std::move(signals)},
                     {"reason", reason},
                     {"subagent", subagent_}};
    const std::string s = state.dump();
    const char* out = nullptr;
    if (ctx_tbl_->plan(ctx_tbl_->self, s.c_str(), &out) != NOSTOS_OK || out == nullptr)
        return {};
    return plan_from_json(json::parse(out, nullptr, /*allow_exceptions=*/false));
}

std::string AgentCtx::strategy_ingest_(const std::string& full_name,
                                       const std::string& content, std::size_t seq) {
    const json in{{"tool", full_name},
                  {"ok", tool_ok(content)},
                  {"body", content},
                  {"turn", turn_no_},
                  {"seq", seq}};
    const std::string s = in.dump();
    const char* out = nullptr;
    if (ctx_tbl_->ingest_tool_result(ctx_tbl_->self, s.c_str(), &out) == NOSTOS_OK &&
        out != nullptr)
        return std::string(out);   // 出参缓冲由实现方持有至下次同槽调用——立即拷贝
    // 槽失败 = 基础设施级异常：原文内联兜底（宁可上下文胖一刻，不丢工具结果）
    host_.log(3, "ithaca.agent: 策略插件 ingest_tool_result 失败，结果原文内联: " + full_name);
    return content;
}

void AgentCtx::strategy_summary_brief_(std::string& system, std::string& instructions) {
    const json in{{"digest", pending_plan_.digest}};
    const std::string s = in.dump();
    const char* out = nullptr;
    if (ctx_tbl_->summary_prompt(ctx_tbl_->self, s.c_str(), &out) != NOSTOS_OK ||
        out == nullptr)
        return;   // 槽失败保持内置 brief 不变（kSummarySystem + 机械 digest 正文）
    const json j = json::parse(out, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return;
    if (const auto it = j.find("system");
        it != j.end() && it->is_string() && !it->get<std::string>().empty())
        system = it->get<std::string>();
    if (const auto it = j.find("instructions");
        it != j.end() && it->is_string() && !it->get<std::string>().empty())
        instructions = it->get<std::string>();
}

bool AgentCtx::strategy_apply_summary_(const std::string& summary, json& out_digest) {
    const json in{{"digest", pending_plan_.digest},
                  {"summary", summary},
                  {"refs", pending_plan_.refs}};
    const std::string s = in.dump();
    const char* out = nullptr;
    if (ctx_tbl_->apply_summary(ctx_tbl_->self, s.c_str(), &out) != NOSTOS_OK ||
        out == nullptr)
        return false;
    const json j = json::parse(out, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || j.value("role", std::string()) != "system") return false;
    out_digest = j;
    return true;
}

}  // namespace vc_agent
