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

// task 内置工具名（子代理派发；对齐 opencode 的 task 工具）。agent 插件内置，
// 不经工具表——dispatch_loop 在 find_bound 之前拦截。
inline constexpr const char* kTaskTool = "task";

// task 工具自述：类型清单进 description（模型据此选 subagent_type）。
// NOTE: 逐层显式构造而非整体初始化列表——嵌套初始化列表有歧义归一规则（某层
// 元素不是两元素对组 ⇒ 整层退化成数组而非对象），错位后无编译期信号，只在运行期
// 以畸形 tools 自述爆雷（踩过：排序比较器按 function.name 取值即挂死）。
inline json build_task_tool_json(const std::vector<std::string>& type_names) {
    std::string types;
    for (const auto& t : type_names) {
        if (!types.empty()) types += " / ";
        types += t;
    }
    json params = json::object();
    params["type"] = "object";
    params["properties"] = json{
        {"description",
         {{"type", "string"}, {"description", "子任务摘要（3-5 词，供展示）"}}},
        {"prompt",
         {{"type", "string"},
          {"description",
           "给子代理的完整任务指令（必须自包含：背景、目标、完成标准——子代理"
           "看不到本次对话的其他内容）"}}},
        {"subagent_type",
         {{"type", "string"}, {"description", "子代理类型（可选，默认 general）"}}}};
    params["required"] = json::array({"description", "prompt"});
    json fn = json::object();
    fn["name"] = kTaskTool;
    fn["description"] =
        std::string("派发一个子代理去执行独立子任务（它有自己的上下文与工具面，不能再派生"
                    "子代理），其最终报告作为本工具结果返回。适合边界清晰、可独立交办的子任务"
                    "（聚焦检索/代码审查/批量机械修改）；需要与用户交互或依赖本对话上下文的"
                    "工作不要派发。可用类型: ") +
        types;
    fn["parameters"] = std::move(params);
    return json{{"type", "function"}, {"function", std::move(fn)}};
}

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
