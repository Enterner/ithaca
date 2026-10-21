// ithaca —— 桌面壳（Win32 + WebView2）：多宿主实例管理器（导航中继形态）。
//
// 职责（界面形态按用户拍板修订：无壳自绘 UI——真实界面 =
// webui 前端，壳只做窗口与生命周期）：
//   1. 会话 = 实例：每个会话一个 ithaca_host.exe（独立 ITHACA_WEBUI_HOME 数据
//      目录、独立端口、经 cmd.exe 受信父进程链拉起、归入 Job Object——壳退出
//      KILL_ON_JOB_CLOSE 全部收走；每 turn 已落盘，立即终止安全）。
//   2. 单一全窗 WebView2 加载**真实前端**（= 活动实例的 ui/dist 服务）；侧栏
//      会话清单由 webui 的 /api/shell/sessions 聚合（壳 manifest + 各实例磁盘
//      status.json/index.json/port——徽标数据不经过壳）。点击其他会话：前端
//      对存活实例直接整页导航；停止态经 WebMessage 通道请壳拉起，壳在实例
//      就绪后回发 navigate 消息，前端再整页导航过去（页面 = 始终同构、零自绘）。
//   3. 生命周期：启动零创建（manifest + 自动恢复上次活跃）；＋新会话 = 壳弹
//      原生选文件夹 → 新 manifest 行 → 拉起；删除 = 停实例 + 摘行 + 递归删
//      home；孤儿实例（壳崩溃残留）启动时凭 port 文件探测附着。
//   4. 凭据共享：实例以 ITHACA_WEBUI_CONFIG 指向共享 config.json（唯一写者是
//      设置面板 UI，运行期实例只读；实例差异项在各自 config-local.json 覆盖层
//      ——webui 侧实现）。
//
// 两种布局（自动识别：exe 旁有没有 ithaca_host.exe）：
//   便携模式 —— 发布包布局：宿主/插件/前端/数据全在 exe 旁，数据目录 = data\；
//   开发模式 —— 仓库布局：exe 在 build/<preset>/bin 下，根在三级之上。
//
// 约束：WebView2 用户数据目录必须在可写位置（便携 = data\webview；
// 开发 = .ithaca-data\webview）。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <objbase.h>
#include <shobjidl.h>
#include <dwmapi.h>

#include "WebView2.h"

#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <algorithm>

namespace {

bool g_portable = false;      // true：发布包布局（exe 旁即全部）；false：仓库开发布局
std::wstring g_bin;           // 宿主与插件所在目录
std::wstring g_webdir;        // 前端 dist 目录（页面由实例服务，此路径只注入 env）
std::wstring g_data;          // 数据目录（中央：manifest/共享 config/旧会话库/hosts）
HWND g_hwnd = nullptr;
HANDLE g_job = nullptr;       // 全部本壳拉起的宿主树（退出 KILL_ON_JOB_CLOSE 收走）
std::atomic<bool> g_run{true};
bool g_mock_mode = false;     // --mock 测试模式：实例只装载 llm_mock（默认只装 cloud）

enum { ST_STOPPED = 0, ST_STARTING = 1, ST_RUNNING = 2, ST_ERROR = 3 };

// ---- 实例表 ----------------------------------------------------------------
struct Inst {
    std::wstring key;        // manifest 键（= home 目录名 = 生成时的 sid）
    std::wstring ws;         // 工作区
    std::wstring home;       // <data>\hosts\<key>
    DWORD pid = 0;           // 本壳拉起的进程 id（孤儿附着 = 0，退出不动它）
    UINT port = 0;           // home\port 读到的实际端口（0 = 未就绪/未拉起）
    int state = ST_STOPPED;
};
std::vector<Inst> g_insts;   // 只在 UI 线程增改
CRITICAL_SECTION g_cs;       // worker 读实例表与 UI 写之间的粗粒度互斥
int g_page_port = 0;         // 页面当前加载的实例端口（删除自源实例时切换用）
std::wstring g_page_key;     // 页面当前实例的 manifest 键
bool g_ctrl_ready = false;   // WebView2 controller 就绪（TryNavigate 的两个条件之一）
int g_navigated = 0;         // 已完成首次导航（后续切换由消息通道驱动）
std::wstring g_pending_key;  // 等待就绪导航的实例键（空 = 无）
ICoreWebView2* g_web = nullptr;   // 页面（非拥有；controller 持有）
ICoreWebView2Controller* g_ctrl = nullptr;

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    const size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}
std::wstring ParentOf(const std::wstring& p, int times) {
    std::wstring r = p;
    for (int i = 0; i < times; ++i) {
        const size_t slash = r.find_last_of(L'\\');
        if (slash == std::wstring::npos) return r;
        r = r.substr(0, slash);
    }
    return r;
}
bool PathExists(const std::wstring& p) {   // 目录也算存在（web/dist 是目录）
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 诊断日志（data\shell.log）：壳是纯窗口程序，排障全靠它
void LogLine(const std::wstring& msg) {
    HANDLE f = CreateFileW((g_data + L"\\shell.log").c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{}; GetLocalTime(&st);
    wchar_t line[600]{};
    swprintf_s(line, L"[%02u:%02u:%02u] %s\r\n", st.wHour, st.wMinute, st.wSecond, msg.c_str());
    DWORD w = 0;
    WriteFile(f, line, (DWORD)(wcslen(line) * 2), &w, nullptr);
    CloseHandle(f);
}

// ---- 小文件读写（UTF-8 ⇄ wide；manifest/port/index 皆小文件）----------------
std::wstring ReadSmallFile(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::string buf;
    char tmp[4096]; DWORD n = 0;
    while (ReadFile(f, tmp, sizeof(tmp), &n, nullptr) && n) buf.append(tmp, n);
    CloseHandle(f);
    if (buf.empty()) return L"";
    int w = MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)buf.size(), nullptr, 0);
    std::wstring out(static_cast<size_t>(w), 0);
    MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)buf.size(), out.data(), w);
    return out;
}
bool WriteSmallFile(const std::wstring& path, const std::wstring& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                      nullptr, 0, nullptr, nullptr);
    std::string u8(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), u8.data(), n, nullptr, nullptr);
    DWORD w = 0;
    WriteFile(f, u8.data(), (DWORD)u8.size(), &w, nullptr);
    CloseHandle(f);
    return true;
}

