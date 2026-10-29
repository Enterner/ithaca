// 统一工具表（plugin/mcp 的 agent 契约切片）。
// list_thunk：按全名 mcp.<server>__<tool> 自述全部工具；exec_thunk/execute：
// tools/call 同步转发 + 结果映射；附会话查找原语。

#include "mcp_plugin.hpp"

#include "vcplugin/tool_table.hpp"  // vcplugin：工具表出参契约（四插件单源）

namespace vc_mcp {

// 工具级失败的标准回灌形态（ok:false + error——给模型的正常通路）
static std::string err_dump(std::string msg) {
    return json{{"ok", false}, {"error", std::move(msg)}}.dump();
}

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
    return vcplugin::finish_result(p->list_buf_, json{{"ok", true}, {"tools", arr}},
                                   out_json);
}

nostos_status McpPlugin::exec_thunk(void* self, const char* name, const char* args_json,
                                    int /*turn_id——MCP 无快照组语义，忽略*/,
                                    const char** out_json) {
    auto* p = static_cast<McpPlugin*>(self);
    return vcplugin::finish_result(
        p->exec_buf_,
        p->execute(name != nullptr ? name : "", args_json != nullptr ? args_json : "{}"),
        out_json);
}

// execute 转发：server__tool 拆分 → tools/call → 结果映射
std::string McpPlugin::execute(const std::string& name, const std::string& args_json) {
    const auto us = name.find("__");
    if (us == std::string::npos)
        return err_dump("MCP 工具名非法: " + name);
    auto s = find_session_(name.substr(0, us));
    if (s == nullptr)
        return err_dump("MCP server 未连接: " + name.substr(0, us));
    json args = json::parse(args_json, nullptr, /*allow_exceptions=*/false);
    if (!args.is_object()) args = json::object();

    json resp;
    if (!s->request("tools/call",
                    {{"name", name.substr(us + 2)}, {"arguments", args}},
                    call_timeout_ms_, resp)) {
        return err_dump("MCP 调用失败: " + s->diag);
    }
    if (!resp.contains("result") || !resp["result"].is_object()) {
        // JSON-RPC error 对象透传（方法不存在/参数非法/权限拒绝…）：整体丢弃成
        // 「缺少 result」模型无法自纠——错误消息点名给模型才走结构化回灌通路
        if (resp.contains("error") && resp["error"].is_object()) {
            const std::string em = resp["error"].value("message", "");
            return err_dump("MCP server 错误: " + (em.empty() ? resp["error"].dump() : em));
        }
        return err_dump("MCP 响应缺少 result");
    }
    const json& r = resp["result"];
    std::string text;
    if (r.contains("content") && r["content"].is_array())
        for (const auto& c : r["content"])
            if (c.is_object() && c.value("type", "") == "text" &&
                c.contains("text") && c["text"].is_string())
                text += c["text"].get<std::string>();
    if (r.value("isError", false))
        return err_dump(text.empty() ? "MCP 工具执行失败" : text);
    return json{{"ok", true}, {"text", text}}.dump();
}

std::shared_ptr<McpSession> McpPlugin::find_session_(const std::string& name) {
    std::lock_guard lk(mu_);
    for (auto& s : sessions_)
        if (s->name == name && s->alive.load()) return s;
    return nullptr;
}

}  // namespace vc_mcp
