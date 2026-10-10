// ithaca_host —— ithaca 通用宿主。
//
//   ithaca_host [--selftest] [--seconds N] <插件路径...>
//     无 --selftest：装载插件后进入 16ms 排水循环，Ctrl+C 退出。
//     --seconds N  ：N 秒后干净退出（0 = 一直跑；自动化验收用，仿内核参考宿主）。
//     --selftest   ：M0 冒烟。装载后直接驱动 vc.llm.v1 跑一次回放，
//                    校验「表调用 → 插件自有线程 → poster → 事件」全链路，收到 done/error 即退出。
//
// 设计要点（方案 §3）：
//   · 排水节奏：poster 唤醒接缝驱动（内核 F3 落地）——任何插件 post 入队即 SetEvent
//     唤醒主循环排水，交互延迟 ≈0；空闲时最多等 250ms（接缝出现前，低延迟与省电
//     不可兼得：16ms 轮询空转、拉长周期则延迟劣化——这个取舍已被消解）。
//   · 宿主不含业务：事件在插件之间流动；宿主唯一的"应用知识"是契约头与这份自检。
//   · 启动自检：加载结果与 PENDING 依赖点名打屏。
#include "vc_events.hpp"
#include "vc_llm.hpp"
#include "vc_tool.hpp"

#include <nostos/loader.hpp>
#include <nostos/nostos.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace {

std::atomic<bool> g_running{true};
std::atomic<int> g_ctrl_count{0};

// 崩溃诊断：未处理异常 ⇒ 用 StackWalk64 走崩溃线程的真实栈并打印符号。
LONG WINAPI on_unhandled(EXCEPTION_POINTERS* info) {
    std::fprintf(stderr, "== 未处理异常 code=0x%08lx addr=%p ==\n",
                 static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
                 info->ExceptionRecord->ExceptionAddress);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    HANDLE proc = GetCurrentProcess();
    CONTEXT ctx = *info->ContextRecord;
    // pc=0（空函数指针调用）时 StackWalk 无法展开：手动打印 RSP 上的返回地址链
    const auto resolve = [&](DWORD64 pc) {
        SYMBOL_INFO* sym = static_cast<SYMBOL_INFO*>(std::calloc(1, sizeof(SYMBOL_INFO) + 256));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_MODULEW mod{};
        mod.SizeOfStruct = sizeof(mod);
        const bool has_sym = SymFromAddr(proc, pc, &disp, sym) != 0;
        const bool has_mod = SymGetModuleInfoW(proc, pc, &mod) != 0;
        std::fprintf(stderr, "    %p [%ls] %s + 0x%llx\n", reinterpret_cast<void*>(pc),
                     has_mod ? mod.ModuleName : L"?", has_sym ? sym->Name : "?",
                     static_cast<unsigned long long>(disp));
        std::free(sym);
    };
    resolve(ctx.Rip);
    const DWORD64 sp = ctx.Rsp;
    for (int k = 0; k < 24; ++k) {
        DWORD64 value = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(sp + k * 8), &value, sizeof value,
                               &got) ||
            got != sizeof value)
            break;
        if (value > 0x10000 && value < 0xFFFFFFFF0000ull) resolve(value);
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