// 递归删目录（删除会话时清理实例 home；FindFirstFileW 手工后序）
void RemoveDirRec(const std::wstring& dir) {
    const std::wstring pat = dir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirRec(full);
            else DeleteFileW(full.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// ---- 布局解析：便携（发布包）/ 开发（仓库）双模式 ----------------------------
void ResolveLayout() {
    const std::wstring exe = ExeDir();
    if (PathExists(exe + L"\\ithaca_host.exe") &&
        PathExists(exe + L"\\web\\dist\\index.html")) {
        g_portable = true;
        g_bin     = exe;
        g_webdir  = exe + L"\\web\\dist";
        g_data    = exe + L"\\data";
        return;
    }
    // 开发模式：exe 在 build/<preset>/bin 下，仓库根在三级之上
    g_bin    = exe;
    g_webdir = ParentOf(exe, 3) + L"\\ui\\dist";
    g_data   = ParentOf(exe, 3) + L"\\.ithaca-data";
}

// ---- manifest（壳私有状态，tab 分隔行）--------------------------------------
// 行格式：key\tworkspace；首行特殊：ACTIVE\tkey（上次活跃，启动自动恢复那一个）。
// key = home 目录名；实例内的真实会话 id 由 webui 聚合端点从 index.json 读出。
void ManifestLoad(std::wstring& last_active) {
    const std::wstring text = ReadSmallFile(g_data + L"\\shell-manifest.txt");
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find(L'\n', pos);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = text.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (line.empty()) continue;
        const size_t t1 = line.find(L'\t');
        if (t1 == std::wstring::npos) continue;
        const std::wstring a = line.substr(0, t1);
        const std::wstring b = line.substr(t1 + 1);
        if (a == L"ACTIVE") { last_active = b; continue; }
        Inst it;
        it.key = a;
        it.ws = b;
        it.home = g_data + L"\\hosts\\" + a;
        g_insts.push_back(it);
    }
}
void ManifestSave() {
    std::wstring out;
    for (const auto& i : g_insts) out += i.key + L"\t" + i.ws + L"\r\n";
    WriteSmallFile(g_data + L"\\shell-manifest.txt", out);
}

// 中央 index.json 的会话条目块（迁移用）：定位 "id":"<sid>" 所在的 { ... }（条目
// 为平面对象、无嵌套花括号，朴素括号匹配即安全）
std::wstring ExtractSessionBlock(const std::wstring& index_text, const std::wstring& sid) {
    const std::wstring key = L"\"id\":\"" + sid + L"\"";
    const size_t k = index_text.find(key);
    if (k == std::wstring::npos) return L"";
    size_t b = index_text.rfind(L'{', k);
    if (b == std::wstring::npos) return L"";
    int depth = 0;
    for (size_t i = b; i < index_text.size(); ++i) {
        if (index_text[i] == L'{') ++depth;
        else if (index_text[i] == L'}' && --depth == 0) return index_text.substr(b, i - b + 1);
    }
    return L"";
}

// ---- HTTP（WinHTTP 短超时，worker 线程专用）---------------------------------
bool HttpGetOk(UINT port, const wchar_t* path) {
    HINTERNET ses = WinHttpOpen(L"ithaca-shell/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses == nullptr) return false;
    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", port, 0);
    bool ok = false;
    if (con != nullptr) {
        HINTERNET req = WinHttpOpenRequest(con, L"GET", path, nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        WinHttpSetTimeouts(req, 900, 900, 900, 900);
        if (req != nullptr) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE |
                                         WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &size, WINHTTP_NO_HEADER_INDEX);
                ok = status == 200;
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return ok;
}
void ShutdownPort(UINT port) {   // 优雅停宿主（每 turn 已落盘；失败由 Job 兜底）
    HINTERNET ses = WinHttpOpen(L"ithaca-shell/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses == nullptr) return;
    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", port, 0);
    if (con != nullptr) {
        HINTERNET req = WinHttpOpenRequest(con, L"POST", L"/api/shutdown", nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        WinHttpSetTimeouts(req, 1200, 1200, 1200, 1200);
        if (req != nullptr) {
            WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
            WinHttpReceiveResponse(req, nullptr);
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
}

UINT ReadPortFile(const std::wstring& home) {
    const std::wstring t = ReadSmallFile(home + L"\\port");
    return t.empty() ? 0 : (UINT)wcstoul(t.c_str(), nullptr, 10);
}

// ---- worker 线程：STARTING 实例的端口文件轮询 + RUNNING 实例健康自愈 ---------
// 就绪即 PostMessage(UWM_PORT_READY)：UI 线程导航页面 + 记账。健康检查失败
// 连续 3 拍 ⇒ 回 STOPPED 并删 port 文件（聚合端点随即将该行显示为"已停止"）。
constexpr UINT UWM_PORT_READY = WM_APP + 1;   // wp=idx lp=port
constexpr UINT UWM_SELF_STOP  = WM_APP + 2;   // wp=idx（实例死亡自愈）

void WorkerLoop(HWND hwnd) {
    int health_miss = 0;
    while (g_run.load()) {
        EnterCriticalSection(&g_cs);
        for (size_t i = 0; i < g_insts.size(); ++i) {
            Inst& it = g_insts[i];
            if (it.state == ST_STARTING) {
                const UINT p = ReadPortFile(it.home);
                if (p != 0) PostMessageW(hwnd, UWM_PORT_READY, (WPARAM)i, (LPARAM)p);
            } else if (it.state == ST_RUNNING && it.port != 0) {
                if (!HttpGetOk(it.port, L"/api/health")) {
                    if (++health_miss >= 3) {
                        health_miss = 0;
                        PostMessageW(hwnd, UWM_SELF_STOP, (WPARAM)i, 0);
                    }
                } else {
                    health_miss = 0;
                }
            }
        }
        LeaveCriticalSection(&g_cs);
        for (int i = 0; i < 20 && g_run.load(); ++i)   // 2s 睡眠（可中断）
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ---- 实例 spawn（cmd.exe 受信链 + 每实例 env + Job 托管 + 懒迁移）-----------
std::wstring BuildEnvBlock(const std::vector<std::pair<std::wstring, std::wstring>>& ov) {
    std::vector<std::pair<std::wstring, std::wstring>> kv;
    LPWCH es = GetEnvironmentStringsW();
    if (es != nullptr) {
        LPWCH p = es;
        while (*p) {
            const std::wstring e(p);
            p += e.size() + 1;
            const size_t eq = e.find(L'=');
            if (eq != std::wstring::npos && eq > 0)
                kv.push_back({e.substr(0, eq), e.substr(eq + 1)});
        }
        FreeEnvironmentStringsW(es);
    }
    for (const auto& o : ov) {
        bool hit = false;
        for (auto& k : kv)
            if (_wcsicmp(k.first.c_str(), o.first.c_str()) == 0) { k.second = o.second; hit = true; break; }
        if (!hit) kv.push_back(o);
    }
    // Unicode 环境块必须按名排序（GetEnvironmentStringsW 本就有序，覆盖合并会
    // 打乱 ⇒ CreateProcessW 报 gle=87 参数无效，踩过）
    std::sort(kv.begin(), kv.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0;
    });
    std::wstring out;
    for (const auto& k : kv) out += k.first + L"=" + k.second, out.append(1, L'\0');
    out.append(1, L'\0');
    return out;
}

// 旧会话懒迁移：中央库的 transcript 搬进实例 home，实例自己的 index.json 由中央
// index 的该条目原文本构成（首次拉起该会话时执行一次）
void MigrateLegacySession(const Inst& it) {
    if (PathExists(it.home + L"\\index.json")) return;   // 已迁移/已初始化（踩过：残留 sessions 目录不可作判据）
    const std::wstring src = g_data + L"\\sessions\\" + it.key + L".transcript.jsonl";
    if (!PathExists(src)) return;
    // NOTE: CreateDirectoryW 不建中间目录——home 整个不存在时（首次迁移）逐级建，
    // 否则 MoveFile/写 index 全部静默失败（踩过：rm 掉 home 后迁移"成功"却零产物）
    CreateDirectoryW((g_data + L"\\hosts").c_str(), nullptr);
    CreateDirectoryW(it.home.c_str(), nullptr);
    CreateDirectoryW((it.home + L"\\sessions").c_str(), nullptr);
    MoveFileW(src.c_str(), (it.home + L"\\sessions\\" + it.key + L".transcript.jsonl").c_str());
    std::wstring block = ExtractSessionBlock(ReadSmallFile(g_data + L"\\index.json"), it.key);
    if (block.empty()) {
        // 工作区路径含裸反斜杠，必须 JSON 转义（\C 非法转义 ⇒ 整个 index 解析
        // 失败被丢弃 ⇒ 实例永远空会话，踩过）
        std::wstring wse;
        for (wchar_t c : it.ws) {
            if (c == L'\\') wse += L"\\\\";
            else wse += c;
        }
        block = L"{\"id\":\"" + it.key + L"\",\"title\":\"(会话)\",\"workspace\":\"" +
                wse + L"\",\"updated\":0,\"turns\":1}";   // turns>0：否则激活即被
        // webui 的未初始化清扫删条目+transcript（sweep 只认 turns）
    }
    WriteSmallFile(it.home + L"\\index.json",
                   L"{\"active\":\"" + it.key + L"\",\"sessions\":[" + block + L"]}");
    LogLine(L"migrated legacy session " + it.key);
}

void SpawnInstance(size_t idx) {
    Inst& it = g_insts[idx];
    it.state = ST_STARTING;
    // 残留 port 文件必是 stale（新宿主会写新端口）——不删则 STARTING 轮询读到旧值
    // 假 ready ⇒ 导航死端口 ⇒ 自愈又误删新宿主的真 port 文件（踩过的竞态）
    DeleteFileW((it.home + L"\\port").c_str());
    MigrateLegacySession(it);
    CreateDirectoryW((g_data + L"\\hosts").c_str(), nullptr);
    CreateDirectoryW(it.home.c_str(), nullptr);
    CreateDirectoryW((it.home + L"\\sessions").c_str(), nullptr);

    // 供应商模式决定插件装载（产品拍板：mock 仅测试模式，两插件互斥存在——
    // 竞争性同名服务的热切竞态随双装载一起消失）：--mock 只装 mock，默认只装 cloud
    const wchar_t* plugin_names[] = {
        g_mock_mode ? L"llm_mock_plugin.dll" : L"llm_cloud_plugin.dll",
        L"workspace_plugin.dll",
        L"runner_plugin.dll", L"agent_plugin.dll", L"mcp_plugin.dll", L"webui_plugin.dll" };
    std::wstring cmdline = L"cmd.exe /d /s /c \"\"" + g_bin + L"\\ithaca_host.exe\"";
    for (const wchar_t* pn : plugin_names) {
        // 插件位置两布局都要认：打包 = exe 旁 plugins\ 子目录；开发构建 = 平铺（踩过）
        std::wstring pd = g_bin + L"\\plugins\\" + pn;
        if (!PathExists(pd)) pd = g_bin + L"\\" + pn;
        if (PathExists(pd)) cmdline += L" \"" + pd + L"\"";
    }
    cmdline += L"\"";

    // 每实例 env（CreateProcessW 独立 env 块；env 块被系统拒时回退进程级继承——
    // spawn 全在 UI 线程顺序发生，设置后立即 CreateProcess 无串台窗口）
    std::wstring env = BuildEnvBlock({
        {L"ITHACA_WEBUI_HOME", it.home},
        {L"ITHACA_WEBUI_WEB", g_webdir},
        {L"ITHACA_WORKSPACE_DIR", it.ws},   // 空工作区 = 空值条目：webui 视同未设 → hero 引导
        {L"ITHACA_WEBUI_CONFIG", g_data + L"\\config.json"},
        {L"ITHACA_WEBUI_PORT", L"0"},
        {L"ITHACA_SHELL_MANIFEST", g_data + L"\\shell-manifest.txt"},
        {L"ITHACA_LLM_ACTIVE", g_mock_mode ? L"mock" : L"cloud"},   // 模式显式化（webui 面板单侧显示的依据）
    });

    const std::wstring log = it.home + L"\\host.log";
    HANDLE host_log = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                  nullptr, OPEN_ALWAYS, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = si.hStdOutput = si.hStdError = host_log;
    PROCESS_INFORMATION pi{};
    // NOTE: cmd.exe 受信父进程链不可省——企业管控按父链拦截未签名进程的子树（踩过）
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             DETACHED_PROCESS, env.data(), g_bin.c_str(), &si, &pi);
    if (!ok) {
        LogLine(L"envblock spawn failed gle=" + std::to_wstring(GetLastError()) +
                L" key=" + it.key + L" —— 回退进程级 env");
        SetEnvironmentVariableW(L"ITHACA_WEBUI_HOME", it.home.c_str());
        SetEnvironmentVariableW(L"ITHACA_WEBUI_WEB", g_webdir.c_str());
        if (!it.ws.empty())
            SetEnvironmentVariableW(L"ITHACA_WORKSPACE_DIR", it.ws.c_str());
        else
            SetEnvironmentVariableW(L"ITHACA_WORKSPACE_DIR", nullptr);
        SetEnvironmentVariableW(L"ITHACA_WEBUI_CONFIG", (g_data + L"\\config.json").c_str());
        SetEnvironmentVariableW(L"ITHACA_WEBUI_PORT", L"0");
        SetEnvironmentVariableW(L"ITHACA_SHELL_MANIFEST",
                                (g_data + L"\\shell-manifest.txt").c_str());
        SetEnvironmentVariableW(L"ITHACA_LLM_ACTIVE", g_mock_mode ? L"mock" : L"cloud");
        ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                            DETACHED_PROCESS, nullptr, g_bin.c_str(), &si, &pi);
    }
    if (host_log != INVALID_HANDLE_VALUE) CloseHandle(host_log);
    if (!ok) {
        it.state = ST_ERROR;
        LogLine(L"spawn failed gle=" + std::to_wstring(GetLastError()) + L" key=" + it.key);
        return;
    }
    CloseHandle(pi.hThread);
    if (g_job != nullptr) AssignProcessToJobObject(g_job, pi.hProcess);   // 退出随壳收走
    it.pid = pi.dwProcessId;
    CloseHandle(pi.hProcess);
    LogLine(L"spawned key=" + it.key + L" pid=" + std::to_wstring(it.pid));
}

// ---- WebView2（单 controller 全窗；页面 = 真实前端）-------------------------
template <typename Derived, typename Base>
struct ComBase : Base {
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete static_cast<Derived*>(this);
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(Base)) {
            *ppv = static_cast<Base*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    unsigned long ref_ = 1;
};

RECT ViewRect() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    return rc;
}

UINT g_deferred_port = 0;   // controller 就绪前请求的导航挂起在这（完成回调里补偿，
                            // 踩过：孤儿附着/快速就绪时 NavigatePage 撞 g_web==null
                            // 静默丢失 ⇒ 黑屏）
UINT g_last_good_port = 0;   // 最后一次导航成功的实例端口（导航失败自愈的回退目标）

void NavigatePage(UINT port) {
    if (port == 0) return;
    if (g_web == nullptr) {
        g_deferred_port = port;   // controller 未完成：挂起，就绪回调里补
        LogLine(L"page nav deferred -> " + std::to_wstring(port));
        return;
    }
    // ?v= 缓存穿透：bundle 名带 hash，但 index.html 本身被 webview 强缓存、依旧
    // 指向旧 hash——导航 URL 必须带每次变化的 ?v=（踩过：页面跑旧 JS，新前端功能全静默）
    const std::wstring url = L"http://127.0.0.1:" + std::to_wstring(port) +
                             L"/?v=" + std::to_wstring(GetTickCount64());
    g_web->Navigate(url.c_str());
    LogLine(L"page -> " + url);
}

// 实例就绪回调（worker 的 UWM_PORT_READY，UI 线程）：首次导航 = 页面加载；此后
// 按 g_pending_key 导航（激活停止态会话时页面停在旧实例上，等新实例就绪切过去）
void OnPortReady(size_t idx, UINT port) {
    if (idx >= g_insts.size()) return;
    Inst& it = g_insts[idx];
    it.port = port;
    it.state = ST_RUNNING;
    LogLine(L"instance ready key=" + it.key + L" port=" + std::to_wstring(port));
    if (!g_navigated || g_pending_key == it.key) {
        g_pending_key.clear();
        g_page_port = (int)port;
        g_page_key = it.key;
        NavigatePage(port);
        g_navigated = 1;
    }
}

// 页面 → 壳消息（activate / new-session / delete；字段值为简单 sid，无转义）
// 的派发（定义在前向声明之后）
void ActivateKey(const std::wstring& key);
void NewSessionFlow(const std::wstring& ws);
void DeleteFlow(const std::wstring& key);
std::wstring BootstrapKey();

struct PageMessageHandler
    : ComBase<PageMessageHandler, ICoreWebView2WebMessageReceivedEventHandler> {
    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        // NOTE: 前端 postMessage(obj) 传对象——TryGetWebMessageAsString 只认字符串
        //（对象消息它会失败），必须走 get_WebMessageAsJson（踩过：TryGet 静默失败
        // ⇒ 消息通道全哑且无诊断）
        PWSTR raw = nullptr;
        if (FAILED(args->get_WebMessageAsJson(&raw)) || raw == nullptr) return S_OK;
        const std::wstring msg = raw;
        CoTaskMemFree(raw);
        LogLine(L"page msg: " + msg);   // 消息通道诊断
        auto field = [&msg](const wchar_t* key) -> std::wstring {
            const std::wstring k = std::wstring(L"\"") + key + L"\":\"";
            const size_t p = msg.find(k);
            if (p == std::wstring::npos) return L"";
            const size_t b = p + k.size();
            const size_t e = msg.find(L'"', b);
            return e == std::wstring::npos ? L"" : msg.substr(b, e - b);
        };
        const std::wstring type = field(L"type");
        if (type == L"activate") {
            ActivateKey(field(L"key"));
        } else if (type == L"new-session") {
            NewSessionFlow(field(L"ws"));   // 工作区由页内选目录流程决定
        } else if (type == L"delete") {
            DeleteFlow(field(L"key"));
        }
        return S_OK;
    }
};

struct ControllerHandler
    : ComBase<ControllerHandler, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctrl) override {
        if (FAILED(hr) || ctrl == nullptr) return E_FAIL;
        g_ctrl = ctrl;
        g_ctrl->AddRef();
        ICoreWebView2* web = nullptr;
        if (SUCCEEDED(g_ctrl->get_CoreWebView2(&web)) && web != nullptr) {
            g_web = web;
            EventRegistrationToken tok{};
            g_web->add_WebMessageReceived(new PageMessageHandler(), &tok);
            // 导航诊断（间歇白屏排查：确认 webview 内部导航是否真正完成）
            EventRegistrationToken t2{};
            g_web->add_NavigationCompleted(new NavLogHandler(), &t2);
        }
        g_ctrl->put_Bounds(ViewRect());
        g_ctrl->put_IsVisible(TRUE);
        g_ctrl_ready = true;
        if (g_deferred_port != 0) {
            const UINT p = g_deferred_port;
            g_deferred_port = 0;
            NavigatePage(p);   // 补偿 controller 就绪前丢失的导航
        }
        return S_OK;
    }
    struct NavLogHandler
        : ComBase<NavLogHandler, ICoreWebView2NavigationCompletedEventHandler> {
        HRESULT STDMETHODCALLTYPE Invoke(
            ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) override {
            BOOL ok = FALSE;
            args->get_IsSuccess(&ok);
            PWSTR uri = nullptr;
            if (SUCCEEDED(sender->get_Source(&uri)) && uri != nullptr) {
                LogLine(std::wstring(L"nav ") + (ok ? L"ok " : L"FAIL ") + uri);
                CoTaskMemFree(uri);
            }
            // 导航失败自愈：目标实例死亡（探测窗口内刚死）⇒ 页面停在错误页无法
            // 操作——自动回退到上一个健康实例的页面，侧栏仍可继续切换
            if (!ok && g_last_good_port != 0) NavigatePage(g_last_good_port);
            else if (ok && g_page_port != 0) g_last_good_port = (UINT)g_page_port;
            return S_OK;
        }
    };

};

struct EnvironmentHandler
    : ComBase<EnvironmentHandler, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment* env) override {
        if (FAILED(hr) || env == nullptr) {
            MessageBoxW(g_hwnd,
                        L"WebView2 环境创建失败——请安装/修复 Microsoft Edge WebView2 Runtime。",
                        L"Ithaca", MB_ICONERROR);
            PostQuitMessage(1);
            return E_FAIL;
        }
        return env->CreateCoreWebView2Controller(g_hwnd, new ControllerHandler());
    }
};

