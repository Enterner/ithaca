// ithaca_host —— ithaca 通用宿主。
//
//   ithaca_host [--profile <路径|名字>] [--selftest] [--seconds N] <插件路径...>
//     --profile    ：产品形态装配（host/profile.hpp）：按声明装载插件集 + 把产品级
//                    配置翻译成既有 env 旋钮。裸名查 profiles/<名>.json（cwd 与 exe
//                    三级父目录）；位置参数插件追加在 profile 清单之后。不带时行为
//                    与无 profile 完全一致（向后兼容）。
//     无 --selftest：装载插件后进入排水循环，Ctrl+C 退出。
//     --seconds N  ：N 秒后干净退出（0 = 一直跑；自动化验收用，仿内核参考宿主）。
//     --selftest   ：冒烟自检。装载后直接驱动 vc.llm.v1 跑一次回放，校验
//                    「表调用 → 插件自有线程 → poster → 事件」全链路，收到 done/error 即退出。
//                    宿主侧脚手架（订阅/剧本/点火/裁决）全在 selftest.cpp。
//     --respawn <pid> --respawn-port <端口>：
//                  内部模式（webui /api/restart 触发，不进用法文本）：分离
//                  respawner——等 pid 进程死亡后按原命令行（剥掉本组 flag）重新
//                  拉起自身；不装载插件不进排水循环。端口经 ITHACA_WEBUI_PORT
//                  覆盖为实际端口，实现同端口重启（见 respawn_entry 注释）。
//
// 设计要点：
//   · 排水节奏：poster 唤醒接缝驱动（接缝由内核 set_post_wake 提供）——任何插件
//     post 入队即 SetEvent 唤醒主循环排水，交互延迟 ≈0；空闲时最多等 250ms——
//     延迟靠 post 即醒、省电靠拉长空闲等待，两全；纯轮询做不到：16ms 轮询空转、
//     拉长周期则延迟劣化。
//   · 宿主不含业务：事件在插件之间流动；宿主唯一的"应用知识"是契约头与这份自检。
//   · 启动自检：加载结果与 PENDING 依赖点名打屏。
#include "profile.hpp"
#include "selftest.hpp"
#include "vc_events.hpp"

#include <nostos/loader.hpp>
#include <nostos/nostos.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#if defined(_WIN32)
#include <crtdbg.h>  // _CrtSetReportMode：Debug 断言改道 stderr（弹窗卡死无人值守链路）
#endif
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

// abort() 落点打符号栈（Debug CRT 断言/abort 不走 SEH，on_unhandled 抓不到——
// 排查「弹窗 abort 但日志无源行号」必须有自己的 SIGABRT 出口；CaptureStackBackTrace
// 在 abort 语境下安全（纯帧遍历，不分配不装载）。
void on_sigabrt(int) {
    void* frames[24];
    const WORD n = CaptureStackBackTrace(0, 24, frames, nullptr);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::fprintf(stderr, "== abort() 调用栈 ==\n");
    for (WORD k = 0; k < n; ++k) {
        const DWORD64 pc = reinterpret_cast<DWORD64>(frames[k]);
        SYMBOL_INFO* sym = static_cast<SYMBOL_INFO*>(
            std::calloc(1, sizeof(SYMBOL_INFO) + 512));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        const bool has_sym = SymFromAddr(GetCurrentProcess(), pc, nullptr, sym) != 0;
        IMAGEHLP_LINEW64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD disp = 0;
        const bool has_line =
            SymGetLineFromAddrW64(GetCurrentProcess(), pc, &disp, &line) != 0;
        std::fprintf(stderr, "    %s  [%ls:%lu]\n", has_sym ? sym->Name : "?",
                     has_line ? line.FileName : L"?", has_line ? line.LineNumber : 0);
        std::free(sym);
    }
    std::fflush(stderr);
    // 复位处理器并重抛：让 _CALL_REPORTFAULT/wer 流程照旧（本处理器只负责留痕）
    signal(SIGABRT, SIG_DFL);
    std::abort();
}

