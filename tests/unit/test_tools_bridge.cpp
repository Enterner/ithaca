// 工具桥纯逻辑的单测：命名映射、tools 拼装、审批策略。
#include "ithaca_test_framework.hpp"

#include "tools_bridge.hpp"

#include <string>

using namespace vcagent;

VC_TEST("tools: svc 名 ↔ 工具集名映射") {
    CHECK(toolset_of_svc("ithaca.tool.workspace") == "workspace");
    CHECK(toolset_of_svc("ithaca.tool.runner") == "runner");
    CHECK(toolset_of_svc("nonstandard") == "nonstandard");  // 无前缀：原样
    CHECK(full_tool_name("ithaca.tool.workspace", "apply") == "workspace.apply");
}

VC_TEST("tools: 从 list() 结果拼 OpenAI tools 数组") {
    const std::string list_json =
        R"({"ok":true,"tools":[{"name":"read","description":"读文件",)"
        R"("schema":{"type":"object"}},{"name":"apply","description":"编辑"}]})";
    json out = json::array();
    std::string err;
    REQUIRE(build_tools_json(list_json, "ithaca.tool.workspace", out, err));
    REQUIRE(out.size() == 2);
    CHECK(out[0]["type"] == "function");
    CHECK(out[0]["function"]["name"] == "workspace.read");
    CHECK(out[0]["function"]["description"] == "读文件");
    CHECK(out[0]["function"]["parameters"] == json::parse(R"({"type":"object"})"));
    CHECK(out[1]["function"]["name"] == "workspace.apply");
}

VC_TEST("tools: list() 形态异常被点名拒绝") {
    json out = json::array();
    std::string err;
    CHECK(!build_tools_json(R"({"ok":false})", "ithaca.tool.x", out, err));
    CHECK(!build_tools_json("不是 JSON", "ithaca.tool.x", out, err));
    CHECK(!build_tools_json(R"({"ok":true,"tools":[{"description":"没有名字"}]})", "ithaca.tool.x",
                            out, err));
    CHECK(!err.empty());
}

VC_TEST("tools: 审批策略与 CSV 解析") {
    const auto list = split_csv("workspace.apply, runner.run ,  , workspace.undo_turn");
    REQUIRE(list.size() == 3);
    CHECK(list[0] == "workspace.apply");
    CHECK(list[2] == "workspace.undo_turn");
    CHECK(needs_confirm("workspace.apply", list));
    CHECK(!needs_confirm("workspace.read", list));
    CHECK(split_csv("").empty());
}
