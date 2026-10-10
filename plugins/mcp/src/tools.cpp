// 统一工具表（plugin/mcp 的 agent 契约切片）。
// list_thunk：按全名 mcp.<server>__<tool> 自述全部工具；exec_thunk/execute：
// tools/call 同步转发 + 结果映射（方案「结果映射」表）；附会话查找原语。

#include "mcp_plugin.hpp"

namespace vc_mcp {

// ---- 统一工具表（契约 vc_tool.hpp：list 自述 + execute 转发）----------------

nostos_status McpPlugin::list_thunk(void* self, const char** out_json) {
    auto* p = static_cast<McpPlugin*>(self);
    json arr = json::array();
    {
        std::lock_guard lk(p->mu_);
        for (const auto& s : p->sessions_) {
            if (!s->alive.load()) continue;
            for (const auto& t : s->tools) {
                if (!t.is_object() || !t.contains("name") || !t["name"].is_string())
                    continue;
                // 全名 mcp.<server>__<tool>：server 段规避跨 server 重名
                arr.push_back(json{
                    {"name", s->name + "__" + t["name"].get<std::string>()},
                    {"description", t.value("description",
                                            "MCP 工具（server: " + s->name + "）")},
                    {"schema", t.value("inputSchema", json::object())}});
            }
        }
    }
    p->list_buf_ = json{{"ok", true}, {"tools", arr}}.dump();
    *out_json = p->list_buf_.c_str();
    return NOSTOS_OK;
}

nostos_status McpPlugin::exec_thunk(void* self, const char* name, const char* args_json,
                                    int /*turn_id——MCP 无快照组语义，忽略*/,
                                    const char** out_json) {
    auto* p = static_cast<McpPlugin*>(self);
    p->exec_buf_ = p->execute(name != nullptr ? name : "",
                              args_json != nullptr ? args_json : "{}");
    *out_json = p->exec_buf_.c_str();
    return NOSTOS_OK;
}

// execute 转发：server__tool 拆分 → tools/call → 结果映射（方案「结果映射」表）
std::string McpPlugin::execute(const std::string& name, const std::string& args_json) {
    const auto us = name.find("__");
    if (us == std::string::npos)
        return json{{"ok", false}, {"error", "MCP 工具名非法: " + name}}.dump();
    McpSession* s = find_session_(name.substr(0, us));
    if (s == nullptr)
        return json{{"ok", false},
                    {"error", "MCP server 未连接: " + name.substr(0, us)}}.dump();
    json args = json::parse(args_json, nullptr, /*allow_exceptions=*/false);
    if (!args.is_object()) args = json::object();

    std::string err;
    json resp;
    if (!s->request("tools/call",
                    {{"name", name.substr(us + 2)}, {"arguments", args}},
                    call_timeout_ms_, resp)) {
        return json{{"ok", false}, {"error", "MCP 调用失败: " + s->diag}}.dump();
    }
    if (!resp.contains("result") || !resp["result"].is_object())
        return json{{"ok", false}, {"error", "MCP 响应缺少 result"}}.dump();
    const json& r = resp["result"];
    std::string text;
    if (r.contains("content") && r["content"].is_array())
        for (const auto& c : r["content"])
            if (c.is_object() && c.value("type", "") == "text" &&
                c.contains("text") && c["text"].is_string())
                text += c["text"].get<std::string>();
    if (r.value("isError", false))
        return json{{"ok", false},
                    {"error", text.empty() ? "MCP 工具执行失败" : text}}.dump();
    return json{{"ok", true}, {"text", text}}.dump();
}

McpSession* McpPlugin::find_session_(const std::string& name) {
    for (auto& s : sessions_)
        if (s->name == name && s->alive.load()) return s.get();
    return nullptr;
}

std::size_t McpPlugin::live_count() const {
    std::size_t n = 0;
    for (const auto& s : sessions_)
        if (s->alive.load()) ++n;
    return n;
}

}  // namespace vc_mcp
