// ithaca_host 的 --selftest 脚手架（main 的唯一 selftest 知识面）。
// 观测端订阅（宿主临时"认识"契约事件——这不是业务编排，编排永远在 agent，
// 这里只校验管道通不通）+ mock 剧本落盘 + M1 workspace 冒烟 / M2 直驱 /
// M3·M4·M5 场景点火 + 三阶段等待推进 + 最终裁决。printf 序列与回归 golden
// 逐字对应——改动输出顺序 = 改 golden（--update-golden 重录）。
#include "selftest.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace ithaca {

void SelftestHarness::install(nostos::Host<>& host, const std::filesystem::path& ws_dir) {
    host_ = &host;
    ws_dir_ = ws_dir;

    // M0 自检的观测端：宿主临时"认识" vc.llm.* 等事件。
    // 【仅 selftest 注册】交互模式下一律不订阅——渲染归界面插件；自动批准/注入类
    // 钩子若在交互模式生效 = 提案被宿主抢答 + delta 刷屏（踩过）。
    subs_.push_back(host.events().on<vc_api::LlmDelta>(
        [this](const vc_api::LlmDelta& e) {
            ++delta_count_;
            std::printf("    delta#%d: %s\n", e.request_id,
                        e.text != nullptr ? e.text : "");
            std::fflush(stdout);
        }));
    subs_.push_back(host.events().on<vc_api::LlmDone>(
        [this](const vc_api::LlmDone& e) {
            if (e.request_id <= 0) return;  // 冒烟注入的"迟到 done"（负 id）不计入
            ++done_count_;
            if (done_count_ >= kDonesRun2 && delta_at_done8_ < 0)
                delta_at_done8_ = delta_count_;
            std::printf("    done#%d\n", e.request_id);
            std::fflush(stdout);
        }));
    subs_.push_back(host.events().on<vc_api::LlmError>(
        [this](const vc_api::LlmError& e) {
            const std::string msg = e.message != nullptr ? e.message : "";
            // M0 语义：用户取消经 llm_error 收口（"已被用户取消"）——这是预期
            // 操作而非错误，单独记 cancel_seen_，不计入 error_count_
            if (msg.find("已被用户取消") != std::string_view::npos) {
                cancel_seen_ = true;
                std::printf("    cancel#%d: %s\n", e.request_id, msg.c_str());
                std::fflush(stdout);
                return;
            }
            ++error_count_;
            std::printf("    error#%d: code=%d %s\n", e.request_id, e.code,
                        e.message != nullptr ? e.message : "");
            std::fflush(stdout);
        }));
    // M3 冒烟观测：自动批准全部提案；首个提案与 decision 之间注入一条"迟到的 done"
    // （request_id 不匹配）——死锁回归：agent 必须忽略而非卡死/污染状态。
    subs_.push_back(host.events().on<vc_api::EditProposed>(
        [this](const vc_api::EditProposed& e) {
            ++proposals_;
            if (proposals_ == 1) {
                const vc_api::LlmDone stale{
                    -1, R"({"role":"assistant","content":"迟到的坏消息"})"};
                host_->events().emit(stale);
            }
            const vc_api::TurnDecision d{e.turn_id, /*approved=*/0};
            host_->events().emit(d);
        }));
    subs_.push_back(host.events().on<vc_api::ToolResult>(
        [](const vc_api::ToolResult& e) {
            std::printf("    [tool] %s ok=%d %s\n", e.tool != nullptr ? e.tool : "?", e.ok,
                        e.result_brief != nullptr ? e.result_brief : "");
            std::fflush(stdout);
        }));
    // M5 观测：run 事件归流（归流正确性 = run_id 与输出/退出码一一对应）
    subs_.push_back(host.events().on<vc_api::RunLine>([this](const vc_api::RunLine& e) {
        run_lines_.emplace_back(e.run_id, e.text != nullptr ? e.text : "");
    }));
    subs_.push_back(host.events().on<vc_api::RunExit>([this](const vc_api::RunExit& e) {
        run_exits_.emplace_back(e.run_id, static_cast<int>(e.exit_code));
    }));

    // M3 冒烟脚手架：写 mock 的 turn 剧本（4 个 LLM 轮次：write → read → replace → 收尾）
    // 并指给 llm_mock——agent 的离线确定性场景（真实 LLM 行为由云端路径覆盖）。
    std::error_code ec;
    std::filesystem::create_directories(ws_dir_ / "ithaca_smoke", ec);
    std::ofstream f(ws_dir_ / "ithaca_m3_scenario.json", std::ios::binary | std::ios::trunc);
    // 定界符用 )json"：内容里的 )" 不会提前终止原始字符串（本仓踩过）。
    // 剧本 8 轮：M3 闭环（write/read/replace/收尾）+ M5（run 失败 → 修复 → run 成功）。
    f << R"json([
 {"role":"assistant","content":"","tool_calls":[{"id":"c1","type":"function","function":{"name":"workspace.apply","arguments":"{\"ops\":[{\"op\":\"write\",\"path\":\"ithaca_smoke/m3.txt\",\"content\":\"状态:.alpha\"}]}"}}]},
 {"role":"assistant","content":"","tool_calls":[{"id":"c2","type":"function","function":{"name":"workspace.read","arguments":"{\"path\":\"ithaca_smoke/m3.txt\"}"}}]},
 {"role":"assistant","content":"","tool_calls":[{"id":"c3","type":"function","function":{"name":"workspace.apply","arguments":"{\"ops\":[{\"op\":\"replace\",\"path\":\"ithaca_smoke/m3.txt\",\"find\":\".alpha\",\"replace\":\".beta\"}]}"}}]},
 {"role":"assistant","content":"M3 场景完成"},
 {"role":"assistant","content":"","tool_calls":[{"id":"c4","type":"function","function":{"name":"runner.run","arguments":"{\"command_line\":\"cmd /c echo BUILD FAILED & exit 1\"}"}}]},
 {"role":"assistant","content":"","tool_calls":[{"id":"c5","type":"function","function":{"name":"workspace.apply","arguments":"{\"ops\":[{\"op\":\"write\",\"path\":\"ithaca_smoke/fix.txt\",\"content\":\"FIXED\"}]}"}}]},
 {"role":"assistant","content":"","tool_calls":[{"id":"c6","type":"function","function":{"name":"runner.run","arguments":"{\"command_line\":\"cmd /c echo BUILD OK\"}"}}]},
 {"role":"assistant","content":"M5 场景完成"},
 {"role":"assistant","content":")json";
    // 第 9 轮（取消场景）：30 行长内容流——中途 cancel，流应即刻终止
    for (int i = 1; i <= 30; ++i) f << "输出行 " << i << "\\n";
    f << "\"}\n]";
    const std::string env1 =
        "ITHACA_MOCK_TURNS=" + (ws_dir_ / "ithaca_m3_scenario.json").string();
    _putenv(env1.c_str());
    // 权限模式：selftest 用 suggest 档——剧本的提案→自动批准→迟到 done
    // 死锁回归路径保持确定性覆盖（auto-edit/full-auto 流由 e2e/单测/dogfood 覆盖）
    _putenv("ITHACA_PERMISSION_MODE=suggest");
}

void SelftestHarness::set_agent_present(bool v) {
    agent_present_ = v;
    expect_dones_ = v ? kDonesRun2 : 1;
    m4_ok_ = !v;
    m5_ok_ = !v;
    selftest_t0_ = std::chrono::steady_clock::now();
}

void SelftestHarness::m2_drive(nostos::Host<>& host) {
    if (agent_present_) return;
    const vc_api::LlmTable* llm = host.published().find_as<vc_api::LlmTable>(vc_api::llm);
    if (llm == nullptr || llm->chat == nullptr) {
        std::printf("selftest 失败：vc.llm.v1 未发布（请加载一个 provider 插件，如 llm_mock）。\n");
        std::fflush(stdout);
    } else {
        const char* info = nullptr;
        if (llm->info != nullptr && llm->info(llm->self, &info) == NOSTOS_OK && info != nullptr)
            std::printf("selftest：provider = %s\n", info);
        const char* messages = "[{\"role\":\"user\",\"content\":\"selftest: 跑一遍回放\"}]";
        const nostos_status st = llm->chat(llm->self, 1, messages);
        std::printf("selftest：chat() => %d，等待 delta/done…\n", static_cast<int>(st));
        std::fflush(stdout);
        if (st != NOSTOS_OK) {
            std::printf("selftest 失败：chat 未受理（status=%d）。\n", static_cast<int>(st));
            std::fflush(stdout);
            done_count_ = -1;  // 让等待循环直接走失败出口
        }
    }
}

void SelftestHarness::m1_smoke_and_kick(nostos::Host<>& host) {
    // M1 冒烟：workspace 工具表在时跑一轮完整编辑事务（同步调用，不参与等待循环）。
    // turn 1 写新文件 → turn 2 replace（先 dry_run 后落盘）→ 撤销 turn 2 → 撤销 turn 1
    // （本批新写的文件，撤销 = 删除）。
    const vc_api::ToolTable* tools =
        host.published().find_as<vc_api::ToolTable>(nostos::svc_id<"ithaca.tool.workspace">);
    if (tools != nullptr && tools->execute != nullptr) {
        ws_ran_ = true;
        const auto call = [&](int turn, const char* tool, const char* args) -> std::string {
            const char* out = nullptr;
            if (tools->execute(tools->self, tool, args, turn, &out) != NOSTOS_OK ||
                out == nullptr)
                return "<execute 基础设施失败>";
            return std::string(out);
        };
        const auto step = [&](const char* label, const std::string& reply, bool expect_ok,
                              const std::string& must_contain) {
            const bool ok =
                reply.find(expect_ok ? "\"ok\":true" : "\"ok\":false") != std::string::npos &&
                (must_contain.empty() || reply.find(must_contain) != std::string::npos);
            std::printf("selftest[workspace] %s: %s\n", label, ok ? "✓" : "✗");
            if (!ok) {
                std::printf("  ↳ %s\n", reply.substr(0, 400).c_str());
                ws_ok_ = false;
            }
        };
        step("list（空目录）", call(1, "list", "{}"), true, "");
        step("write 新文件（turn 1）",
             call(1, "apply",
                  R"({"ops":[{"op":"write","path":"ithaca_smoke/hello.txt","content":"第一行\n第二行\n"}]})"),
             true, "+ 第一行");
        step("read 验证写入", call(1, "read", R"({"path":"ithaca_smoke/hello.txt"})"), true,
             "第二行");
        step("dry_run 校验（turn 2）",
             call(2, "apply",
                  R"({"dry_run":true,"ops":[{"op":"replace","path":"ithaca_smoke/hello.txt","find":"第二行","replace":"第贰行"}]})"),
             true, "+ 第贰行");
        step("read 确认 dry_run 未落盘",
             call(1, "read", R"({"path":"ithaca_smoke/hello.txt"})"), true, "第二行");
        step("replace 正式落盘（turn 2）",
             call(2, "apply",
                  R"({"ops":[{"op":"replace","path":"ithaca_smoke/hello.txt","find":"第二行","replace":"第贰行"}]})"),
             true, "第贰行");
        step("undo 撤销 turn 2", call(1, "undo_turn", "{}"), true, "\"restored\"");
        step("read 验证还原", call(1, "read", R"({"path":"ithaca_smoke/hello.txt"})"), true,
             "第二行");
        step("undo 撤销 turn 1（新文件 = 删除）", call(1, "undo_turn", "{}"), true,
             "\"restored\"");
        step("read 确认已删除", call(1, "read", R"({"path":"ithaca_smoke/hello.txt"})"), false,
             "文件不存在");
    }

    // M3 点火：M1 冒烟先跑完（turn_id 命名空间互不重叠、undo 语义无歧义），再驱动 agent
    if (agent_present_) {
        const vc_api::TurnInput kick{"把 ithaca_smoke/m3.txt 里的 .alpha 改成 .beta"};
        host.events().emit(kick);
    }
}

void SelftestHarness::tick(nostos::Host<>& host) {
    if (phase_ == Phase::Run1) {
        if (done_count_ >= kDonesRun1 && error_count_ == 0) {
            phase_ = Phase::Run2;
            // M5 点火：第二个 turn——重跑构建（失败）→ 修复 → 重跑（成功）
            const vc_api::TurnInput kick2{"重跑构建验证修复"};
            host.events().emit(kick2);
        } else if (error_count_ > 0 || done_count_ < 0) {
            phase_ = Phase::Done;  // 失败收尾（verdict 按 done/error 判定）
        }
    } else if (phase_ == Phase::Run2) {
        if (done_count_ >= kDonesRun2 && error_count_ == 0) {
            phase_ = Phase::Cancel;
            cancel_t0_ = std::chrono::steady_clock::now();
            cancel_sent_ = false;
            // M4 取消场景：第 9 轮走长内容流，中途取消
            const vc_api::TurnInput kick3{"输出 30 行测试文本"};
            host.events().emit(kick3);
        } else if (error_count_ > 0 || done_count_ < 0) {
            phase_ = Phase::Done;
        }
    } else if (phase_ == Phase::Cancel) {
        if (cancel_seen_) {
            m4_ok_ = true;
            phase_ = Phase::Done;
        } else {
            const auto el = std::chrono::steady_clock::now() - cancel_t0_;
            if (!cancel_sent_ && el > std::chrono::milliseconds(300)) {
                const vc_api::TurnDecision c{0, 4};
                host.events().emit(c);
                cancel_sent_ = true;
            }
            if (el > std::chrono::seconds(6)) phase_ = Phase::Done;  // m4_ok_ 保持 false
        }
    }
    if (phase_ != Phase::Done &&
        std::chrono::steady_clock::now() - selftest_t0_ > std::chrono::seconds(20)) {
        std::printf("selftest 失败：20s 内未完成（done=%d/%d error=%d）。\n", done_count_,
                    expect_dones_, error_count_);
        std::fflush(stdout);
        done_count_ = -1;
        phase_ = Phase::Done;
    }
}

int SelftestHarness::verdict() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - selftest_t0_)
                        .count();
    // M3/M5 断言：种子文件被改写（.alpha → .beta）；修复文件落盘；run 事件按
    // run_id 归流（失败命令的输出挂在 run 1，退出码 1 → 0 与剧本一致）。
    bool m3_ok = false;
    if (agent_present_) {
        std::ifstream seed(ws_dir_ / "ithaca_smoke" / "m3.txt", std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(seed)),
                                  std::istreambuf_iterator<char>());
        m3_ok = content.find(".beta") != std::string::npos &&
                content.find(".alpha") == std::string::npos;
    }
    if (agent_present_) {
        std::ifstream fix(ws_dir_ / "ithaca_smoke" / "fix.txt", std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(fix)),
                                  std::istreambuf_iterator<char>());
        const bool exits_ok = run_exits_.size() == 2 && run_exits_[0].first == 1 &&
                              run_exits_[0].second == 1 && run_exits_[1].first == 2 &&
                              run_exits_[1].second == 0;
        bool failed_line_routed = false;
        for (const auto& [id, text] : run_lines_)
            if (id == 1 && text.find("BUILD FAILED") != std::string::npos)
                failed_line_routed = true;
        m5_ok_ = content.find("FIXED") != std::string::npos && exits_ok && failed_line_routed;
    }
    if (done_count_ >= expect_dones_ && error_count_ == 0 && (!ws_ran_ || ws_ok_) &&
        (!agent_present_ || m3_ok) && m4_ok_ && m5_ok_) {
        std::printf("selftest 通过：delta=%d done=%d error=%d proposals=%d，全程 %lldms%s%s%s%s\n",
                    delta_at_done8_ >= 0 ? delta_at_done8_ : delta_count_, done_count_,
                    error_count_, proposals_,
                    static_cast<long long>(ms), ws_ran_ ? "；workspace 冒烟 ✓" : "",
                    agent_present_ ? (m3_ok ? "；M3 闭环 ✓（含死锁回归）" : "") : "",
                    agent_present_ ? (m4_ok_ ? "；M4 取消 ✓" : "") : "",
                    agent_present_ ? (m5_ok_ ? "；M5 回灌 ✓" : "") : "");
        std::fflush(stdout);
        return 0;
    }
    std::printf("selftest 未通过（done=%d/%d error=%d proposals=%d，workspace 冒烟 %s，M3 %s，M4 取消 %s，M5 回灌 %s）。\n",
                done_count_, expect_dones_, error_count_, proposals_,
                ws_ran_ ? (ws_ok_ ? "✓" : "✗") : "未运行",
                agent_present_ ? (m3_ok ? "✓" : "✗") : "未运行",
                agent_present_ ? (m4_ok_ ? "✓" : "✗") : "未运行",
                agent_present_ ? (m5_ok_ ? "✓" : "✗") : "未运行");
    std::fflush(stdout);
    return 3;
}

}  // namespace ithaca