// ---- 自我 respawn（--respawn 内部模式；webui /api/restart 的落地机制）-------
// 重启端点在本进程退出前 spawn 的分离 respawner：等父进程真正死亡（监听端口
// 随之释放）后按原命令行重新拉起宿主。端口不变 ⇒ 前端（浏览器/WebView2 同源）
// 靠既有自愈机制（SSE 重连 + 补洞轮询 + after 回填）原地恢复，无需端口再发现。
// NOTE: 两段 spawn 一律 bInheritHandles=FALSE——父进程的可继承句柄（监听
// socket、transcript 文件等）泄进 respawner/新宿主会让新宿主 bind「成功」
//（SO_REUSEADDR 双绑）而连接落进无人 accept 的旧套接字（踩过: 健康检查全超
// 时，netstat 同端口双 LISTENING）。代价是 std 句柄不透传，新宿主日志断写
//（壳模式 host.log 到此为止，下次壳拉起续上）。
// NOTE: 等父用 pid + OpenProcess：respawner 由「即将退出的父」spawn——起跑时
// 父必然还在（spawn-then-exit 顺序），pid 无复用窗口；OpenProcess 失败即父已
// 死亡，等待目标本身已达成，直接进入拉起。
// NOTE: 新宿主 env 继承 respawner（= 旧宿主 env 全量），ITHACA_WEBUI_PORT 显式
// 覆盖为实际端口——壳模式传 0（系统分配），不覆盖则新宿主换端口、同源自愈失效。
// NOTE: 重启 = 复用原启动参数：插件清单启动时定死（argv 列 DLL 形态不变；
// --profile 形态重读 profile，清单内增删的插件生效）。
// 返回值：-1 = 非 respawn 模式（交回正常启动）；0 = respawn 完成；1 = respawn 失败。
int respawn_entry(int argc, char** argv) {
    DWORD parent_pid = 0;
    std::string port;
    bool is_respawn = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--respawn" && i + 1 < argc) {
            is_respawn = true;
            parent_pid = static_cast<DWORD>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--respawn-port" && i + 1 < argc) {
            port = argv[++i];
        }
    }
    if (!is_respawn) return -1;
    if (port.empty()) return 1;   // 缺端口 = 无法保证同端口，宁可不起新实例

    const std::wstring wport(port.begin(), port.end());   // 纯数字端口，窄化无损
    SetEnvironmentVariableW(L"ITHACA_WEBUI_PORT", wport.c_str());

    // 等父死亡（30s 上限）：超时 = 旧宿主 exit 卡死，宁可不起——双宿主会争抢
    // 同一 home（index/transcript 共写）与端口
    if (HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid)) {
        const DWORD w = WaitForSingleObject(parent, 30000);
        CloseHandle(parent);
        if (w != WAIT_OBJECT_0) return 1;
    }

    // 新宿主命令行 = 原命令行剥掉本组 flag：spawn 方固定把 flag 追加在尾部，
    // 精确后缀匹配可安全剥离；不重拼引号，原始参数里的引号/空格原样保留
    std::wstring cmd = GetCommandLineW();
    const std::wstring suffix = L" --respawn " + std::to_wstring(parent_pid) +
                                L" --respawn-port " + wport;
    const auto pos = cmd.rfind(suffix);
    if (pos == std::wstring::npos || pos + suffix.size() != cmd.size()) return 1;
    cmd.resize(pos);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                        /*bInheritHandles=*/FALSE, DETACHED_PROCESS,
                        nullptr, nullptr, &si, &pi))
        return 1;   // 旧宿主已死：壳看门狗判停 / 用户点会话行重拉
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