// ---- 实例激活/新建/删除（UI 线程）-------------------------------------------
Inst* FindByKey(const std::wstring& key) {
    for (auto& it : g_insts)
        if (it.key == key) return &it;
    return nullptr;
}

void ActivateKey(const std::wstring& key) {
    Inst* it = FindByKey(key);
    if (it == nullptr) return;
    if (it->state == ST_RUNNING && it->port != 0) {
        // NOTE: 直航前健康复查——state 可能落后于宿主真实状态（死亡瞬间/探测
        // 窗口），盲航会落到死端口错误页（踩过：nav FAIL 循环）。不健康 ⇒ 按
        // 停止态重新拉起。
        if (!HttpGetOk(it->port, L"/api/health")) {
            it->state = ST_STOPPED;
            it->port = 0;
            DeleteFileW((it->home + L"\port").c_str());
        } else {
            g_page_port = (int)it->port;
            g_page_key = it->key;
            NavigatePage(it->port);
            return;
        }
    }
    if (it->state == ST_STOPPED || it->state == ST_ERROR) {
        g_pending_key = key;
        SpawnInstance((size_t)(it - &g_insts[0]));
    }
    // ST_STARTING：等 worker ready 回调导航（g_pending_key 已指向这里）
    if (g_pending_key.empty()) g_pending_key = key;
}