BOOL WINAPI on_console_ctrl(DWORD type) {
    (void)type;  // Ctrl+C / Ctrl+Break / 关窗：计数交给主循环（一次取消 turn，两次退出）
    ++g_ctrl_count;
    return TRUE;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Per-Monitor-V2 DPI 感知（与 shell 同款）：本进程弹的原生对话框（webui 选工作
    // 文件夹）按物理像素渲染——缺省时 DWM 位图拉伸，既糊又有渲染不全的首帧
    // （踩过：对话框首帧灰底白块）。宿主本身无窗口，对其他行为零影响。
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetDpiCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        const auto set_dpi = reinterpret_cast<SetDpiCtxFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_dpi != nullptr)
            set_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    // 崩溃静默（无人值守运行不弹 WER），与内核参考宿主同一组开关。
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    SetUnhandledExceptionFilter(on_unhandled);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    SetConsoleOutputCP(CP_UTF8);  // 中文输出；VT 转义由界面插件自行启用
#endif

    bool selftest = false;
    int seconds = 0;  // 0 = 一直跑（Ctrl+C 退出）
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--selftest") {
            selftest = true;
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else {
            paths.emplace_back(arg);
        }
    }
    if (paths.empty()) {
        std::printf("用法：ithaca_host [--selftest] <插件路径...>\n"
                    "例：ithacaweb.bat 会拼好全部插件产物路径再启动。\n");
        return 2;
    }

    std::printf("== ithaca_host %s ==\n", selftest ? "selftest" : "交互");
    for (const auto& p : paths) std::printf("待加载： %s\n", p.c_str());

    nostos::Host<> host;
    host.set_log_sink([](int level, std::string_view msg) {
        std::printf("  [日志 %d] %.*s\n", level, static_cast<int>(msg.size()), msg.data());
        std::fflush(stdout);
    });

    // M0 自检的观测端：宿主临时"认识" vc.llm.* 三个事件。
    // 这不是业务编排 —— 编排永远在 agent；这里只校验管道通不通。
    // 【仅 selftest 注册】交互模式下一律不订阅——渲染归界面插件；自动批准/注入类
    // 钩子若在交互模式生效 = 提案被宿主抢答 + delta 刷屏（踩过）。
    int delta_count = 0;
    int done_count = 0;
    int error_count = 0;
    int proposals = 0;
    int delta_at_done8 = -1;  // done=8 时刻的 delta 快照——取消阶段（turn 9 长内容）
                              // 流出的行数是"取消 vs 回放"的竞态，不计入确定性断言
    bool cancel_seen = false;  // M4：取消场景的完成信号（agent 的"已被取消"通告）
    std::vector<std::pair<int, std::string>> run_lines;  // M5 观测：run 事件归流
    std::vector<std::pair<int, int>> run_exits;
    // NOTE: 订阅句柄必须活到 main 结束——放 if 块内会在块尾析构＝立刻全部退订
    // （踩过：selftest 的 done/proposal 全部收不到，20s 超时）
    std::vector<nostos::Subscription> subs;
    if (selftest) {
        subs.push_back(host.events().on<vc_api::LlmDelta>(
            [&delta_count](const vc_api::LlmDelta& e) {
                ++delta_count;
                std::printf("    delta#%d: %s\n", e.request_id,
                            e.text != nullptr ? e.text : "");
                std::fflush(stdout);
            }));
        subs.push_back(host.events().on<vc_api::LlmDone>(
            [&done_count, &delta_count, &delta_at_done8](const vc_api::LlmDone& e) {
                if (e.request_id <= 0) return;  // 冒烟注入的"迟到 done"（负 id）不计入
                ++done_count;
                if (done_count >= 8 && delta_at_done8 < 0) delta_at_done8 = delta_count;
                std::printf("    done#%d\n", e.request_id);
                std::fflush(stdout);
            }));
        subs.push_back(host.events().on<vc_api::LlmError>(
            [&error_count, &cancel_seen](const vc_api::LlmError& e) {
                const std::string msg = e.message != nullptr ? e.message : "";
                // M0 语义：用户取消经 llm_error 收口（"已被用户取消"）——这是预期
                // 操作而非错误，单独记 cancel_seen，不计入 error_count
                if (msg.find("已被用户取消") != std::string_view::npos) {
                    cancel_seen = true;
                    std::printf("    cancel#%d: %s\n", e.request_id, msg.c_str());
                    std::fflush(stdout);
                    return;
                }
                ++error_count;
                std::printf("    error#%d: code=%d %s\n", e.request_id, e.code,
                            e.message != nullptr ? e.message : "");
                std::fflush(stdout);
            }));
        // M3 冒烟观测：自动批准全部提案；首个提案与 decision 之间注入一条"迟到的 done"
        // （request_id 不匹配）——死锁回归：agent 必须忽略而非卡死/污染状态（方案 §8 M3）。
        subs.push_back(host.events().on<vc_api::EditProposed>(
            [&](const vc_api::EditProposed& e) {
                ++proposals;
                if (proposals == 1) {
                    const vc_api::LlmDone stale{
                        -1, R"({"role":"assistant","content":"迟到的坏消息"})"};
                    host.events().emit(stale);
                }
                const vc_api::TurnDecision d{e.turn_id, /*approved=*/0};
                host.events().emit(d);
            }));
        subs.push_back(host.events().on<vc_api::ToolResult>(
            [](const vc_api::ToolResult& e) {
                std::printf("    [tool] %s ok=%d %s\n", e.tool != nullptr ? e.tool : "?", e.ok,
                            e.result_brief != nullptr ? e.result_brief : "");
                std::fflush(stdout);
            }));
        // M5 观测：run 事件归流（归流正确性 = run_id 与输出/退出码一一对应）
        subs.push_back(host.events().on<vc_api::RunLine>([&](const vc_api::RunLine& e) {
            run_lines.emplace_back(e.run_id, e.text != nullptr ? e.text : "");
        }));
        subs.push_back(host.events().on<vc_api::RunExit>([&](const vc_api::RunExit& e) {
            run_exits.emplace_back(e.run_id, static_cast<int>(e.exit_code));
        }));
    }

    host.start();

    // M3 冒烟脚手架：写 mock 的 turn 剧本（4 个 LLM 轮次：write → read → replace → 收尾）
    // 并指给 llm_mock——agent 的离线确定性场景（真实 LLM 行为由云端路径覆盖）。
    const auto ws_dir = [] {
        if (const char* v = std::getenv("ITHACA_WORKSPACE_DIR"); v != nullptr && *v != '\0')
            return fs::path(v);
        return fs::current_path();
    };
    if (selftest) {
        std::error_code ec;
        fs::create_directories(ws_dir() / "ithaca_smoke", ec);
        std::ofstream f(ws_dir() / "ithaca_m3_scenario.json", std::ios::binary | std::ios::trunc);
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
        // 第 9 轮取消场景）：30 行长内容流——中途 cancel，流应即刻终止
        for (int i = 1; i <= 30; ++i) f << "输出行 " << i << "\\n";
        f << "\"}\n]";
        const std::string env1 = "ITHACA_MOCK_TURNS=" + (ws_dir() / "ithaca_m3_scenario.json").string();
        _putenv(env1.c_str());
        // 权限模式：selftest 用 suggest 档——剧本的提案→自动批准→迟到 done
        // 死锁回归路径保持确定性覆盖（auto-edit/full-auto 流由 e2e/单测/dogfood 覆盖）
        _putenv("ITHACA_PERMISSION_MODE=suggest");
    }

    // M2 懒激活（内核宿主策略）：manifest 依赖未就绪的插件登记 PENDING，重估后自动拉起。
    nostos::LazyGate gate{host.abi_bridge()};
    std::vector<nostos::DynamicPlugin> plugins;
    for (const auto& path : paths) {
        try {
            auto plugin = nostos::DynamicPlugin::load(path);
            std::printf("已加载插件 '%s' v%s\n", plugin.info().name.c_str(),
                        plugin.info().version_string().c_str());
            if (gate.submit(plugin)) {
                plugins.push_back(std::move(plugin));
            } else {
                std::printf("  ↳ manifest 依赖未就绪，登记 PENDING（依赖出现后自动激活）\n");
            }
            for (auto& ready : gate.pump_into()) plugins.push_back(std::move(ready));
        } catch (const nostos::nostos_error& e) {
            std::printf("加载插件失败（宿主继续跑）：%s\n", e.what());
        }
    }
    std::printf("自检：活跃插件 %zu 个。\n", plugins.size());
    std::fflush(stdout);

    // 完成判定：agent 在场时剧本前 7 轮为闭环（4 轮编辑 + 3 轮回灌）⇒ 等 7 个 done；
    // 第 8 轮（长内容）留给取消场景。仅 provider 时等 1 个。
    int expect_dones = 1;
    bool agent_present = false;
    for (const auto& p : plugins)
        if (p.info().name == "vc.agent") agent_present = true;
    if (selftest && agent_present) expect_dones = 8;  // 剧本 8 轮（4 轮编辑 + 3 轮回灌 + 收尾）
    bool m4_ok = !agent_present;  // M4：取消场景结论（无 agent 时视为通过）
    bool m5_ok = !agent_present;  // M5：构建回灌结论（无 agent 时视为通过）
    const auto selftest_t0 = std::chrono::steady_clock::now();
    bool ws_ran = false;  // M1：workspace 冒烟是否运行（工具表在才运行）
    bool ws_ok = true;    // 冒烟结论，并入 selftest 退出码

    // M2 冒烟（无 agent 时）：直接驱动 vc.llm.v1 校验链路；agent 在场时**让位**——
    // 直连 chat 会抢走 mock 剧本的轮次，闭环由 agent 驱动。
    if (selftest && !agent_present) {
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
                done_count = -1;  // 让下面的循环直接走失败出口
            }
        }
    }

    // M1 冒烟：workspace 工具表在时跑一轮完整编辑事务（同步调用，不参与等待循环）。
    // turn 1 写新文件 → turn 2 replace（先 dry_run 后落盘）→ 撤销 turn 2 → 撤销 turn 1
    // （本批新写的文件，撤销 = 删除）。
    if (selftest) {
        const vc_api::ToolTable* tools =
            host.published().find_as<vc_api::ToolTable>(nostos::svc_id<"ithaca.tool.workspace">);
        if (tools != nullptr && tools->execute != nullptr) {
            ws_ran = true;
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
                    ws_ok = false;
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
    if (selftest && agent_present) {
        const vc_api::TurnInput kick{"把 ithaca_smoke/m3.txt 里的 .alpha 改成 .beta"};
        host.events().emit(kick);
    }
    }

    // 主循环：poster 唤醒驱动（内核 F3）——post 入队即唤醒，排水 + 懒激活重估；
    // 空闲时最多等 250ms。selftest 两阶段：闭环（等 4 done）→ 取消场景（长流水
    // 中途 cancel，等"已被取消"通告且 done 不再增长）；--seconds 到点即退。
#if defined(_WIN32)
    const HANDLE wake_event = CreateEventW(nullptr, /*bManualReset=*/FALSE, /*bInitialState=*/FALSE,
                                           nullptr);
    host.abi_bridge().set_post_wake(
        [](void* arg) noexcept { SetEvent(static_cast<HANDLE>(arg)); },
        wake_event);
#endif
    const auto loop_t0 = std::chrono::steady_clock::now();
    // selftest 三阶段：Run1闭环，等 4 done）→ Run2回灌，等 8 done）→
    // Cancel（长流水中途取消，等"已被取消"通告且 done 不再增长）
    enum class Phase { Run1, Run2, Cancel, Done };
    Phase phase = Phase::Run1;
    auto cancel_t0 = std::chrono::steady_clock::now();
    bool cancel_sent = false;
    int ctrl_seen = 0;
    auto last_ctrl_at = std::chrono::steady_clock::now();
    while (g_running) {
        host.drain_posted();
        for (auto& ready : gate.pump_into()) plugins.push_back(std::move(ready));

        // Ctrl+C 语义：一次取消当前 turn（agent 在场时），1.5s 内两次退出
        const int ctrl = g_ctrl_count.load();
        if (ctrl != ctrl_seen) {
            const auto now2 = std::chrono::steady_clock::now();
            if (ctrl >= 2 && now2 - last_ctrl_at < std::chrono::milliseconds(1500)) {
                g_running = false;
                break;
            }
            last_ctrl_at = now2;
            ctrl_seen = ctrl;
            if (agent_present) {
                const vc_api::TurnDecision cancel{0, 4};
                host.events().emit(cancel);
                std::printf("  已请求取消当前 turn（1.5s 内再次 Ctrl+C 退出）\n");
                std::fflush(stdout);
            } else {
                g_running = false;
                break;
            }
        }

        if (selftest) {
            if (phase == Phase::Run1) {
                if (done_count >= 4 && error_count == 0) {
                    phase = Phase::Run2;
                    // M5 点火：第二个 turn——重跑构建（失败）→ 修复 → 重跑（成功）
                    const vc_api::TurnInput kick2{"重跑构建验证修复"};
                    host.events().emit(kick2);
                } else if (error_count > 0 || done_count < 0) {
                    phase = Phase::Done;  // 失败收尾（verdict 按 done/error 判定）
                }
            } else if (phase == Phase::Run2) {
                if (done_count >= 8 && error_count == 0) {
                    phase = Phase::Cancel;
                    cancel_t0 = std::chrono::steady_clock::now();
                    cancel_sent = false;
                    // M4 取消场景：第 9 轮走长内容流，中途取消
                    const vc_api::TurnInput kick3{"输出 30 行测试文本"};
                    host.events().emit(kick3);
                } else if (error_count > 0 || done_count < 0) {
                    phase = Phase::Done;
                }
            } else if (phase == Phase::Cancel) {
                if (cancel_seen) {
                    m4_ok = true;
                    phase = Phase::Done;
                } else {
                    const auto el = std::chrono::steady_clock::now() - cancel_t0;
                    if (!cancel_sent && el > std::chrono::milliseconds(300)) {
                        const vc_api::TurnDecision c{0, 4};
                        host.events().emit(c);
                        cancel_sent = true;
                    }
                    if (el > std::chrono::seconds(6)) phase = Phase::Done;  // m4_ok 保持 false
                }
            } else {
                break;  // Done
            }
            if (phase != Phase::Done &&
                std::chrono::steady_clock::now() - selftest_t0 > std::chrono::seconds(20)) {
                std::printf("selftest 失败：20s 内未完成（done=%d/8 error=%d）。\n", done_count,
                            error_count);
                std::fflush(stdout);
                done_count = -1;
                phase = Phase::Done;
                break;
            }
        }
        if (seconds > 0 && std::chrono::steady_clock::now() - loop_t0 >
                               std::chrono::milliseconds(static_cast<long long>(seconds) * 1000)) {
            break;
        }
#if defined(_WIN32)
        WaitForSingleObject(wake_event, 250);  // 唤醒即刻返回；空闲每 250ms 醒一次
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(16));  // 无接缝平台的轮询回退
#endif
    }
