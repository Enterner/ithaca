// ithaca_host —— ithaca 通用宿主。
//
//   ithaca_host [--selftest] [--seconds N] <插件路径...>
//     无 --selftest：装载插件后进入排水循环，Ctrl+C 退出。
//     --seconds N  ：N 秒后干净退出（0 = 一直跑；自动化验收用，仿内核参考宿主）。
//     --selftest   ：M0 冒烟。装载后直接驱动 vc.llm.v1 跑一次回放，校验
//                    「表调用 → 插件自有线程 → poster → 事件」全链路，收到 done/error 即退出。
//                    宿主侧脚手架（订阅/剧本/点火/裁决）全在 selftest.cpp。
//
// 设计要点：
//   · 排水节奏：poster 唤醒接缝驱动（内核 F3 落地）——任何插件 post 入队即 SetEvent
//     唤醒主循环排水，交互延迟 ≈0；空闲时最多等 250ms（接缝出现前，低延迟与省电
//     不可兼得：16ms 轮询空转、拉长周期则延迟劣化——这个取舍已被消解）。
//   · 宿主不含业务：事件在插件之间流动；宿主唯一的"应用知识"是契约头与这份自检。
//   · 启动自检：加载结果与 PENDING 依赖点名打屏。
#include "selftest.hpp"
#include "vc_events.hpp"

#include <nostos/loader.hpp>
#include <nostos/nostos.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

    // selftest 脚手架（观测订阅 + 剧本落盘 + env 指路）——必须在插件装载前：
    // llm_mock/runner 激活时读取这些 env。
    ithaca::SelftestHarness st;
    if (selftest) {
        const auto ws_dir = [] {
            if (const char* v = std::getenv("ITHACA_WORKSPACE_DIR"); v != nullptr && *v != '\0')
                return std::filesystem::path(v);
            return std::filesystem::current_path();
        }();
        st.install(host, ws_dir);
    }

    host.start();

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

    bool agent_present = false;   // Ctrl+C 语义（一次取消 turn）只在 agent 在场时有意义
    if (selftest) {
        for (const auto& p : plugins)
            if (p.info().name == "vc.agent") agent_present = true;
        st.set_agent_present(agent_present);
        st.m2_drive(host);              // 仅无 agent 时生效（有 agent 让位，防抢剧本轮次）
        st.m1_smoke_and_kick(host);     // workspace 冒烟 + M3 点火
    }

    // 主循环：poster 唤醒驱动（内核 F3）——post 入队即唤醒，排水 + 懒激活重估；
    // 空闲时最多等 250ms。selftest 的三阶段推进在 SelftestHarness::tick；
    // --seconds 到点即退。
#if defined(_WIN32)
    const HANDLE wake_event = CreateEventW(nullptr, /*bManualReset=*/FALSE, /*bInitialState=*/FALSE,
                                           nullptr);
    host.abi_bridge().set_post_wake(
        [](void* arg) noexcept { SetEvent(static_cast<HANDLE>(arg)); },
        wake_event);
#endif
    const auto loop_t0 = std::chrono::steady_clock::now();
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
            st.tick(host);
            if (st.finished()) break;
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

    if (selftest) return st.verdict();
    std::printf("done\n");
    return 0;
}