void NewSessionFlow(const std::wstring& ws) {
    if (ws.empty()) return;
    Inst it;
    it.key = L"s" + std::to_wstring(GetTickCount64());
    it.ws = ws;
    it.home = g_data + L"\\hosts\\" + it.key;
    g_insts.push_back(it);
    ManifestSave();
    LogLine(L"new session key=" + it.key + L" ws=" + ws);
    ActivateKey(it.key);
}

void DeleteFlow(const std::wstring& key) {
    Inst* it = FindByKey(key);
    if (it == nullptr) return;
    if (it->port != 0) ShutdownPort(it->port);
    const std::wstring home = it->home;
    const bool was_page = g_page_key == key;
    g_insts.erase(g_insts.begin() + (it - &g_insts[0]));
    ManifestSave();
    RemoveDirRec(home);
    LogLine(L"deleted session key=" + key);
    if (was_page) {   // 删的是页面自源实例：切到任一存活实例，没有则现拉一个
        int target = -1;
        for (size_t i = 0; i < g_insts.size(); ++i)
            if (g_insts[i].state == ST_RUNNING && g_insts[i].port != 0) { target = (int)i; break; }
        if (target >= 0) {
            g_page_key = g_insts[(size_t)target].key;
            NavigatePage(g_insts[(size_t)target].port);
        } else {
            g_navigated = 0;   // 无家可归：走 bootstrap（拉空工作区实例进 hero）
            ActivateKey(BootstrapKey());
        }
    }
}