#if defined(_WIN32)
    host.abi_bridge().set_post_wake(nullptr, nullptr);  // 先摘钩再关句柄（防御纵深）
    CloseHandle(wake_event);
#endif

    // 逆序卸载：插件之间无依赖时就是倒着来；依赖者先行由内核会话语义兜底。
    for (auto it = plugins.rbegin(); it != plugins.rend(); ++it) {
        it->deactivate();
        it->unload();
    }
    host.shutdown();

    if (selftest) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - selftest_t0)
                            .count();
        // M3/M5 断言：种子文件被改写（.alpha → .beta）；修复文件落盘；run 事件按
        // run_id 归流（失败命令的输出挂在 run 1，退出码 1 → 0 与剧本一致）。
        bool m3_ok = false;
        if (agent_present) {
            std::ifstream seed(ws_dir() / "ithaca_smoke" / "m3.txt", std::ios::binary);
            const std::string content((std::istreambuf_iterator<char>(seed)),
                                      std::istreambuf_iterator<char>());
            m3_ok = content.find(".beta") != std::string::npos &&
                    content.find(".alpha") == std::string::npos;
        }
        if (agent_present) {
            std::ifstream fix(ws_dir() / "ithaca_smoke" / "fix.txt", std::ios::binary);
            const std::string content((std::istreambuf_iterator<char>(fix)),
                                      std::istreambuf_iterator<char>());
            const bool exits_ok = run_exits.size() == 2 && run_exits[0].first == 1 &&
                                  run_exits[0].second == 1 && run_exits[1].first == 2 &&
                                  run_exits[1].second == 0;
            bool failed_line_routed = false;
            for (const auto& [id, text] : run_lines)
                if (id == 1 && text.find("BUILD FAILED") != std::string::npos)
                    failed_line_routed = true;
            m5_ok = content.find("FIXED") != std::string::npos && exits_ok &&
                    failed_line_routed;
        }
        if (done_count >= expect_dones && error_count == 0 && (!ws_ran || ws_ok) &&
            (!agent_present || m3_ok) && m4_ok && m5_ok) {
            std::printf("selftest 通过：delta=%d done=%d error=%d proposals=%d，全程 %lldms%s%s%s%s\n",
                        delta_at_done8 >= 0 ? delta_at_done8 : delta_count, done_count,
                        error_count, proposals,
                        static_cast<long long>(ms), ws_ran ? "；workspace 冒烟 ✓" : "",
                        agent_present ? (m3_ok ? "；M3 闭环 ✓（含死锁回归）" : "") : "",
                        agent_present ? (m4_ok ? "；M4 取消 ✓" : "") : "",
                        agent_present ? (m5_ok ? "；M5 回灌 ✓" : "") : "");
            std::fflush(stdout);
            return 0;
        }
        std::printf("selftest 未通过（done=%d/%d error=%d proposals=%d，workspace 冒烟 %s，M3 %s，M4 取消 %s，M5 回灌 %s）。\n",
                    done_count, expect_dones, error_count, proposals,
                    ws_ran ? (ws_ok ? "✓" : "✗") : "未运行",
                    agent_present ? (m3_ok ? "✓" : "✗") : "未运行",
                    agent_present ? (m4_ok ? "✓" : "✗") : "未运行",
                    agent_present ? (m5_ok ? "✓" : "✗") : "未运行");
        std::fflush(stdout);
        return 3;
    }
    std::printf("done\n");
    return 0;
}
