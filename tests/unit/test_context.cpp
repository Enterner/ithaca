// 上下文压缩阶段一的单测（docs/上下文压缩.md）：mission 卡/最小充分记录模板/
// provenance 回查/压缩判定与折叠计划——纯逻辑层直接驱动。
#include "ithaca_test_framework.hpp"

#include "context.hpp"

#include <string>
#include <fstream>

using nlohmann::json;
using vcagent::compact_result;
using vcagent::collect_tasks;
using vcagent::is_digest;
using vcagent::is_mission;
using vcagent::kRecallProvenance;
using vcagent::mission_message;
using vcagent::plan_compress;
using vcagent::recall_result;
using vcagent::should_compress;

VC_TEST("context: mission 卡——当前任务逐字保留，历史任务截到首行 120 字节") {
    json msgs = json::array();
    msgs.push_back(json{{"role", "system"}, {"content", "sys"}});
    msgs.push_back(json{{"role", "user"},
                        {"content", "旧任务第一行\n旧任务第二行（应被截掉）"}});
    msgs.push_back(json{{"role", "assistant"}, {"content", "完成"}});
    const std::string cur(200, 'x');
    msgs.push_back(json{{"role", "user"}, {"content", cur}});

    const auto tasks = collect_tasks(msgs);
    CHECK(tasks.size() == 2);
    CHECK(tasks[0].find('\n') == std::string::npos);   // 历史任务截到首行
    CHECK(tasks[0].size() <= 121);
    CHECK(tasks[1] == cur);                            // 当前任务逐字节保留

    const auto m = mission_message(tasks);
    CHECK(is_mission(m));
    CHECK(m["content"].get<std::string>().find(cur) != std::string::npos);
    CHECK(m["content"].get<std::string>().find("←当前") != std::string::npos);
}

VC_TEST("context: compact read——内容丢弃只留锚点与 ref") {
    json r{{"ok", true}, {"path", "src/main.cpp"},
           {"content", std::string(9000, 'A')}, {"truncated", false}};
    const std::string c = compact_result("workspace.read", r, "t1_1_read.json");
    CHECK(c.find("src/main.cpp") != std::string::npos);        // 锚点逐字节保留
    CHECK(c.find("\"bytes\":9000") != std::string::npos);
    CHECK(c.find("\"ref\":\"t1_1_read.json\"") != std::string::npos);
    CHECK(c.find("AAAA") == std::string::npos);                // 内容不进上下文
}

VC_TEST("context: compact 失败保留详细错误（2KB 内全量）+ ref 兜底") {
    const std::string detail(3000, 'E');
    json r{{"ok", false}, {"error", detail}};
    const std::string c = compact_result("workspace.apply", r, "t2_1_apply.json");
    CHECK(c.find("\"ok\":false") != std::string::npos);
    CHECK(c.find("\"ref\":\"t2_1_apply.json\"") != std::string::npos);
    CHECK(c.find("EEEE") != std::string::npos);                // 2KB 内保留详文
    CHECK(c.size() < detail.size() + 400);                     // 超出部分截断
}

VC_TEST("context: compact apply——文件清单与 delta 保留，diff 丢弃") {
    json r{{"ok", true},
           {"files",
            json::array({json{{"path", "a.cpp"}, {"diff", "@@ -1,1 +1,1 @@\n（+1 −1 行）\n"}}})},
           {"snapshot_depth", 3}};
    const std::string c = compact_result("workspace.apply", r, "");
    CHECK(c.find("a.cpp") != std::string::npos);
    CHECK(c.find("+1 −1") != std::string::npos);
    CHECK(c.find("@@") == std::string::npos);                  // diff 本体不进上下文
}

VC_TEST("context: compact run——退出码与首行保留，输出外置") {
    json r{{"ok", true}, {"exit_code", 1}, {"output", "BUILD FAILED\nline2\nline3\n"}};
    const std::string c = compact_result("runner.run", r, "t3_1_run.json");
    CHECK(c.find("\"exit_code\":1") != std::string::npos);
    CHECK(c.find("BUILD FAILED") != std::string::npos);
    CHECK(c.find("line3") == std::string::npos);               // 仅首行
}

VC_TEST("context: recall——provenance 不可信数据标记") {
    const std::string c = recall_result("t1_1_read.json", "原文内容");
    CHECK(c.find(kRecallProvenance) != std::string::npos);
    CHECK(c.find("原文内容") != std::string::npos);
}

VC_TEST("context: should_compress——阈值判定，0 = 关闭") {
    CHECK(should_compress(32001, 32000));
    CHECK(!should_compress(32000, 32000));
    CHECK(!should_compress(99000, 0));  // 0 = 关闭
}