// ---- mods 目录扫描（约定式装载面）------------------------------------------
// <exe>\mods\*.dll 按文件名序追加进插件清单——argv/profile 之外的唯一自动发现
// 通道：mod 作者（含发行版里的 agent 自己，随包 sdk/mod 提供构建 SDK）把 DLL
// 丢进目录、重启即生效，无需改启动器或 profile。重启 respawn 复用原命令行，
// 本扫描属启动逻辑照跑 ⇒ 「装 mod → 点重启」闭环成立。
// NOTE: 恒在 argv/profile 清单之后追加 ⇒ mod 晚于 agent 激活，其 tool_register
// 热接入通告必然送达已订阅的 agent（先于 agent 装载的插件走静态清单/环境变量）。
// NOTE: 壳多实例共享同一 exe 目录——mods 是全实例级装配，不随会话隔离。
std::vector<std::string> scan_mods(const std::filesystem::path& dir,
                                   const std::vector<std::string>& known) {
    std::vector<std::string> found;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return found;
    auto fold = [](std::string s) {   // Windows 路径大小写不敏感：比较前统一折叠
        for (char& c : s) c = static_cast<char>(std::tolower(
                                  static_cast<unsigned char>(c)));
        return s;
    };
    std::vector<std::string> known_fold;
    known_fold.reserve(known.size());
    for (const auto& p : known) {
        auto canon = std::filesystem::weakly_canonical(p, ec);
        known_fold.push_back(fold(ec ? p : canon.string()));
    }
    for (const auto& it : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!it.is_regular_file(ec)) continue;
        std::string ext = it.path().extension().string();
        if (fold(ext) != ".dll") continue;
        const std::string p = it.path().string();
        const auto canon = std::filesystem::weakly_canonical(p, ec);
        const std::string key = fold(ec ? p : canon.string());
        if (std::find(known_fold.begin(), known_fold.end(), key) !=
            known_fold.end())
            continue;   // 与 argv/profile 指向同一文件：只装载一次
        known_fold.push_back(key);
        found.push_back(p);
    }
    std::sort(found.begin(), found.end());   // 文件名序 = 确定性装载序
    return found;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Per-Monitor-V2 DPI 感知（与 shell 同款）：本进程弹的原生对话框（webui 选工作
    // 文件夹）按物理像素渲染——缺省时 DWM 位图拉伸，既糊又有渲染不全的首帧
    // （踩过: 对话框首帧灰底白块）。宿主本身无窗口，对其他行为零影响。
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
    // Debug CRT 断言改道 stderr（断言文本 + 源文件行号进 host.log 后即弃进程）：
    // 默认弹窗形态在多实例/连续断言时会叠出一排模态框卡死无人值守链路（踩过:
    // 工具轮次连发断言 → 8 个弹窗叠罗汉，宿主假死）。Debug 构建专属，Release 无 CRT
    // 断言路径不受影响。
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    SetUnhandledExceptionFilter(on_unhandled);
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
    SetConsoleOutputCP(CP_UTF8);  // 中文输出；VT 转义由界面插件自行启用
    signal(SIGABRT, on_sigabrt);  // abort 落点留痕（Debug 断言改道后仍可能直走 abort）
