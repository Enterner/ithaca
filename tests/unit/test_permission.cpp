// 权限模式三档的单测：模式归一与确认清单映射（agent 侧纯逻辑）。
// 沙箱（AppContainer）与 Job Object 是 Windows API 集成，走 e2e/dogfood 验证。
#include "ithaca_test_framework.hpp"

#include "context.hpp"
#include "permission.hpp"

#include <string>
#include <vector>

using vcagent::mode_confirm;
using vcagent::normalize_mode;

VC_TEST("permission: 模式归一——未知取值回落 auto-edit") {
    CHECK(normalize_mode("suggest") == "suggest");
    CHECK(normalize_mode("auto-edit") == "auto-edit");
    CHECK(normalize_mode("full-auto") == "full-auto");
    CHECK(normalize_mode("") == "auto-edit");
    CHECK(normalize_mode("yolo") == "auto-edit");
}

VC_TEST("permission: suggest——编辑与命令逐条批准") {
    const auto c = mode_confirm("suggest");
    CHECK(std::find(c.begin(), c.end(), "workspace.apply") != c.end());
    CHECK(std::find(c.begin(), c.end(), "runner.run") != c.end());
    CHECK(c.size() == 2);
}

VC_TEST("permission: auto-edit——编辑免批，命令逐条批准") {
    const auto c = mode_confirm("auto-edit");
    CHECK(std::find(c.begin(), c.end(), "runner.run") != c.end());
    CHECK(std::find(c.begin(), c.end(), "workspace.apply") == c.end());
    CHECK(c.size() == 1);
}

VC_TEST("permission: full-auto——确认清单为空（把关交给沙箱）") {
    CHECK(mode_confirm("full-auto").empty());
}