// bootstrap 键：无任何实例时的空工作区实例（hero 引导；首个会话由实例自己创建，
// 真实会话 id 由聚合端点从 home\index.json 读出——manifest key 只是 home 名）
std::wstring BootstrapKey() {
    Inst it;
    it.key = L"s" + std::to_wstring(GetTickCount64());
    it.home = g_data + L"\\hosts\\" + it.key;
    g_insts.push_back(it);
    ManifestSave();
    LogLine(L"bootstrap instance key=" + it.key);
    SpawnInstance(g_insts.size() - 1);
    g_pending_key = it.key;
    return it.key;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;   // 全窗 WebView2，免闪烁
        case WM_SIZE:
            if (g_ctrl != nullptr && wp != SIZE_MINIMIZED) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                g_ctrl->put_Bounds(rc);
            }
            return 0;
        case UWM_PORT_READY:
            OnPortReady((size_t)wp, (UINT)lp);
            return 0;
        case UWM_SELF_STOP: {
            Inst& it = g_insts[(size_t)wp];
            it.state = ST_STOPPED;
            it.port = 0;
            DeleteFileW((it.home + L"\\port").c_str());   // 聚合端点随即显示"已停止"
            LogLine(L"instance unhealthy key=" + it.key + L" -> stop");
            return 0;
        }
        case WM_DESTROY:
            g_run.store(false);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    // Per-Monitor-V2 DPI 感知：WebView2 按物理像素渲染。缺省时系统按 96dpi
    // 渲染再拉伸到屏幕缩放倍数 ⇒ 全屏/缩放下文字发糊（踩过）
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // --mock = 测试模式：实例只装载 llm_mock（默认 cloud——两 llm 插件互斥装载，
    // 模型配置面板随之只显示当前模式一项）
    g_mock_mode = lpCmdLine != nullptr && std::wstring(lpCmdLine).find(L"--mock") != std::wstring::npos;
    ResolveLayout();
    CreateDirectoryW(g_data.c_str(), nullptr);
    CreateDirectoryW((g_data + L"\\hosts").c_str(), nullptr);
    LogLine(g_portable ? L"layout=portable" : L"layout=dev");

    InitializeCriticalSection(&g_cs);
    // manifest 恢复实例表 + 孤儿探测（port 文件仍健康的直接附着，不重复 spawn）
    std::wstring last_active;
    ManifestLoad(last_active);
    for (auto& it : g_insts) {
        const UINT p = ReadPortFile(it.home);
        if (p != 0 && HttpGetOk(p, L"/api/health")) {
            it.port = p;
            it.state = ST_RUNNING;   // 孤儿附着：pid=0，退出时不动它
            LogLine(L"attach orphan key=" + it.key + L" port=" + std::to_wstring(p));
        }
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"ithaca";
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, (LPCWSTR)(ULONG_PTR)1);
    if (wc.hIcon == nullptr) wc.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"ithaca", L"Ithaca", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820,
                              nullptr, nullptr, hInst, nullptr);
    g_hwnd = hwnd;
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        const COLORREF caption = 0x1F1712;   // #12171F 面板色（Nebula 主题）
        const COLORREF text    = 0xF2EAE6;   // #E6EAF2 主题文字
        const COLORREF border  = 0x3B2B22;   // #222B3B 描边
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    }
    ShowWindow(hwnd, nCmdShow);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // 静默 WebView2 运行时内部资源的 libpng 解码告警（tRNS+alpha 噪音，与应用无关）。
    // 不覆盖外部已设的参数，缺则补。
    {
        std::wstring args = _wgetenv(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS") != nullptr
                                ? _wgetenv(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS") : L"";
        if (args.find(L"--disable-logging") == std::wstring::npos)
            args += args.empty() ? L"--disable-logging" : L" --disable-logging";
        SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", args.c_str());
    }
    const std::wstring udf = g_data + L"\\webview";
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, udf.c_str(), nullptr, new EnvironmentHandler());
    if (FAILED(hr)) {
        MessageBoxW(nullptr,
                    L"WebView2 运行时不可用（Windows 10/11 一般自带）。\n"
                    L"请安装 Microsoft Edge WebView2 Runtime 后重试。",
                    L"Ithaca", MB_ICONERROR);
        return 1;
    }

    // 启动零创建 + 自动恢复上次活跃；无实例 = bootstrap（空工作区实例进 hero 引导）
    if (!g_insts.empty()) {
        int start = -1;
        for (size_t i = 0; i < g_insts.size(); ++i)
            if (g_insts[i].key == last_active) { start = (int)i; break; }
        if (start < 0) start = 0;
        ActivateKey(g_insts[(size_t)start].key);
    } else {
        BootstrapKey();
    }

    std::thread worker(WorkerLoop, hwnd);
    worker.detach();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // 退出收走本壳拉起的实例：先逐个优雅停（数据已落盘），Job Kill 兜底；
    // 孤儿附着（pid=0）的不动它
    for (auto& it : g_insts)
        if (it.pid != 0 && it.port != 0) ShutdownPort(it.port);
    if (g_job != nullptr) CloseHandle(g_job);   // KILL_ON_JOB_CLOSE：残余树全部终止
    return 0;
}