#endif

    // --respawn 内部模式先于一切启动逻辑：respawner 只是「等 + 拉起」的薄壳，
    // 不装载插件、不进排水循环（正常启动没有这一态）
    {
        const int rc = respawn_entry(argc, argv);
        if (rc >= 0) return rc;
    }

    // 用法文本单一出处（--profile 缺值与无插件两出口共用；签名一变改这里一处）
    static const char* const kUsage =
        "用法：ithaca_host [--profile <路径|名字>] [--selftest] [--seconds N] <插件路径...>\n";

    bool selftest = false;
    int seconds = 0;
    std::string profile_arg;
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--selftest") {
            selftest = true;
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        } else if (arg == "--profile" && i + 1 < argc) {
            profile_arg = argv[++i];
        } else if (arg == "--profile") {  // 缺值：装配目标不明，立即拒绝
            std::printf("%s--profile 缺少值。\n", kUsage);
            return 2;
        } else {
            paths.emplace_back(arg);
        }
    }
    // profile 装配先于一切 env 消费方（selftest 的场景 env 在其后应用 ⇒ 场景覆盖
    // profile 默认）；清单合并 = profile 在前、位置参数追加在后（两路可共存，
    // 不带 --profile 时本段整体跳过，行为与无 profile 的裸宿主完全一致）。
    if (!profile_arg.empty()) {
        const ithaca::profile::Loaded prof =
            ithaca::profile::load(profile_arg, ithaca::profile::exe_dir(argv[0]));
        if (!prof.ok) {
            std::printf("profile 装配失败：%s\n", prof.error.c_str());
            return 2;  // 产品装配失败不能带病启动
        }
        if (!prof.error.empty()) std::printf("profile 警告：%s\n", prof.error.c_str());
        std::printf("profile：%s（%s）\n", prof.name.c_str(), prof.path.c_str());
        paths.insert(paths.begin(), prof.plugins.begin(), prof.plugins.end());
    }
    // mods 扫描（约定式装载面，见 scan_mods 注释）在空清单判定之前——裸宿主 +
    // 空 mods 仍拒绝启动，有 mod 则放行。selftest 跳过：验收剧本对在场插件集
    // 敏感，mod 的偶发在场不得影响自动化判定。
    if (!selftest) {
        const std::filesystem::path mods_dir =
            ithaca::profile::exe_dir(argv[0]) / "mods";
        const std::vector<std::string> mods = scan_mods(mods_dir, paths);
        if (!mods.empty()) {
            std::printf("mods：%zu 个（%s）\n", mods.size(),
                        mods_dir.string().c_str());
            paths.insert(paths.end(), mods.begin(), mods.end());
        }
    }
    if (paths.empty()) {
        std::printf("%s例：npm run dev（webui 开发链）或 scripts/启动宿主.cmd（桌面链）。\n",
                    kUsage);
        return 2;
    }

    // 标准日志行：时间戳 + 级别名 + 消息（插件消息自带 "ithaca.xxx: " 来源前缀，
    // 宿主自身的生命周期行同走此出口——控制台/host.log 单一格式）。级别名与内核
    // context.hpp 的档位名对齐（0-4 = TRACE/DEBUG/INFO/WARN/ERROR）
    const auto log_line = [](int level, std::string_view msg) {
        using clock = std::chrono::system_clock;
        const auto tp = clock::now();
        const std::time_t t = clock::to_time_t(tp);
        std::tm tm{};
        localtime_s(&tm, &t);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            tp.time_since_epoch()).count() % 1000;
        static constexpr const char* kLevels[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
        const char* lv = (level >= 0 && level <= 4) ? kLevels[level] : "LOG";
        std::printf("%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] %.*s\n",
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                    tm.tm_min, tm.tm_sec, static_cast<int>(ms), lv,
                    static_cast<int>(msg.size()), msg.data());
        std::fflush(stdout);
    };

    nostos::Host<> host;
    host.set_log_sink(log_line);

    std::printf("== ithaca_host %s ==\n", selftest ? "selftest" : "交互");
    for (const auto& p : paths) log_line(2, "ithaca.host: 待加载 " + p);

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

    // 懒激活（内核宿主策略）：manifest 依赖未就绪的插件登记 PENDING，重估后自动拉起。
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
        st.m1_smoke_and_kick(host);     // workspace 冒烟 + M3 闭环场景点火（write→read→replace 编辑链）
    }

    // 主循环：poster 唤醒驱动——post 入队即唤醒，排水 + 懒激活重估；
    // 空闲时最多等 250ms。selftest 的三阶段（Run1 闭环→Run2 回灌→Cancel 取消）
    // 推进在 SelftestHarness::tick；
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
    // teardown 契约（内核 set_post_wake 注释）：先摘钩再关句柄——hook 指向的
    // wake_event 必须比桥活得久，摘钩收窄窗口、关句柄前摘钩保序
    host.abi_bridge().set_post_wake(nullptr, nullptr);
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
