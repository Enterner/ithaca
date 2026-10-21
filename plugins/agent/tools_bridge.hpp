#pragma once
// agent 的纯逻辑件：工具名映射 / tools JSON 拼装 / 审批策略。与宿主事件零耦合，可单测。
//
// 工具命名约定：LLM 看到的工具名 = "<工具集>.<工具>"，其中工具集 =
// 工具表 svc 名去掉 "ithaca.tool." 前缀（vc.tool.workspace → workspace）。
// agent 因此对任意新工具插件零硬编码：拿到表就能拼 tools，拿到 tool_call 就能回表。
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace vcagent {

using nlohmann::json;

inline std::string toolset_of_svc(const std::string& svc) {
    const std::string prefix = "ithaca.tool.";
    if (svc.rfind(prefix, 0) == 0) return svc.substr(prefix.size());
    return svc;
}

inline std::string full_tool_name(const std::string& svc, const std::string& tool) {
    return toolset_of_svc(svc) + "." + tool;
}

// 从工具表的 list() 结果（{"ok":true,"tools":[{name,description,schema}]}）拼
// OpenAI tools 数组。list 失败 / 形态不对 ⇒ false + err 点名。
inline bool build_tools_json(const std::string& list_json, const std::string& svc, json& out,
                             std::string& err) {
    const json doc = json::parse(list_json, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object() || doc.value("ok", false) != true) {
        err = "工具表 list() 返回异常: " + list_json.substr(0, list_json.size() < 120 ? list_json.size() : 120);
        return false;
    }
    const auto tools = doc.find("tools");
    if (tools == doc.end() || !tools->is_array()) {
        err = "工具表缺少 tools 数组";
        return false;
    }
    for (const auto& t : *tools) {
        if (!t.is_object() || !t.contains("name") || !t["name"].is_string()) {
            err = "工具条目缺 name";
            return false;
        }
        json fn = {{"name", full_tool_name(svc, t["name"].get<std::string>())}};
        if (t.contains("description")) fn["description"] = t["description"];
        if (t.contains("schema")) fn["parameters"] = t["schema"];
        out.push_back(json{{"type", "function"}, {"function", std::move(fn)}});
    }
    return true;
}

inline std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c != ' ' && c != '\t') {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const std::string& x) { return x.empty(); }),
              out.end());
    return out;
}

inline bool needs_confirm(const std::string& tool_full_name,
                          const std::vector<std::string>& confirm_list) {
    return std::find(confirm_list.begin(), confirm_list.end(), tool_full_name) !=
           confirm_list.end();
}

}  // namespace vcagent