VC_TEST("context: plan_compress——除最末段外折叠，请求锚点逐字保留") {
    json msgs = json::array();
    msgs.push_back(json{{"role", "system"}, {"content", "sys"}});
    // 段 1：user → assistant(tool_calls) → tool → assistant 收尾
    msgs.push_back(json{{"role", "user"}, {"content", "任务一：修 a.cpp"}});
    msgs.push_back(json{{"role", "assistant"},
                        {"content", ""},
                        {"tool_calls",
                         json::array({json{{"id", "c1"},
                                           {"type", "function"},
                                           {"function",
                                            {{"name", "workspace.apply"},
                                             {"arguments", "{}"}}}}})}});
    msgs.push_back(json{{"role", "tool"},
                        {"tool_call_id", "c1"},
                        {"content",
                         json{{"ok", true},
                              {"files", json::array({json{{"path", "a.cpp"}}})}}
                             .dump()}});
    msgs.push_back(json{{"role", "assistant"}, {"content", "修好了"}});
    // 段 2（最末段：刚完结的 turn，逐字保留）
    msgs.push_back(json{{"role", "user"}, {"content", "任务二：跑测试"}});
    msgs.push_back(json{{"role", "assistant"}, {"content", "全过"}});

    const auto plan = plan_compress(msgs);
    CHECK(plan.valid);
    CHECK(plan.cut_begin == 1);
    CHECK(plan.cut_end == 5);
    CHECK(plan.segments_folded == 1);

    json nm = json::array();
    bool placed = false;
    for (std::size_t k = 0; k < msgs.size(); ++k) {
        if (!placed && k == plan.cut_begin) { nm.push_back(plan.digest); placed = true; }
        if (k >= plan.cut_begin && k < plan.cut_end) continue;
        nm.push_back(msgs[k]);
    }
    CHECK(nm.size() == 4);  // sys + digest + 末段 user + 末段 assistant
    CHECK(nm[0]["content"] == "sys");
    CHECK(is_digest(nm[1]));
    const std::string body = nm[1]["content"].get<std::string>();
    CHECK(body.find("任务一：修 a.cpp") != std::string::npos);   // 请求锚点逐字
    CHECK(body.find("a.cpp") != std::string::npos);              // 调用锚点保留
    CHECK(body.find("\"outcome\":\"ok\"") != std::string::npos);
    CHECK(nm[2]["content"] == "任务二：跑测试");                  // 末段逐字保留
    CHECK(nm[3]["content"] == "全过");
}

VC_TEST("context: plan_compress——旧 digest 并入新 digest 不堆积；不足两段不折叠") {
    json msgs = json::array();
    msgs.push_back(json{{"role", "system"}, {"content", "sys"}});
    msgs.push_back(json{{"role", "system"},
                        {"content",
                         std::string(vcagent::kDigestMarker) + "旧摘要\n- 旧条目\n"}});
    msgs.push_back(json{{"role", "user"}, {"content", "任务一"}});
    msgs.push_back(json{{"role", "assistant"}, {"content", "完成"}});
    msgs.push_back(json{{"role", "user"}, {"content", "任务二"}});
    msgs.push_back(json{{"role", "assistant"}, {"content", "完成"}});
    msgs.push_back(json{{"role", "user"}, {"content", "任务三（末段）"}});

    const auto plan = plan_compress(msgs);
    CHECK(plan.valid);
    CHECK(plan.cut_begin == 2);
    CHECK(plan.cut_end == 6);
    // 装配（与 agent finish_turn 同款）：先插 digest 再做跳过判断
    json nm = json::array();
    bool placed = false;
    for (std::size_t k = 0; k < msgs.size(); ++k) {
        if (!placed && k == plan.cut_begin) { nm.push_back(plan.digest); placed = true; }
        if (k >= plan.cut_begin && k < plan.cut_end) continue;
        if (k < plan.cut_begin && k > 0 && is_digest(msgs[k])) continue;
        nm.push_back(msgs[k]);
    }
    if (!placed) nm.push_back(plan.digest);
    std::size_t digest_count = 0;
    for (const auto& m : nm)
        if (is_digest(m)) ++digest_count;
    CHECK(digest_count == 1);                                    // 不堆积
    const std::string body = nm[1]["content"].get<std::string>();
    CHECK(body.find("旧摘要") != std::string::npos);             // 旧 digest 承接
    CHECK(body.find("任务一") != std::string::npos);
    CHECK(nm.back()["content"] == "任务三（末段）");              // 末段保留

    json few = json::array();
    few.push_back(json{{"role", "system"}, {"content", "sys"}});
    few.push_back(json{{"role", "user"}, {"content", "只有一个任务"}});
    CHECK(!plan_compress(few).valid);
}
