// plugin/webui —— Web 前端插件（正式 UI，方案 docs/webui-方案.md）。
//
// 职责：kit::http serve SPA + REST + SSE；订阅事件总线（tui 同款事件面）⇒
// JSONL 持久化 + SSE 广播；输入/裁决经 poster 回总线。会话持久化的两层：
//   · transcript.jsonl（本插件写，展示事件流，每条 flush）
//   · state.json（agent 写，LLM 上下文；路径由启动器按活跃会话给定）
//
// 线程纪律（方案 §5）：Handler::handle 在 select 线程（必须快——poster_submit/
// 小文件写都非阻塞）；事件回调在宿主线程（drain_posted 串行化）；broadcast
// 内核注明线程安全。锁：无（各线程只触碰自己的数据面）。

#include "vc_events.hpp"
#include "vc_llm.hpp"
#include "vc_tool.hpp"  // vc_api::ToolTable：文件树代理调用的 workspace 工具表形状

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/http.hpp>
#include <nostos/kit/poster.hpp>

#include "../workspace/iface.hpp"  // vcws::kToolSvc/kToolSvcName：workspace 工具表的稳定契约名
#include "../workspace/text.hpp"  // vcws::trim_utf8：码点安全截断（F15 收敛点）

#include <objbase.h>
#include <winhttp.h>   // CoInitializeEx / CoTaskMemFree：原生文件夹选择对话框
#include <shlobj.h>    // SHCreateItemFromParsingName：选目录对话框起始目录
#include <shobjidl.h>  // IFileDialog + FOS_PICKFOLDERS：现代选目录对话框

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace http = nostos::kit::http;

namespace {

using json = nlohmann::json;

// ---- 原生选目录对话框（UI 原语，自内核 kit::win 下沉到唯一消费方；置顶修复随迁）--

inline std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring wide(need > 0 ? static_cast<std::size_t>(need) : 0, L'\0');
    if (!wide.empty())
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                            need);
    return wide;
}

inline std::string wide_to_utf8(const wchar_t* text) {
    if (text == nullptr) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};  // 只剩结尾 NUL = 空串
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), need, nullptr, nullptr);
    return out;
}

// 对话框定位状态（in_flight 门卫保证同时最多一个；机制见 pick_folder_dialog 注释）
struct PickerLayout {
    HWND dlg = nullptr;              // 对话框顶层窗口（HCBT_ACTIVATE 时捕获）
    RECT work = {};                  // 目标显示器工作区（激活时的前台窗口所在屏）
    unsigned __int64 t_activate = 0; // 激活时刻——重居中只在此后短时限内生效
    bool shown = false;              // 已执行延迟显示
    bool view_seen = false;          // SHELLDLL_DefView（视图）已出现
};
PickerLayout g_picker;

// 居中到 g_picker.work；show=true 时同时置顶显示
inline void picker_center(HWND hwnd, bool show) {
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return;
    const int x = g_picker.work.left +
                  ((g_picker.work.right - g_picker.work.left) - (rc.right - rc.left)) / 2;
    const int y = g_picker.work.top +
                  ((g_picker.work.bottom - g_picker.work.top) - (rc.bottom - rc.top)) / 2;
    if (show)
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    else
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// WinEvent 兜底（机制见 pick_folder_dialog 注释）：等视图就绪显示 + 时限内重居中
void CALLBACK picker_win_event(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_obj,
                               LONG, DWORD, DWORD) {
    if (g_picker.dlg == nullptr || id_obj != OBJID_WINDOW) return;
    if (event == EVENT_OBJECT_SHOW) {
        if (!g_picker.view_seen && GetAncestor(hwnd, GA_ROOT) == g_picker.dlg) {
            wchar_t cls[64] = {};
            GetClassNameW(hwnd, cls, 64);
            g_picker.view_seen = wcscmp(cls, L"SHELLDLL_DefView") == 0;
        }
    } else if (event == EVENT_OBJECT_LOCATIONCHANGE && hwnd == g_picker.dlg &&
               GetTickCount64() - g_picker.t_activate <= 1500) {
        picker_center(hwnd, false);   // 已居中 ⇒ 无实际位移 ⇒ 事件不再触发，自然收敛
    }
    // 视图就绪或 2s 超时 ⇒ 居中显示
    if (!g_picker.shown && (g_picker.view_seen ||
                            GetTickCount64() - g_picker.t_activate > 2000)) {
        g_picker.shown = true;
        picker_center(g_picker.dlg, true);
    }
}

// 弹出"选择文件夹"对话框（IFileDialog + FOS_PICKFOLDERS：Vista 现代样式，
// Explorer 同款面包屑/侧栏/搜索；老 SHBrowseForFolderW 是 Win95 树形对话框）。
// 返回选中绝对路径（UTF-8）；取消/失败返回空串。调用线程需 COM 已初始化。
//
// 宿主是无窗口进程，无主对话框有一串系统默认行为要逐个纠正，全部经本线程挂的
// CBT/WinEvent 钩子完成（真实踩过，细节见各分支）：
//   · 沉底（被浏览器页面盖住 ⇒ "点了没反应"）——激活瞬间置顶
//   · 不居中（落主屏左上级联；老 API 自带居中，迁移后才暴露）——按激活时的
//     前台窗口所在显示器居中
//   · 记忆边界回写（shell view 初始化后把注册表记忆的位置再套回来）——激活后
//     1.5s 内 WinEvent 持续重居中，收敛即停；时限外不干预用户手动拖动
//   · 空壳一闪（窗口先于 shell 视图内容上屏；壳扩展冷加载时间隙可达秒级）——
//     视图未就绪先隐藏，等 SHELLDLL_DefView 出现或 2s 超时再显示
// NOTE: MONITORINFO 必须先置 cbSize，否则 GetMonitorInfoW 必败
// （真实踩过：居中静默失效，31 次激活全部短路）。
inline std::string pick_folder_dialog(const std::string& title,
                                      const std::string& initial_dir = {}) {
    IFileDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fd))) || fd == nullptr)
        return {};
    std::string result;
    HHOOK cbt = SetWindowsHookExW(
        WH_CBT,
        [](int code, WPARAM wp, LPARAM lp) -> LRESULT {
            if (code == HCBT_ACTIVATE) {
                const HWND hwnd = reinterpret_cast<HWND>(wp);
                // MonitorFromWindow 携 DEFAULTTONEAREST 永不返回空，无需兜底
                HMONITOR mon = MonitorFromWindow(GetForegroundWindow(),
                                                 MONITOR_DEFAULTTONEAREST);
                RECT rc{};
                MONITORINFO info{sizeof(info)};   // cbSize 必须先置（踩过：居中静默失效）
                if (GetMonitorInfoW(mon, &info) && GetWindowRect(hwnd, &rc)) {
                    g_picker.dlg = hwnd;
                    g_picker.work = info.rcWork;
                    g_picker.t_activate = GetTickCount64();
                    if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr)
                        picker_center(hwnd, true);   // 视图已就绪：居中显示
                    else {
                        ShowWindow(hwnd, SW_HIDE);   // 延迟显示：等视图出现（WinEvent 兜底）
                        picker_center(hwnd, false);
                    }
                } else {
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                }
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        },
        nullptr, GetCurrentThreadId());
    do {
        DWORD opts = 0;
        if (FAILED(fd->GetOptions(&opts))) break;
        fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        fd->SetTitle(utf8_to_wide(title.empty() ? std::string("选择工作文件夹") : title).c_str());
        fd->SetOkButtonLabel(L"选择文件夹");
        if (!initial_dir.empty()) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(utf8_to_wide(initial_dir).c_str(), nullptr,
                                                      IID_PPV_ARGS(&item))) &&
                item != nullptr) {
                fd->SetDefaultFolder(item);   // 起始目录（不预选，只定位）
                item->Release();
            }
        }
        // 记忆边界回写兜底 + 视图就绪延迟显示（见 picker_win_event）
        g_picker.shown = false;
        g_picker.view_seen = false;
        const HWINEVENTHOOK winevent = SetWinEventHook(
            EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
            picker_win_event, GetCurrentProcessId(), GetCurrentThreadId(),
            WINEVENT_OUTOFCONTEXT);
        const HRESULT shown = fd->Show(nullptr);
        if (winevent != nullptr) UnhookWinEvent(winevent);
        if (SUCCEEDED(shown)) {
            IShellItem* sel = nullptr;
            if (SUCCEEDED(fd->GetResult(&sel)) && sel != nullptr) {
                PWSTR ws = nullptr;
                if (SUCCEEDED(sel->GetDisplayName(SIGDN_FILESYSPATH, &ws))) {
                    result = wide_to_utf8(ws);
                    CoTaskMemFree(ws);
                }
                sel->Release();
            }
        }
    } while (false);
    if (cbt != nullptr) UnhookWindowsHookEx(cbt);
    fd->Release();
    g_picker.dlg = nullptr;   // 对话框已关，WinEvent 兜底随之失效
    return result;
}

class WebuiPlugin : public http::Handler {
public:
    static constexpr std::string_view name = "vc.webui";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 1, 0);

    nostos_status activate(const nostos::abi::Host& host) {
        host_ = host;

        // 数据目录与会话（启动器给定；缺省兜底）
        const char* home = std::getenv("ITHACA_WEBUI_HOME");
        home_ = (home != nullptr && *home != '\0')
                    ? fs::path(home)
                    : fs::path(std::getenv("USERPROFILE") != nullptr
                                   ? std::getenv("USERPROFILE")
                                   : ".") /
                          ".ithaca";
        std::error_code ec;
        fs::create_directories(home_ / "sessions", ec);
        // ITHACA_WEBUI_SESSION 仅作注册表命中时的活跃会话优先项
        // （老启动器/dev 脚本仍在传）；未命中不产生任何会话
        std::string prefer_session;
        if (const char* v = std::getenv("ITHACA_WEBUI_SESSION"); v != nullptr && *v != '\0')
            prefer_session = v;
        if (const char* v = std::getenv("ITHACA_WEBUI_WEB"); v != nullptr && *v != '\0')
            web_root_ = fs::path(v);
        else
            web_root_ = fs::current_path() / "web";
        web_root_ = fs::weakly_canonical(web_root_, ec);

        load_config_();
        // 会话注册表：加载 + 未初始化条目清扫 + 活跃会话选择（注册表只含已初始化会话）
        if (const char* v = std::getenv("ITHACA_WORKSPACE_DIR"); v != nullptr && *v != '\0')
            host_workspace_ = v;
        load_index_();
        sweep_uninitialized_();
        {
            std::string act = prefer_session;
            if (act.empty() || session_entry_(act) == nullptr) {
                act = index_.value("active", "");
                if (act.empty() || session_entry_(act) == nullptr) {
                    long long best = -1;
                    for (const auto& s : index_["sessions"]) {
                        const long long u = s.is_object() ? s.value("updated", 0LL) : 0LL;
                        if (u > best) { best = u; act = s.value("id", ""); }
                    }
                    if (best < 0) act = "";
                }
            }
            session_ = act;
            if (!session_.empty()) {
                index_["active"] = session_;
                save_index_();
                load_transcript_();  // 容忍损坏尾行（崩溃安全）
                open_transcript_();
            }
            // session_ 为空 = 无会话态（首装/全删）：不落任何盘，前端经
            // /api/session 空 id 进草稿态，首条消息时才创建会话
        }
        // LlmTable 在 activate 时解析一次（桥在；service 走 TLS，select 线程上为空）
        llm_ = host_.service<vc_api::LlmTable>(vc_api::llm);
        if (llm_ == nullptr) host_.log(3, "ithaca.webui: 无 LLM provider——模型热切不可用");

        // 供应商模式（产品拍板：mock 仅测试模式）：ITHACA_LLM_ACTIVE = mock|cloud，
        // 缺省 cloud。两 llm 插件由启动器互斥装载，本值只决定配置面板的单侧显示
        // 与保存范围——config 磁盘数据保持全量（cloud 是用户数据，mock 测试不擦）
        llm_mode_ = [] {
            const char* v = std::getenv("ITHACA_LLM_ACTIVE");
            return (v != nullptr && std::string_view(v) == "mock") ? "mock" : "cloud";
        }();
        // workspace 工具表（文件树 /api/fs/list 的后端）：activate 时解析一次；
        // 装载顺序在 webui 之后的情况由 subscribe_ 的 tool_register 兜底重解析
        tools_ = host_.service<vc_api::ToolTable>(vcws::kToolSvc);
        if (tools_ == nullptr) host_.log(3, "ithaca.webui: workspace 工具表未就绪——文件树不可用");

        if (host.acquire_poster(&poster_) != NOSTOS_OK) {
            host_.log(4, "ithaca.webui: 领取 poster 失败");
            return NOSTOS_ERR_FAILED;
        }

        subscribe_();
        // 供应商两层配置：providers 数组——
        // 第一层供应商（名称/type/Base URL/API Key/各自的模型列表），第二层模型
        // （id/上下文窗口/enabled）。缺失 ⇒ 从旧配置一次性迁移
        // （provider/api_key/base_url/models/context_sizes → Mock 内置 + 云端供应商）。
        if (!config_.contains("providers") || !config_["providers"].is_array() ||
            config_["providers"].empty()) {
            const bool was_mock = config_.value("provider", "cloud") == "mock";
            const json legacy_sizes = config_.value("context_sizes", json::object());
            json cloud_models = config_.contains("models") && config_["models"].is_array()
                                    ? config_["models"] : json::array();
            if (cloud_models.empty())
                cloud_models = json::array({
                    json{{"id", "deepseek-flash"}, {"context", 128000}, {"enabled", true}},
                    json{{"id", "deepseek-v4-pro"}, {"context", 128000}, {"enabled", true}},
                    json{{"id", "deepseek-reasoner"}, {"context", 128000}, {"enabled", true}}});
            for (auto& m : cloud_models) {   // 旧 context_sizes 覆盖 context
                if (m.is_object() && m.contains("id") && m["id"].is_string()) {
                    const std::string mid = m["id"].get<std::string>();
                    if (legacy_sizes.contains(mid) && legacy_sizes[mid].is_number_integer())
                        m["context"] = legacy_sizes[mid];
                }
                if (!m.contains("enabled")) m["enabled"] = true;
            }
            json providers = json::array();
            providers.push_back(json{
                {"name", "Mock 本地回放"}, {"type", "mock"}, {"builtin", true},
                {"enabled", was_mock},
                {"models", json::array({json{{"id", "mock-model"}, {"context", 128000}, {"enabled", true}}})}});
            providers.push_back(json{
                {"name", "DeepSeek 官方"}, {"type", "cloud"},
                {"base_url", config_.value("base_url", "https://api.deepseek.com")},
                {"api_key", config_.value("api_key", "")},
                {"enabled", true}, {"models", cloud_models}});
            config_["providers"] = providers;
            config_["current_provider"] = was_mock ? "Mock 本地回放" : "DeepSeek 官方";
            config_.erase("context_sizes");
            config_.erase("models");
        }
        // NOTE: providers[].models 必须是数组——低 Depth 的 ConvertTo-Json（PS5.1
        // 启动器回写配置）会把它压成字符串，前端 cfgFromServer 对它 .map 直接抛
        // 异常、页面静默卡死。非数组一律按类型回种默认列表（踩过）。
        for (auto& p : config_["providers"]) {
            if (!p.is_object() || p["models"].is_array()) continue;
            if (p.value("type", "cloud") == "mock")
                p["models"] = json::array(
                    {json{{"id", "mock-model"}, {"context", 128000}, {"enabled", true}}});
            else
                p["models"] = json::array(
                    {json{{"id", "deepseek-flash"}, {"context", 128000}, {"enabled", true}},
                     json{{"id", "deepseek-v4-pro"}, {"context", 128000}, {"enabled", true}},
                     json{{"id", "deepseek-reasoner"}, {"context", 128000}, {"enabled", true}}});
        }
        // 启动对齐：当前使用供应商的 key/base_url 热切给 llm 插件（poster 异步）。
        // NOTE: 载荷以 llm_mode_ 为准（mock 模式恒发 mock）——config 的
        // current_provider 可能是另一模式的（共享 config 跨模式留存），对齐成
        // 另一模式会把本模式的插件休眠掉（踩过：mock 宿主启动即被对齐成 cloud
        // 休眠 ⇒ LLM 不可用）
        {
            const std::string cur = llm_mode_ == "mock"
                                        ? std::string("mock")
                                        : config_.value("current_provider", "");
            for (const auto& p : config_["providers"]) {
                if (!p.is_object() || p.value("name", "") != cur) continue;
                const std::string type = llm_mode_ == "mock"
                                             ? std::string("mock")
                                             : p.value("type", "cloud");
                const std::string key = p.value("api_key", "");
                const std::string burl = p.value("base_url", "");
                nostos::kit::poster_submit(poster_, [this, type, key, burl] {
                    const vc_api::LlmSetActive e{type.c_str(), key.c_str(),
                                                 burl.empty() ? nullptr : burl.c_str()};
                    host_.emit(vc_api::llm_set_active, &e);
                });
                break;
            }
        }
        // 直连/便携启动（桌面壳直发宿主，不经启动器注入 env）：按 config 工作区
        // 热切——空 = hero 引导（首次 assign 再切）；有 env 时 env 优先，不动。
        if (std::getenv("ITHACA_WORKSPACE_DIR") == nullptr &&
            config_.value("workspace", "") != "") {
            const std::string ws0 = config_.value("workspace", "");
            nostos::kit::poster_submit(poster_, [this, ws0] {
                const vc_api::WorkspaceSet e{ws0.c_str()};
                host_.emit(vc_api::workspace_set, &e);
            });
        }
        // 权限模式对齐：config 持久化值 → agent/runner（默认 auto-edit）
        {
            const std::string mode = vc_api::normalize_permission_mode(
                config_.value("permission_mode", "auto-edit"));
            nostos::kit::poster_submit(poster_, [this, mode] {
                const vc_api::PermissionMode e{mode.c_str()};
                host_.emit(vc_api::permission_mode, &e);
            });
        }

        // HTTP 服务：端口 = env > config > 8790；占用则回退系统分配并广播实际端口
        int port = 8790;
        if (const char* v = std::getenv("ITHACA_WEBUI_PORT"); v != nullptr && *v != '\0')
            port = std::atoi(v);
        else if (config_.contains("port"))
            port = config_.value("port", 8790);
        std::string err;
        server_ = std::make_shared<http::Server>(static_cast<std::uint16_t>(port), *this,
                                                 http::ServerOptions{}, &err);
        if (server_ == nullptr || !server_->ok()) {
            err = err.empty() ? "占用" : err;
            host_.log(3, "ithaca.webui: 端口 " + std::to_string(port) + " 失败（" + err +
                             "），回退系统分配");
            server_ = std::make_shared<http::Server>(0, *this, http::ServerOptions{}, &err);
        }
        if (server_ == nullptr || !server_->ok()) {
            host_.log(4, "ithaca.webui: HTTP 服务启动失败：" + err);
            return NOSTOS_ERR_FAILED;
        }
        host_.log(2, "ithaca.webui: 就绪 http://127.0.0.1:" + std::to_string(server_->port()) +
                         "  会话=" + (session_.empty() ? "（无——前端草稿态）" : session_) +
                         "  记录=" + std::to_string(records_.size()));
        // 实际端口落盘（多实例发现锚点：壳/启动器凭 <home>\port 找到本实例——
        // 回退系统分配后宿主日志不可编程读取，文件是唯一可靠出口）
        {
            std::ofstream pf(home_ / "port", std::ios::binary | std::ios::trunc);
            if (pf) pf << server_->port();
        }
        start_fs_watcher_(host_workspace_);   // 文件树真监听（空根 = hero 态，随会话切换重启）
        start_prober_();   // 多实例聚合 alive 真值（非壳模式不启动）
        return NOSTOS_OK;
    }

    void deactivate() noexcept {
        stop_prober_();
        stop_fs_watcher_();   // 先停 watcher——它还在广播，退订/停服后不得再发
        subs_.clear();  // 先退订——此后不得再 broadcast
        if (server_) server_->stop();
        server_.reset();
        if (out_.is_open()) out_.close();
        host_.log(2, "ithaca.webui: 已停用");
    }

    // ---- 文件树 watcher（VSCode 式真监听）--------------------------------------
    // ReadDirectoryChangesW 递归监听当前工作区根，变更经 SSE 广播 fs_changed——
    // 前端 fsRev++ 触发文件树自动重拉。黑名单目录不过滤（递归监听无法排除子树），
    // 靠 350ms 去抖合并事件风暴，刷新本身有界（几次 HTTP list）。
    // NOTE: broadcast_ 自本线程直发是安全的（handle 内禁的是 select 线程自锁——
    // pick-folder 线程同款先例）；server_ 未起/已停时空操作。

    void start_fs_watcher_(const std::string& root) {
        stop_fs_watcher_();
        if (root.empty()) return;   // hero 态：无工作区不监听
        fs_watch_handle_ = CreateFileW(utf8_to_wide(root).c_str(), FILE_LIST_DIRECTORY,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                       nullptr);
        if (fs_watch_handle_ == INVALID_HANDLE_VALUE || fs_watch_handle_ == nullptr) {
            fs_watch_handle_ = nullptr;
            // 网络盘/特殊路径监听不可靠——只降级不致命，树退化为手动刷新
            host_.log(3, "ithaca.webui: 文件监听打不开（" + root + "）——文件树退化为手动刷新");
            return;
        }
        fs_watch_running_.store(true);
        fs_watch_thread_ = std::thread([this] {
            std::vector<char> buf(64 * 1024);
            while (fs_watch_running_.load()) {
                DWORD n = 0;
                if (!ReadDirectoryChangesW(
                        fs_watch_handle_, buf.data(), static_cast<DWORD>(buf.size()), TRUE,
                        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                        &n, nullptr, nullptr)) {
                    // ERROR_OPERATION_ABORTED = 正常停止路径；其余记一次后退出（退化手动）
                    if (const DWORD err = GetLastError(); err != ERROR_OPERATION_ABORTED)
                        host_.log(3, "ithaca.webui: 文件监听停止（错误 " + std::to_string(err) + "）");
                    break;
                }
                if (!fs_watch_running_.load()) break;
                Sleep(350);   // 去抖：一次保存/构建是成簇通知，合并成一条
                if (!fs_watch_running_.load()) break;
                broadcast_({{"type", "fs_changed"}});
            }
        });
    }

    void stop_fs_watcher_() {
        if (fs_watch_handle_ != nullptr && fs_watch_handle_ != INVALID_HANDLE_VALUE) {
            fs_watch_running_.store(false);
            CancelIoEx(fs_watch_handle_, nullptr);   // 唤醒阻塞中的同步读
        }
        if (fs_watch_thread_.joinable()) fs_watch_thread_.join();
        if (fs_watch_handle_ != nullptr && fs_watch_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(fs_watch_handle_);
            fs_watch_handle_ = nullptr;
        }
    }

    // ---- 事件订阅（宿主线程序列化；各 handler 只做：拼 JSON → 落盘 → 广播）-----

    void subscribe_() {
        // 事件面与 tui 同构；raw 订阅形态（agent/tui 同款）：on(event_id, fn(const void*))
        // workspace 工具表后到接入的兜底：activate 时解析不到（装载顺序在 webui
        // 之后）就等它 tool_register 时再解析——回调在宿主线程上执行，TLS 桥在。
        subs_.push_back(host_.on(vc_api::tool_register, [this](const void* p) {
            const auto* e = static_cast<const vc_api::ToolRegister*>(p);
            if (e->svc_name == nullptr ||
                std::string_view(e->svc_name) != vcws::kToolSvcName || tools_ != nullptr)
                return;
            tools_ = host_.service<vc_api::ToolTable>(vcws::kToolSvc);
            if (tools_ != nullptr) host_.log(2, "ithaca.webui: workspace 工具表已接入（后到）");
        }));
        subs_.push_back(host_.on(vc_api::llm_delta, [this](const void* p) {
            const auto* e = static_cast<const vc_api::LlmDelta*>(p);
            if (e->text == nullptr) return;
            broadcast_({{"type", "assistant_delta"}, {"text", std::string(e->text)}});
        }));
        subs_.push_back(host_.on(vc_api::llm_reasoning, [this](const void* p) {
            const auto* e = static_cast<const vc_api::LlmReasoning*>(p);
            if (e->text == nullptr) return;
            broadcast_({{"type", "assistant_reasoning"}, {"text", std::string(e->text)}});
        }));
        subs_.push_back(host_.on(vc_api::llm_done, [this](const void* p) {
            const auto* e = static_cast<const vc_api::LlmDone*>(p);
            std::string content;
            json usage = json::object();
            bool turn_done = true;  // 无 tool_calls ⇒ 本 turn 收口（前端「本轮修改」卡时机）
            if (e->assistant_json != nullptr) {
                json a = json::parse(e->assistant_json, nullptr, false);
                if (a.is_object() && a.contains("content") && a["content"].is_string())
                    content = a["content"].get<std::string>();
                if (a.is_object() && a.contains("tool_calls") && a["tool_calls"].is_array() &&
                    !a["tool_calls"].empty())
                    turn_done = false;
            }
            if (e->usage != nullptr) {
                json u = json::parse(e->usage, nullptr, false);
                if (u.is_object()) {
                    usage = u;
                    token_total_ += u.value("total_tokens", 0LL);
                }
            }
            in_flight_.store(false);
            persist_status_();
            record_({{"type", "assistant"},
                     {"text", content},
                     {"usage", usage},
                     {"turn_done", turn_done}});
        }));
        subs_.push_back(host_.on(vc_api::llm_error, [this](const void* p) {
            const auto* e = static_cast<const vc_api::LlmError*>(p);
            in_flight_.store(false);
            persist_status_();
            record_({{"type", "error"},
                     {"code", e->code},
                     {"msg", e->message != nullptr ? std::string(e->message) : ""}});
        }));
        subs_.push_back(host_.on(vc_api::tool_call, [this](const void* p) {
            const auto* e = static_cast<const vc_api::ToolCall*>(p);
            record_({{"type", "tool_call"},
                     {"tool", e->tool != nullptr ? std::string(e->tool) : ""},
                     {"brief", e->args_brief != nullptr ? std::string(e->args_brief) : ""}});
        }));
        subs_.push_back(host_.on(vc_api::tool_result, [this](const void* p) {
            const auto* e = static_cast<const vc_api::ToolResult*>(p);
            record_({{"type", "tool"},
                     {"tool", e->tool != nullptr ? std::string(e->tool) : ""},
                     {"ok", e->ok},
                     {"brief", e->result_brief != nullptr ? std::string(e->result_brief) : ""}});
        }));
        subs_.push_back(host_.on(vc_api::edit_proposed, [this](const void* p) {
            const auto* e = static_cast<const vc_api::EditProposed*>(p);
            pending_turn_ = e->turn_id;
            persist_status_();   // 提案待裁决（聚合徽标同步）
            record_({{"type", "proposal"},
                     {"turn_id", e->turn_id},
                     {"diff", e->diff_text != nullptr ? std::string(e->diff_text) : ""}});
        }));
        subs_.push_back(host_.on(vc_api::edit_applied, [this](const void* p) {
            const auto* e = static_cast<const vc_api::EditApplied*>(p);
            json paths = json::array();
            if (e->paths_json != nullptr) {
                json pa = json::parse(e->paths_json, nullptr, false);
                if (pa.is_array()) paths = pa;
            }
            // edit-diff 方案 v1.1：applied 携带 hunk diff——落盘完成后前端立即渲染；
            // diff_text 为 null = 旧生产者组合（前端退化为路径列表）
            record_({{"type", "applied"},
                     {"turn_id", e->turn_id},
                     {"paths", paths},
                     {"diff", e->diff_text != nullptr ? std::string(e->diff_text) : ""}});
        }));
        subs_.push_back(host_.on(vc_api::edit_undone, [this](const void* p) {
            const auto* e = static_cast<const vc_api::EditUndone*>(p);
            record_({{"type", "undone"}, {"turn_id", e->turn_id}, {"depth", e->snapshot_depth}});
        }));
        subs_.push_back(host_.on(vc_api::run_line, [this](const void* p) {
            const auto* e = static_cast<const vc_api::RunLine*>(p);
            record_({{"type", "run_line"},
                     {"run_id", e->run_id},
                     {"stream", e->stream},
                     {"text", e->text != nullptr ? std::string(e->text) : ""}});
        }));
        subs_.push_back(host_.on(vc_api::run_exit, [this](const void* p) {
            const auto* e = static_cast<const vc_api::RunExit*>(p);
            record_({{"type", "run_exit"}, {"run_id", e->run_id}, {"exit_code", e->exit_code}});
        }));
        subs_.push_back(host_.on(vc_api::notice, [this](const void* p) {
            const auto* e = static_cast<const vc_api::Notice*>(p);
            record_({{"type", "notice"},
                     {"level", e->level},
                     {"text", e->text != nullptr ? std::string(e->text) : ""},
                     {"sys", e->sys}});
        }));
    }

    // ---- HTTP 路由（select 线程：必须快）----------------------------------------

    http::Response handle(const http::Request& req) override {
        const std::string& m = req.method;
        // 剥掉 query（"?v=..." 形态的缓存穿透参数）——否则带参请求全部 404
        const std::string t = req.target.substr(0, req.target.find('?'));

        if (m == "GET" && (t == "/" || t == "/index.html")) return file_("index.html");
        if (m == "GET" && t.rfind("/web/", 0) == 0) return file_(t.substr(5));
        if (m == "GET" && t == "/events") {
            http::Response r;
            r.sse_stream = true;
            r.content_type = "text/event-stream";
            return r;
        }
        if (m == "GET" && t == "/api/health")
            return json_resp({{"ok", true}, {"session", session_}});
        if (m == "GET" && t == "/api/config") return json_resp(config_public_());
        if (m == "POST" && t == "/api/config") return api_config_save_(req.body);
        if (m == "GET" && t.rfind("/api/transcript", 0) == 0) {
            long after = 0;
            const auto q = req.target.find("after=");   // 从原始 target 解析（t 已剥 query）
            if (q != std::string::npos) after = std::atol(req.target.c_str() + q + 6);
            json arr = json::array();
            for (std::size_t i = static_cast<std::size_t>(after); i < records_.size(); ++i)
                arr.push_back(records_[i]);
            return json_resp({{"records", arr}, {"next", records_.size()}});
        }
        if (m == "GET" && t == "/api/session") return json_resp(session_meta_());
        if (m == "GET" && t == "/api/sessions") return json_resp(index_["sessions"]);
        if (m == "POST" && t == "/api/session/new") return api_session_new_(req.body);
        if (m == "POST" && t == "/api/session/open") return api_session_open_(req.body);
        if (m == "POST" && t == "/api/session/delete") return api_session_delete_(req.body);
        if (m == "POST" && t == "/api/session/rename") return api_session_rename_(req.body);
        if (m == "POST" && t == "/api/input") return api_input_(req.body);
        if (m == "POST" && t == "/api/decision") return api_decision_(req.body);
        if (m == "POST" && t == "/api/model") return api_model_(req.body);
        if (m == "GET" && t == "/api/permission-mode")
            return json_resp({{"mode", vc_api::normalize_permission_mode(
                                             config_.value("permission_mode", "auto-edit"))}});
        if (m == "POST" && t == "/api/permission-mode") return api_permission_mode_(req.body);
        // NOTE: 传 req.target（含 query）——上面的 t 已剥 query（静态资源缓存穿透
        // 用），带参 API 从原始 target 自行解析。踩过: 传剥完的 t ⇒ 参数永远解析
        // 不到（transcript 的 after= 曾因此静默失效，靠前端按 id 去重掩住）
        if (m == "GET" && t.rfind("/api/fs/list", 0) == 0) return api_fs_list_(req.target);
        if (m == "POST" && t == "/api/pick-folder") return api_pick_folder_();
        if (m == "GET" && t == "/api/mcp/status") return api_mcp_status_();
        if (m == "GET" && t.rfind("/api/shell/sessions", 0) == 0)
            return api_shell_sessions_();
        if (m == "POST" && t == "/api/shutdown") {
            // 不落「宿主停止」通告：每次关窗写一条，重启重放必现纯噪音
            // （踩过：开关 10 次攒 10 条）
            std::exit(0);  // 每条记录/每 turn 已落盘，立即退出是安全的（方案 §10）
        }
        host_.log(2, "ithaca.webui: 404 " + m + " " + t);   // 临时诊断
        return http::Response::status_page(404, "not found");
    }

private:
    // ---- 工具 -------------------------------------------------------------------

    // ---- 兄弟实例健康探测（多实例聚合 alive 真值）------------------------------
    // 背景：聚合端点原以 port 文件存在判 alive——宿主死亡后文件残留 ⇒ 死会话被标
    // 存活，前端整页导航到死端口（踩过：切换会话 nav FAIL 循环）。本线程每 2s 对
    // manifest 各 key 的实际端口做 /api/health 探测并缓存真值；聚合端点读缓存。
    // 仅壳模式（ITHACA_SHELL_MANIFEST 已设）运行。
    void start_prober_() {
        const char* mf = std::getenv("ITHACA_SHELL_MANIFEST");
        if (mf == nullptr || *mf == '\0') return;
        manifest_path_ = mf;
        prober_run_.store(true);
        prober_ = std::thread([this] {
            while (prober_run_.load()) {
                std::ifstream in(manifest_path_, std::ios::binary);
                std::string line;
                std::map<std::string, std::pair<bool, int>> fresh;
                while (std::getline(in, line)) {
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                    if (line.empty()) continue;
                    const size_t t1 = line.find('\t');
                    if (t1 == std::string::npos) continue;
                    const std::string key = line.substr(0, t1);
                    if (key == "ACTIVE") continue;
                    std::ifstream pin(manifest_path_.parent_path() / "hosts" / key / "port",
                                      std::ios::binary);
                    int port = 0;
                    if (pin >> port) {
                        HINTERNET ses = WinHttpOpen(L"ithaca-probe/1.0",
                                                    WINHTTP_ACCESS_TYPE_NO_PROXY,
                                                    WINHTTP_NO_PROXY_NAME,
                                                    WINHTTP_NO_PROXY_BYPASS, 0);
                        bool alive = false;
                        if (ses != nullptr) {
                            HINTERNET con = WinHttpConnect(ses, L"127.0.0.1",
                                                           static_cast<INTERNET_PORT>(port), 0);
                            if (con != nullptr) {
                                HINTERNET req = WinHttpOpenRequest(
                                    con, L"GET", L"/api/health", nullptr, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
                                WinHttpSetTimeouts(req, 800, 800, 800, 800);
                                if (req != nullptr) {
                                    if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                                        WinHttpReceiveResponse(req, nullptr)) {
                                        DWORD status = 0, sz = sizeof(status);
                                        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE |
                                                                 WINHTTP_QUERY_FLAG_NUMBER,
                                                            WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                                            &sz, WINHTTP_NO_HEADER_INDEX);
                                        alive = status == 200;
                                    }
                                    WinHttpCloseHandle(req);
                                }
                                WinHttpCloseHandle(con);
                            }
                            WinHttpCloseHandle(ses);
                        }
                        fresh[key] = {alive, port};
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(probe_mu_);
                    probe_cache_ = std::move(fresh);
                }
                for (int i = 0; i < 20 && prober_run_.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

    void stop_prober_() {
        prober_run_.store(false);
        if (prober_.joinable()) prober_.join();
    }

    // 实例瞬态状态落盘（聚合侧栏的徽标数据源：壳/兄弟实例只读磁盘，不互发 HTTP）
    void persist_status_() {
        std::ofstream f(home_ / "status.json", std::ios::binary | std::ios::trunc);
        if (f) f << json{{"working", in_flight_.load()}, {"pending", pending_turn_ >= 0}}.dump();
    }

    static json parse_body_(const std::string& body) {
        json d = json::parse(body, nullptr, false);
        return (!d.is_discarded() && d.is_object()) ? d : json::object();
    }

    http::Response json_resp(json obj, int status = 200) {
        std::string s = obj.dump();
        http::Response r = http::Response::text(status, "application/json; charset=utf-8",
                                                std::move(s));
        return r;
    }

    static std::string mime_of(const fs::path& p) {
        auto ext = p.extension().string();
        if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
        if (ext == ".js") return "text/javascript; charset=utf-8";
        if (ext == ".css") return "text/css; charset=utf-8";
        if (ext == ".json") return "application/json; charset=utf-8";
        if (ext == ".svg") return "image/svg+xml";
        if (ext == ".png") return "image/png";
        return "application/octet-stream";
    }

    // 静态文件：归界检查必须在 web_root_ 内（路径穿越 = CwdJail 同款教训）
    http::Response file_(const std::string& rel) {
        if (rel.find("..") != std::string::npos)
            return http::Response::status_page(403, "forbidden");
        std::error_code ec;
        fs::path target = fs::weakly_canonical(web_root_ / rel, ec);
        if (ec || target.string().rfind(web_root_.string(), 0) != 0)
            return http::Response::status_page(403, "forbidden");
        if (!fs::exists(target, ec)) return http::Response::status_page(404, "not found");
        http::Response r;
        r.file_path = target.string();
        r.file_length = fs::file_size(target, ec);
        r.content_type = mime_of(target);
        // NOTE: html/js/css 加 no-cache——webview 会启发式缓存旧页面，UI 改版后
        // 一直跑旧脚本（踩过）。图片等二进制仍可缓存。
        const std::string ct = r.content_type;
        if (ct.rfind("text/html", 0) == 0 || ct.rfind("text/javascript", 0) == 0 ||
            ct.rfind("text/css", 0) == 0)
            r.headers.push_back({"cache-control", "no-cache"});
        return r;
    }

    // 只广播不落盘（流式增量；完整内容在 LlmDone 时以 assistant 记录收口）
    void broadcast_(json rec) { if (server_) server_->broadcast(rec.dump()); }

    // ---- 会话记录（落盘 + 广播；id = transcript 行号，页面据此续传）--------------

    // 只落盘（select 线程用）。NOTE: handle() 内严禁 broadcast——select 循环持
    // kit 内部锁调 handle，再抢同一把锁 = EDEADLOCK（踩过：POST /api/input 即 500）。
    // persist_ 是 select/宿主两线程共用的唯一持久化路径（计数/标题更新在内）。
    // NOTE: 按**引用**收参——id 写回调用方的 rec，广播/延迟广播因此都带 id，前端
    // 按 id 去重；按值收参时广播无 id，SSE 与重放会重复渲染/重复计数。
    void persist_(json& rec) {
        rec["id"] = records_.size();
        records_.push_back(rec);
        if (out_.is_open()) {
            out_ << rec.dump() << "\n";
            out_.flush();
        }
        // 更新活跃会话的计数/标题（工作区分组侧栏的数据源）
        if (json* e = session_entry_(session_); e != nullptr) {
            const std::string type = rec.value("type", "");
            if (type == "user") {
                (*e)["turns"] = (*e).value("turns", 0) + 1;
                if ((*e).value("title", "") == "(新会话)" ||
                    (*e).value("title", "").empty()) {
                    // NOTE: 按字节切 48 会劈开多字节 UTF-8，index_.dump() 必抛 ⇒
                    // index.json 永久保存失败——必须走码点边界回退
                    std::string t = rec.value("text", "");
                    if (t.size() > 48) t = vcws::trim_utf8(t, 48) + "…";
                    if (!t.empty()) (*e)["title"] = t;
                }
            }
            if (type == "assistant" && rec.contains("usage") && rec["usage"].is_object())
                (*e)["tokens"] = (*e).value("tokens", 0LL) +
                                 rec["usage"].value("total_tokens", 0LL);
            (*e)["updated"] = now_epoch_();
            // NOTE: index.json 只在轮次边界落盘（user=开头 / assistant=收口）——
            // 每条记录都全量重写注册表是纯浪费（几百条转录 = 几十次写盘），元数据
            // 最多滞后半个 turn；改名/删除/新建/切换等路径直接调 save_index_ 不受此限
            if (type == "user" || type == "assistant") save_index_();
        }
    }

    // 落盘 + 广播（宿主线程用：事件回调里调用，不在 handle 栈内）
    void record_(json rec) {
        persist_(rec);
        if (server_) server_->broadcast(rec.dump());
    }

    void load_transcript_() {
        std::error_code ec;
        const fs::path f = home_ / "sessions" / (session_ + ".transcript.jsonl");
        if (!fs::exists(f, ec)) return;
        std::ifstream in(f, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            json rec = json::parse(line, nullptr, false);
            if (rec.is_object() && rec.contains("id") && rec["id"].is_number_unsigned() &&
                rec["id"] == records_.size())  // 尾行损坏/截断 ⇒ 从断点起忽略
                records_.push_back(rec);
        }
    }

    void open_transcript_() {
        if (session_.empty()) return;   // 无会话态：不建任何 transcript 文件
        std::error_code ec;
        fs::create_directories(home_ / "sessions", ec);
        out_.open(home_ / "sessions" / (session_ + ".transcript.jsonl"),
                  std::ios::app | std::ios::binary);
    }

    // ---- 配置 -------------------------------------------------------------------

    void load_config_() {
        std::error_code ec;
        const fs::path f = config_path_();
        if (!fs::exists(f, ec)) return;
        std::ifstream in(f, std::ios::binary);
        json d = json::parse(in, nullptr, false);
        if (!d.is_object()) return;
        config_ = d;
        merge_local_overlay_();
    }

    // ---- 配置两层化（多实例：凭据共享 + 实例覆盖层）---------------------------
    // 共享文件（ITHACA_WEBUI_CONFIG，未设 = home/config.json 即单实例原行为）存
    // providers/Key 等凭据；实例差异项（当前供应商/模型/权限模式）落 home 的
    // config-local.json 覆盖层。唯一写者是设置面板 UI（用户驱动、低频），运行中
    // 的实例只读共享文件——结构上无写竞争（多宿主方案 §3.1）。
    fs::path config_path_() const {
        if (const char* v = std::getenv("ITHACA_WEBUI_CONFIG"); v != nullptr && *v != '\0')
            return fs::path(v);
        return home_ / "config.json";
    }
    bool config_shared() const noexcept {
        const char* v = std::getenv("ITHACA_WEBUI_CONFIG");
        return v != nullptr && *v != '\0';
    }
    void merge_local_overlay_() {
        if (!config_shared()) return;
        std::error_code ec;
        const fs::path f = home_ / "config-local.json";
        if (!fs::exists(f, ec)) return;
        std::ifstream in(f, std::ios::binary);
        json d = json::parse(in, nullptr, false);
        if (!d.is_object()) return;
        for (const char* k : {"current_provider", "model", "provider", "permission_mode"})
            if (d.contains(k)) config_[k] = d[k];
    }
    bool persist_config_() {
        // 单实例模式 = 原 home/config.json 全量行为不变。共享模式下：providers/
        // Key 等凭据写共享文件，但**实例差异项（当前供应商/模型/权限模式）不进
        // 共享文件**——方案 §3.1「实例对差异项只读」，否则 A 实例切模型会把 B
        // 实例的重启默认值污染掉；差异项只落本实例覆盖层
        const fs::path f = config_path_();
        std::ofstream out(f, std::ios::binary | std::ios::trunc);
        if (!out) {
            // 写失败绝不能静默：否则 key 每次重启都丢，用户只看到"key=未设置"
            host_.log(4, "ithaca.webui: 配置写入失败（" + f.string() +
                             "）——数据目录不可写，用启动器 -DataHome 换位置");
            return false;
        }
        if (config_shared()) {
            json shared = config_;
            for (const char* k : {"current_provider", "model", "provider", "permission_mode"})
                shared.erase(k);
            out << shared.dump();
            json local = json::object();
            for (const char* k : {"current_provider", "model", "provider", "permission_mode"})
                if (config_.contains(k)) local[k] = config_[k];
            std::ofstream lo(home_ / "config-local.json", std::ios::binary | std::ios::trunc);
            if (lo) lo << local.dump();
        } else {
            out << config_.dump();
        }
        return true;
    }

    json config_public_() {
        // 供应商两层配置：providers 按模式单侧下发（mock=测试内置合成项；cloud=
        // 用户配置项）——面板/模型下拉只显示当前模式一项（产品拍板）；api_key
        // 掩码（不出前端，只给 has_key 供"已设置/未设置"显示）。
        const bool mock = llm_mode_ == "mock";
        json providers = json::array();
        std::string current;
        for (const auto& p : config_.value("providers", json::array())) {
            if (!p.is_object()) continue;
            const std::string type = p.value("type", "cloud");
            if (mock ? type != "mock" : type != "cloud") continue;
            json q = p;
            q.erase("api_key");
            q["has_key"] = !p.value("api_key", "").empty();
            providers.push_back(q);
        }
        if (mock) {   // mock 内置合成：不依赖 config（测试模式不持久化供应商数据）
            if (providers.empty())
                providers.push_back(json{{"name", "mock"},
                                         {"type", "mock"},
                                         {"base_url", ""},
                                         {"enabled", true},
                                         {"has_key", false},
                                         {"models", json::array({json{{"id", "mock-model"},
                                                                      {"context", 128000},
                                                                      {"enabled", true}}})}});
            current = "mock";
        } else if (!providers.empty()) {
            current = config_.value("current_provider", "");
            bool known = false;
            for (const auto& p : providers)
                if (p.value("name", "") == current) { known = true; break; }
            if (!known) current = providers[0].value("name", "");   // 兜底：钳在模式内
        }
        return {{"provider", mock ? std::string("mock")
                                  : config_.value("provider", "cloud")},
                {"model", config_.value("model", "deepseek-flash")},
                {"workspace", config_.value("workspace", "")},
                {"port", config_.value("port", 8790)},
                {"permission_mode",
                 vc_api::normalize_permission_mode(config_.value("permission_mode", "auto-edit"))},
                {"providers", providers},
                {"current_provider", current},
                {"mcp", config_.value("mcp", json::object())}};
    }

    /// POST /api/config → 供应商两层配置校验 + 落盘 + 当前供应商热切。key 只写
    /// 不读（响应经 has_key 掩码；请求留空 = 按名保留旧值）。
    http::Response api_config_save_(const std::string& body) {
        json in = parse_body_(body);
        // 供应商两层配置：{"providers":[{name,type,base_url,api_key,enabled,
        // models:[{id,context,enabled}...]}], "current_provider":"名称"}——
        // 校验：供应商名非空/≤64/去重（大小写不敏感）、type 只许 mock|cloud、
        // 模型 id 同规 + context ≥1024、至多 32 供应商 × 32 模型。
        // api_key 留空/缺失 = 按名保留旧值（key 不出前端，前端回传必为空）。
        // mock = 测试模式：供应商内置合成、不持久化——面板的供应商/模型修改忽略
        // （mcp/权限模式等其余字段照常保存）；cloud 模式替换式合并（只动 cloud 项，
        // 保留其他类型项——磁盘数据跨模式保全）
        if (in.contains("providers") && in["providers"].is_array() && llm_mode_ != "mock") {
            const json existing = config_.value("providers", json::array());
            const auto old_key_of = [&existing](const std::string& name) -> std::string {
                for (const auto& p : existing)
                    if (p.is_object() && p.value("name", "") == name)
                        return p.value("api_key", "");
                return "";
            };
            json cleaned = json::array();
            std::vector<std::string> seen;
            bool bad = false;
            for (const auto& p : in["providers"]) {
                if (!p.is_object() || cleaned.size() >= 32) { bad = true; break; }
                const std::string name = p.contains("name") && p["name"].is_string()
                                             ? p["name"].get<std::string>() : "";
                const std::string type = p.contains("type") && p["type"].is_string()
                                             ? p["type"].get<std::string>() : "cloud";
                if (name.empty() || name.size() > 64 || type != "cloud") {
                    bad = true; break;   // 面板单侧显示（产品拍板）：mock 内置不接受外部写入
                }
                std::string low = name;
                for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                if (std::find(seen.begin(), seen.end(), low) != seen.end()) { bad = true; break; }
                seen.push_back(low);
                json e = {{"name", name}, {"type", type}};
                if (p.contains("builtin") && p["builtin"].is_boolean())
                    e["builtin"] = p["builtin"];
                if (type == "cloud") {
                    e["base_url"] = p.contains("base_url") && p["base_url"].is_string()
                                        ? p["base_url"].get<std::string>() : "";
                    std::string key = p.contains("api_key") && p["api_key"].is_string()
                                          ? p["api_key"].get<std::string>() : "";
                    if (key.empty()) key = old_key_of(name);   // 留空 = 按名保留旧 key
                    e["api_key"] = key;
                }
                e["enabled"] = p.value("enabled", false);
                json models = json::array();
                std::vector<std::string> mseen;
                if (p.contains("models") && p["models"].is_array())
                    for (const auto& m : p["models"]) {
                        if (!m.is_object() || models.size() >= 32) { bad = true; break; }
                        const std::string mid = m.contains("id") && m["id"].is_string()
                                                    ? m["id"].get<std::string>() : "";
                        const int ctx = m.contains("context") && m["context"].is_number_integer()
                                            ? m["context"].get<int>() : 0;
                        std::string mlow = mid;
                        for (char& c : mlow)
                            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                        if (mid.empty() || mid.size() > 64 || ctx < 1024 ||
                            std::find(mseen.begin(), mseen.end(), mlow) != mseen.end()) {
                            bad = true; break;
                        }
                        mseen.push_back(mlow);
                        models.push_back(json{{"id", mid}, {"context", ctx},
                                              {"enabled", m.value("enabled", true)}});
                    }
                if (bad) break;
                e["models"] = models;
                cleaned.push_back(e);
            }
            if (bad)
                return json_resp({{"ok", false},
                                  {"error", "供应商配置非法（名称/模型 ID 重复或为空、"
                                            "上下文 ≥1024、超 32 个等）"}});
            // current_provider：前端指定或保留；必须命中列表，否则回落第一个
            std::string cur = config_.value("current_provider", "");
            if (in.contains("current_provider") && in["current_provider"].is_string())
                cur = in["current_provider"].get<std::string>();
            bool cur_ok = false;
            for (const auto& p : cleaned)
                if (p.value("name", "") == cur) { cur_ok = true; break; }
            if (!cur_ok && !cleaned.empty()) cur = cleaned.front().value("name", "");
            {
                json merged = json::array();
                for (const auto& p : config_.value("providers", json::array()))
                    if (p.is_object() && p.value("type", "cloud") != "cloud")
                        merged.push_back(p);   // 非 cloud 类型项保全（磁盘跨模式数据不丢）
                // NOTE: 不可 move——合并块之后的 provider/model 同步循环还要遍历
                // cleaned，move 会把元素搬成 null 令该循环静默失效 + 响应路径抛
                // type_error（500，实锤）
                for (const auto& c : cleaned) merged.push_back(c);
                config_["providers"] = std::move(merged);
            }
            config_["current_provider"] = cur;
            // config.provider/model 同步为当前供应商（重启 env 兼容 + 旧面板读法）
            std::string cur_key, cur_burl;
            for (const auto& p : cleaned) {
                if (p.value("name", "") != cur) continue;
                config_["provider"] = p.value("type", "cloud");
                cur_key = p.value("api_key", "");
                cur_burl = p.value("base_url", "");
                // 当前模型不在该供应商启用列表 ⇒ 切到第一个启用模型
                bool m_ok = false;
                for (const auto& m : p.value("models", json::array()))
                    if (m.value("enabled", false) &&
                        m.value("id", "") == config_.value("model", "")) { m_ok = true; break; }
                if (!m_ok)
                    for (const auto& m : p.value("models", json::array()))
                        if (m.value("enabled", false)) { config_["model"] = m["id"]; break; }
                break;
            }
            // cloud 无 key ⇒ 必然 FAILED(5)，提前拦截并点名
            if (config_.value("provider", "mock") == "cloud" && cur_key.empty())
                return json_resp({{"ok", false},
                                  {"error", "当前供应商（" + cur + "）未填写 API Key——"
                                            "请在供应商详情中输入后重试"}});
        }
        if (in.contains("key") && in["key"].is_string() && !in["key"].get<std::string>().empty())
            config_["api_key"] = in["key"];
        for (const char* k : {"provider", "model", "base_url", "workspace"})
            if (in.contains(k) && in[k].is_string()) config_[k] = in[k];
        // MCP server 清单（设置面板 MCP 工具页）：整对象透传，保存后通知 mcp 插件热重载
        bool mcp_changed = false;
        if (in.contains("mcp") && in["mcp"].is_object()) {
            config_["mcp"] = in["mcp"];
            mcp_changed = true;
        }
        if (!persist_config_()) {
            const std::string cfg_path = config_path_().string();
            return json_resp({{"ok", false},
                              {"error", "数据目录不可写：" + cfg_path +
                                            " —— 重启宿主时加 -DataHome 参数换个位置"}});
        }
        // 当前使用供应商热切：广播 vc.llm.set_active（provider 名=type、key、
        // base_url——尾部追加）——即时生效，无需重启。
        // NOTE: mock 模式恒发 mock（同 activate 对齐——config 的 provider 可能是
        // 另一模式遗留值，跨模式热切会把本模式插件休眠掉）
        const std::string provider = llm_mode_ == "mock"
                                         ? std::string("mock")
                                         : config_.value("provider", "mock");
        const std::string key = config_.value("api_key", "");
        const std::string cur = config_.value("current_provider", "");
        std::string cur_key = key, cur_burl;
        for (const auto& p : config_.value("providers", json::array()))
            if (p.is_object() && p.value("name", "") == cur) {
                cur_key = p.value("api_key", key);
                cur_burl = p.value("base_url", "");
                break;
            }
        nostos::kit::poster_submit(poster_, [this, provider, cur_key, cur_burl] {
            const vc_api::LlmSetActive la{provider.c_str(), cur_key.c_str(),
                                          cur_burl.empty() ? nullptr : cur_burl.c_str()};
            host_.emit(vc_api::llm_set_active, &la);
            // 工作区归会话管：配置保存不热切工作区
        });
        host_.log(2, "ithaca.webui: 配置已保存（供应商「" + config_.value("current_provider", "") +
                         "」已热切）");
        return json_resp({{"ok", true}, {"note", "已保存；当前供应商即时生效"}});
    }

    // ---- 会话注册表（index.json：工作区分组的元数据源；--------------------

    void load_index_() {
        std::error_code ec;
        const fs::path f = home_ / "index.json";
        if (!fs::exists(f, ec)) {
            index_ = json{{"active", session_}, {"sessions", json::array()}};
            return;
        }
        std::ifstream in(f, std::ios::binary);
        json d = json::parse(in, nullptr, false);
        index_ = (d.is_object() && d.contains("sessions") && d["sessions"].is_array())
                     ? d
                     : json{{"active", session_}, {"sessions", json::array()}};
    }

    void save_index_() {
        std::ofstream out(home_ / "index.json", std::ios::binary | std::ios::trunc);
        if (out) out << index_.dump();
    }

    json* session_entry_(const std::string& id) {
        for (auto& s : index_["sessions"])
            if (s.is_object() && s.value("id", "") == id) return &s;
        return nullptr;
    }

    static long long now_epoch_() {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    void ensure_session_(const std::string& id, const std::string& workspace) {
        if (session_entry_(id) != nullptr) return;
        json s = {{"id", id},
                  {"title", "(新会话)"},
                  {"workspace", workspace},
                  {"created", now_epoch_()},
                  {"updated", now_epoch_()},
                  {"turns", 0},
                  {"tokens", 0}};
        index_["sessions"].push_back(s);
        save_index_();
    }

    // 启动兜底清扫：注册表只允许已初始化会话（turns ≥ 1 = transcript 至少 1 条
    // 用户记录）。turns==0 的条目（历史首装种子 / 组合端点中途崩溃的残留）移除，
    // 其空 transcript 一并清掉——注册表结构上不可能留未初始化条目。
    void sweep_uninitialized_() {
        bool changed = false;
        json arr = json::array();
        for (auto& s : index_["sessions"]) {
            if (s.is_object() && s.value("turns", 0) > 0) { arr.push_back(s); continue; }
            std::error_code ec;
            fs::remove(home_ / "sessions" / (s.value("id", "") + ".transcript.jsonl"), ec);
            changed = true;
        }
        if (changed) index_["sessions"] = arr;
    }

    // 会话切换：换 transcript + 通知 agent 加载上下文（vc.session.load）
    void switch_to_(const std::string& id) {
        session_ = id;
        index_["active"] = id;
        if (json* e = session_entry_(id); e != nullptr) (*e)["updated"] = now_epoch_();
        save_index_();
        records_.clear();
        load_transcript_();
        if (out_.is_open()) out_.close();
        open_transcript_();
        const fs::path sp = home_ / "sessions" / (id + ".state.json");
        // 工作区随会话：会话记录的 workspace ≠ 当前 ⇒ 热切 jail/runner
        std::string ws = host_workspace_;
        if (json* e = session_entry_(id); e != nullptr)
            ws = e->value("workspace", host_workspace_);
        const bool ws_changed = !ws.empty() && ws != host_workspace_;
        if (ws_changed) host_workspace_ = ws;
        // 广播与 emit 都 defer 到宿主线程（handle 内禁 broadcast——EDEADLOCK）
        nostos::kit::poster_submit(poster_, [this, path = sp.string(), id, ws, ws_changed] {
            broadcast_({{"type", "session"}, {"id", id}, {"workspace", ws}});
            const vc_api::SessionLoad e{path.c_str()};
            host_.emit(vc_api::session_load, &e);
            if (ws_changed) {
                const vc_api::WorkspaceSet w{ws.c_str()};
                host_.emit(vc_api::workspace_set, &w);
                start_fs_watcher_(ws);   // 监听随工作区迁移（同工作区切会话无需重启）
            }
        });
    }

    /// GET /api/sessions → 注册表 sessions 数组（侧栏/工作区分组的唯一数据源）。
    http::Response api_sessions_() { return json_resp(index_["sessions"]); }

    /// POST /api/session/new {message 必须, workspace?} → 创建即初始化：注册 + 切换
    /// + 首条落盘 + 开 turn 一次完成，返回 {ok, id, workspace}。裸调用 400。
    /// 副作用：写注册表/transcript、SSE session 广播、vc.session.load。
    http::Response api_session_new_(const std::string& body) {
        if (in_flight_.load())
            return json_resp({{"ok", false}, {"error", "turn 进行中，稍后再试"}});
        json in = parse_body_(body);
        // 创建即初始化（方案 B：前端草稿态的唯一出口）——必须携带首条消息，裸调用
        // 400 ⇒ 服务端结构上不可能产生未初始化会话（无撤回/清扫逻辑可写）。
        const std::string msg = in.contains("message") && in["message"].is_string()
                                    ? in["message"].get<std::string>() : "";
        if (msg.empty())
            return json_resp({{"ok", false},
                              {"error", "缺少 message——会话仅在首条消息发出时创建"}}, 400);
        // 毫秒级 id——同秒内连点不撞车
        const std::string id = "s" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        // NOTE: 新会话的工作区由请求显式指定，不继承宿主当前工作区——继承会让切过
        // 带工作区会话后新建的会话全被带偏（踩过）。语义：主「＋新会话」不带
        // workspace（hero 引导选择）；侧栏组「＋」带 {workspace}（该工作区下新建）。
        std::string ws;
        if (in.contains("workspace") && in["workspace"].is_string() &&
            !in["workspace"].get<std::string>().empty()) {
            ws = in["workspace"].get<std::string>();
            std::error_code ec;
            fs::create_directories(fs::path(ws), ec);
            if (ec) return json_resp({{"ok", false},
                                      {"error", "无法创建工作目录：" + ws + "（" + ec.message() + "）"}});
        }
        ensure_session_(id, ws);   // 此刻注册（turns=0）；首条消息落盘即 turns=1，
                                   // 中途崩溃由启动清扫兜底
        switch_to_(id);
        start_turn_(msg);
        return json_resp({{"ok", true}, {"id", id}, {"workspace", ws}});
    }

    /// POST /api/session/open {id} → 切换活跃会话，返回 {ok, id}；turn 进行中或
    /// id 不存在被拒。副作用：SSE session 广播、vc.session.load、工作区随会话热切。
    http::Response api_session_open_(const std::string& body) {
        if (in_flight_.load())
            return json_resp({{"ok", false}, {"error", "turn 进行中，稍后再试"}});
        json in = parse_body_(body);
        const std::string id = in.value("id", "");
        if (session_entry_(id) == nullptr)
            return json_resp({{"ok", false}, {"error", "会话不存在"}});
        if (id == session_) return json_resp({{"ok", true}, {"id", id}});
        switch_to_(id);
        return json_resp({{"ok", true}, {"id", id}});
    }

    /// POST /api/session/rename {id, title} → 改注册表 title 并落盘；活跃/非活跃均可。
    http::Response api_session_rename_(const std::string& body) {
        json in = parse_body_(body);
        const std::string id = in.value("id", "");
        const std::string title = in.value("title", "");
        if (id.empty() || title.empty())
            return json_resp({{"ok", false}, {"error", "id 与 title 不能为空"}});
        json* e = session_entry_(id);
        if (e == nullptr) return json_resp({{"ok", false}, {"error", "会话不存在"}});
        (*e)["title"] = title;
        (*e)["updated"] = now_epoch_();
        save_index_();
        return json_resp({{"ok", true}, {"title", title}});
    }

    /// POST /api/session/delete {id} → 删注册表条目 + transcript + state 并落盘；
    /// 删活跃会话则自动切最近更新者，全空进入无会话态。
    http::Response api_session_delete_(const std::string& body) {
        json in = parse_body_(body);
        const std::string id = in.value("id", "");
        if (session_entry_(id) == nullptr)
            return json_resp({{"ok", false}, {"error", "会话不存在"}});
        // 活跃会话也可删（用户定义）：删后自动切到最近更新的会话；全空则新建无工作区会话
        json arr = json::array();
        for (auto& s : index_["sessions"])
            if (s.is_object() && s.value("id", "") != id) arr.push_back(s);
        index_["sessions"] = arr;
        save_index_();
        std::error_code ec;
        fs::remove(home_ / "sessions" / (id + ".transcript.jsonl"), ec);
        fs::remove(home_ / "sessions" / (id + ".state.json"), ec);
        if (id == session_) {
            std::string next;
            long long best = -1;
            for (auto& s : index_["sessions"]) {
                const long long u = s.value("updated", 0LL);
                if (u > best) { best = u; next = s.value("id", ""); }
            }
            if (!next.empty()) {
                switch_to_(next);
            } else {
                // 全删 ⇒ 无会话态：注册表只含已初始化会话，前端 loadSessions 后进草稿态
                session_ = "";
                index_["active"] = "";
                save_index_();
                records_.clear();
                if (out_.is_open()) out_.close();
            }
        }
        return json_resp({{"ok", true}});
    }

    // ---- 输入与裁决 -------------------------------------------------------------

    bool host_running_() const {
        return true;  // 本插件活着 = 宿主活着（HTTP 服务在同进程内）
    }

    // 发送管线（select 线程共用；/api/input 与 /api/session/new 同走这一条）：
    // 落盘用户记录 + 延迟广播/开 turn。NOTE: handle 内禁 broadcast——EDEADLOCK
    // （踩过：POST /api/input 即 500）。
    void start_turn_(const std::string& text) {
        in_flight_.store(true);
        persist_status_();   // turn 开始（聚合徽标同步）
        json rec = {{"type", "user"}, {"text", text}};
        persist_(rec);   // 只落盘；广播 defer 到宿主线程
        nostos::kit::poster_submit(poster_, [this, text, rec] {
            broadcast_(rec);                       // 先广播用户消息
            const vc_api::TurnInput e{text.c_str()};
            host_.emit(vc_api::turn_input, &e);    // 再开 turn（顺序保证）
        });
    }

    /// POST /api/input {line} → 发送后续消息：落盘 + 延迟广播 + 开 turn；空输入或
    /// 无会话态 400。
    http::Response api_input_(const std::string& body) {
        try {
            json in = parse_body_(body);
            std::string line = in.value("line", "");
            if (line.empty()) return json_resp({{"ok", false}, {"error", "空输入"}});
            if (session_.empty())
                return json_resp({{"ok", false},
                                  {"error", "无活跃会话——发送首条消息时自动创建"}}, 400);
            start_turn_(line);
            return json_resp({{"ok", true}});
        } catch (const std::exception& e) {
            return json_resp({{"ok", false}, {"error", e.what()}});
        }
    }

    /// POST /api/decision {decision 0-4, paths?, msg?} → 裁决提案：0 批准所选 /
    /// 1 拒绝 / 2 免批 / 3 撤销 / 4 取消；任意裁决终结当前 pending。
    http::Response api_decision_(const std::string& body) {
        json in = parse_body_(body);
        const int d = in.value("decision", -1);
        if (d < 0 || d > 4) return json_resp({{"ok", false}, {"error", "decision 越界"}});
        // 逐文件批准：{paths:["a","b"]} ⇒ 只落盘接受的文件；缺省 = 全部
        std::string paths_json;
        if (in.contains("paths") && in["paths"].is_array() && !in["paths"].empty())
            paths_json = in["paths"].dump();   // 裸数组：agent 侧 keep.is_array() 直读
        // 拒绝反馈：{msg:"..."} 随拒绝回灌给 agent（ZCode 式"拒绝并告诉它怎么做"）
        const std::string msg = in.contains("msg") && in["msg"].is_string()
                                    ? in["msg"].get<std::string>() : "";
        const int turn = (d == 0 || d == 1) ? pending_turn_ : 0;  // undo/cancel 不看 turn
        // NOTE: 取消（d=4）后 provider 侧不发 done/error（mock 直接静默终止），agent
        // 只发 notice——in_flight_ 无人清零 ⇒ UI 永远卡「生成中」（踩过：停止键卡死）。
        // 用户主动终止 = UI 必须立即解锁。
        if (d == 4) in_flight_.store(false);
        persist_status_();   // 取消 ⇒ 解锁（聚合徽标同步）
        nostos::kit::poster_submit(poster_, [this, turn, d, paths_json, msg] {
            const vc_api::TurnDecision e{turn, d, paths_json.c_str(), msg.c_str()};
            host_.emit(vc_api::turn_decision, &e);
        });
        persist_status_();   // 裁决 ⇒ 提案清零（聚合徽标同步）
        pending_turn_ = -1;   // 任意裁决（含免批/撤销/取消）都终结当前提案——重启后
                              // pending 恒为 false，前端决策坞以后端事实为准（防幻影）
        return json_resp({{"ok", true}});
    }

    /// POST /api/permission-mode {mode} → 权限模式持久化 + 热切。
    http::Response api_permission_mode_(const std::string& body) {
        json in = parse_body_(body);
        const std::string mode = vc_api::normalize_permission_mode(in.value("mode", ""));
        config_["permission_mode"] = mode;
        persist_config_();
        nostos::kit::poster_submit(poster_, [this, mode] {
            const vc_api::PermissionMode e{mode.c_str()};
            host_.emit(vc_api::permission_mode, &e);
        });
        return json_resp({{"ok", true}, {"mode", mode}});
    }

    /// POST /api/model {model} → 模型热切 + 配置持久化 + 未知模型自动建档；
    /// 缺槽/失败不报错，落盘通告重启生效（热切成功完全静默，防通告刷屏）。
    http::Response api_model_(const std::string& body) {
        json in = parse_body_(body);
        std::string model = in.value("model", "");
        if (model.empty()) return json_resp({{"ok", false}, {"error", "模型为空"}});
        const auto* llm = llm_;   // activate 时已解析（select 线程不能 service）
        bool live = false;
        std::string diag;
        if (llm == nullptr) diag = "provider 不在（llm 为空）";
        else if (llm->set_model == nullptr) diag = "provider 未实现 set_model";
        else {
            const nostos_status st = llm->set_model(llm->self, model.c_str());
            live = (st == NOSTOS_OK);
            if (!live) diag = "set_model 返回 " + std::to_string(static_cast<int>(st));
        }
        config_["model"] = model;  // 持久化（下次启动同模型）
        // 未知模型 ⇒ 自动建档进「当前使用供应商」的模型列表
        for (auto& p : config_["providers"])
            if (p.is_object() && p.value("name", "") == config_.value("current_provider", "")) {
                bool known = false;
                for (const auto& m : p["models"])
                    if (m.is_object() && m.value("id", "") == model) { known = true; break; }
                if (!known)
                    p["models"].push_back(json{{"id", model}, {"context", 128000},
                                               {"enabled", true}});
                break;
            }
        persist_config_();
        // 热切成功完全静默（下拉已反映，通告只会刷屏——用户要求"其他操作一律不显示"）；
        // 非实时生效（缺槽/失败）才落盘通告——这是必须可见的状态偏差
        if (!live) {
            json rec = json{{"type", "notice"},
                            {"level", 2},
                            {"text", "模型已保存为 " + model + "（重启后生效；" + diag + "）"}};
            persist_(rec);  // 只落盘；广播 defer 到宿主线程（handle 内禁 broadcast）
            nostos::kit::poster_submit(poster_, [this, rec] { broadcast_(rec); });
        }
        return json_resp({{"ok", true}, {"live", live}});
    }

    /// POST /api/pick-folder → 原生目录对话框（独立线程弹，防模态阻塞），结果经
    /// SSE folder_picked 事件返回。ITHACA_TEST_PICK_PATH 时跳过对话框直接返回。
    http::Response api_pick_folder_() {
        // UI 自动化测试钩子：设置 ITHACA_TEST_PICK_PATH ⇒ 跳过原生对话框，沿同一
        // folder_picked 事件路径返回（poster defer——handle 内禁 broadcast）
        if (const char* v = std::getenv("ITHACA_TEST_PICK_PATH"); v != nullptr && *v != '\0') {
            nostos::kit::poster_submit(poster_, [this, p = std::string(v)] {
                broadcast_({{"type", "folder_picked"}, {"path", p}});
            });
            return json_resp({{"ok", true}, {"path", std::string(v)}});
        }
        const std::string initial = config_.value("workspace", "");
        std::thread([this, initial] {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const bool com = SUCCEEDED(hr);
            const std::string picked =
                com ? pick_folder_dialog("选择工作文件夹", initial)
                    : std::string();
            if (com) CoUninitialize();
            if (!picked.empty()) broadcast_({{"type", "folder_picked"}, {"path", picked}});
        }).detach();
        return json_resp({{"ok", true}});
    }
    /// GET /api/shell/sessions → 聚合会话清单（多实例壳模式）。数据全部来自磁盘：
    /// 壳 manifest（ITHACA_SHELL_MANIFEST env，未设 = 非壳模式返回 shell:false）+
    /// 各实例 home 的 index.json（标题/时间/真实会话 id）、port（存活）、status.json
    /// （working/pending 徽标）。本实例自己的行会照常列出——前端以 /api/session 的
    /// id 判定哪一行是"当前页"。文件缺失/损坏的实例按尽力而为字段返回。
    http::Response api_shell_sessions_() {
        const char* mf = std::getenv("ITHACA_SHELL_MANIFEST");
        if (mf == nullptr || *mf == '\0')
            return json_resp({{"ok", true}, {"shell", false}});
        const fs::path manifest(mf);
        const fs::path hosts_root = manifest.parent_path() / "hosts";
        json rows = json::array();
        std::ifstream in(manifest, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            const size_t t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            const std::string key = line.substr(0, t1);
            if (key == "ACTIVE") continue;
            const std::string ws = line.substr(t1 + 1);
            const fs::path home = hosts_root / key;
            json row = {{"key", key},
                        {"id", key},
                        {"title", ""},
                        {"updated", 0},
                        {"workspace", ws},
                        {"alive", false},
                        {"port", 0},
                        {"working", false},
                        {"pending", false}};
            std::error_code ec;
            const fs::path idx_f = home / "index.json";
            std::string idx_text;
            if (fs::exists(idx_f, ec)) {
                std::ifstream idx_in(idx_f, std::ios::binary);
                std::ostringstream buf;
                buf << idx_in.rdbuf();
                idx_text = buf.str();
            } else {
                // 未迁移（从没被点开过）：从中央库条目兜底取标题/时间/工作区
                const fs::path central = manifest.parent_path() / "index.json";
                if (fs::exists(central, ec)) {
                    std::ifstream cin2(central, std::ios::binary);
                    std::ostringstream buf;
                    buf << cin2.rdbuf();
                    const std::string ctext = buf.str();
                    const std::string idkey = "\"id\":\"" + key + "\"";
                    const size_t k = ctext.find(idkey);
                    if (k != std::string::npos) {
                        const size_t b = ctext.rfind('{', k);
                        const size_t e = ctext.find('}', k);
                        if (b != std::string::npos && e != std::string::npos)
                            idx_text = ctext.substr(b, e - b + 1);
                    }
                }
            }
            if (!idx_text.empty()) {
                json idx = json::parse(idx_text, nullptr, false);
                if (idx.is_object()) {
                    // 两种形态：实例 index（{active, sessions:[…]}) / 中央条目块
                    //（直接就是会话对象）。取"活会话"条目填标题/时间/工作区
                    const json* sess = nullptr;
                    if (idx.contains("sessions") && idx["sessions"].is_array()) {
                        const std::string active_id = idx.value("active", "");
                        row["active_id"] = active_id;
                        for (const auto& e : idx["sessions"]) {
                            if (e.is_object() && e.value("id", "") == active_id) {
                                sess = &e;
                                break;
                            }
                        }
                    } else if (idx.value("id", "") == key) {
                        sess = &idx;
                    }
                    if (sess != nullptr) {
                        row["id"] = sess->value("id", key);
                        row["title"] = sess->value("title", "");
                        row["updated"] = sess->value("updated", 0LL);
                        const std::string w = sess->value("workspace", "");
                        if (!w.empty()) row["workspace"] = w;
                    }
                }
            }
            // alive 真值 = 探测缓存（实际健康检查）；缓存未覆盖（首轮）回退 port 文件
            {
                int port = 0;
                bool alive = false;
                {
                    std::lock_guard<std::mutex> lock(probe_mu_);
                    const auto it = probe_cache_.find(key);
                    if (it != probe_cache_.end()) {
                        alive = it->second.first;
                        port = it->second.second;
                    }
                }
                if (port == 0) {
                    const fs::path port_f = home / "port";
                    std::error_code pec;
                    if (fs::exists(port_f, pec)) {
                        std::ifstream pin(port_f, std::ios::binary);
                        if (pin >> port) alive = port > 0;   // 首轮探测未跑：乐观回退
                    }
                }
                row["port"] = port;
                row["alive"] = alive;
            }
            const fs::path st_f = home / "status.json";
            if (fs::exists(st_f, ec)) {
                std::ifstream st_in(st_f, std::ios::binary);
                json st = json::parse(st_in, nullptr, false);
                if (!st.is_discarded() && st.is_object()) {
                    row["working"] = st.value("working", false);
                    row["pending"] = st.value("pending", false);
                }
            }
            rows.push_back(row);
        }
        return json_resp({{"ok", true}, {"shell", true}, {"sessions", rows}});
    }

    /// GET /api/fs/list?path=<相对路径,空=根>&depth=<1..6> → 文件树栏的目录列举。
    /// 代理 workspace 的 list 工具（jail 越界防护/上限/symlink 跳过全在提供方），
    /// 本端点只做：黑名单目录过滤（树视图不需要的噪音，agent 的 list 保持无偏）、
    /// query 的 %XX 解码。NOTE: handle 在 select 线程上且 kit 持内部锁——禁止在
    /// 此等待 poster 排水（其他任务体里的 broadcast 会跟内部锁成 ABBA 死锁），
    /// 故直调工具表，跨线程安全由 workspace 的全工具串行锁保证。
    http::Response api_fs_list_(const std::string& target) {
        auto qval = [&target](const char* key) -> std::string {
            const std::size_t q = target.find(std::string(key) + "=");
            if (q == std::string::npos) return "";
            std::size_t e = target.find('&', q);
            if (e == std::string::npos) e = target.size();
            std::string raw = target.substr(q + std::string(key).length() + 1, e - q - std::string(key).length() - 1);
            std::string out;                     // %XX 解码（encodeURIComponent 形态；
            for (std::size_t i = 0; i < raw.size(); ++i) {  // '+' 保留——文件名可含加号）
                if (raw[i] == '%' && i + 2 < raw.size()) {
                    auto hex = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    const int hi = hex(raw[i + 1]), lo = hex(raw[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        out.push_back(static_cast<char>(hi * 16 + lo));
                        i += 2;
                        continue;
                    }
                }
                out.push_back(raw[i]);
            }
            return out;
        };
        const std::string rel = qval("path");
        const std::string depth_s = qval("depth");
        if (tools_ == nullptr) return json_resp({{"ok", false}, {"error", "workspace 不在"}});
        const json args = json{{"path", rel},
                               {"depth", depth_s.empty() ? 1 : std::clamp(std::atoi(depth_s.c_str()), 1, 6)}};
        const char* out_json = nullptr;
        // 直调工具表（线程安全见上）：NOSTOS_OK + {ok:false} = 工具级失败，原样透传
        const nostos_status st = tools_->execute(tools_->self, "list", args.dump().c_str(), 0,
                                                 &out_json);
        json r = st == NOSTOS_OK && out_json != nullptr
                     ? json::parse(out_json, nullptr, false)
                     : json{{"ok", false}, {"error", "workspace execute 失败 (" +
                                                  std::to_string(static_cast<int>(st)) + ")"}};
        if (r.is_discarded() || !r.is_object())
            r = json{{"ok", false}, {"error", "workspace 返回了无效 JSON"}};
        // 黑名单：一级目录名精确匹配（树视图噪音；agent 的 list 工具不受影响）
        static const char* kHide[] = {".git",           "node_modules", "build",
                                      "dist",           ".ithaca-data", "__pycache__",
                                      ".vscode",        ".zcode"};
        if (r.value("ok", false) && r.contains("entries") && r["entries"].is_array()) {
            json kept = json::array();
            for (auto& e : r["entries"]) {
                if (!e.is_object()) continue;
                const std::string p = e.value("path", "");
                const std::string seg = p.substr(0, p.find('/'));
                if (std::find(std::begin(kHide), std::end(kHide), seg) == std::end(kHide))
                    kept.push_back(std::move(e));
            }
            r["entries"] = std::move(kept);
        }
        return json_resp(std::move(r));
    }
    // GET /api/mcp/status → mcp 插件回发的 server 状态（home/mcp-status.json，
    // 由 mcp 插件在 activate/reload/server 变化时改写）。文件缺失/损坏 ⇒ 空清单。
    http::Response api_mcp_status_() {
        json fallback = {{"ok", true}, {"servers", json::array()}};
        std::error_code ec;
        const fs::path f = home_ / "mcp-status.json";
        if (!fs::exists(f, ec)) return json_resp(fallback);
        std::ifstream in(f, std::ios::binary);
        const json st = json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (!st.is_object() || !st.contains("servers") || !st["servers"].is_array())
            return json_resp(fallback);
        return json_resp(st);
    }

    // ---- 会话元信息（/api/session 的响应源；多会话/切换经 vc.session.load）----

    json session_meta_() {
        // pending：后端当前真有挂起提案（运行时事实，重启即 false）——前端决策坞
        // 以此为门卫，防止任何来源的幻影提案弹审批

        int turns = 0;
        long long tokens = 0;
        std::string title = "(新会话)";
        for (const auto& r : records_) {
            const std::string type = r.value("type", "");
            if (type == "user") {
                ++turns;
                if (title == "(新会话)") {
                    std::string t = r.value("text", "");
                    if (t.size() > 48) t = vcws::trim_utf8(t, 48) + "…";  // 码点安全
                    title = t;
                }
            }
            if (type == "usage") tokens += r.value("total", 0LL);
        }
        std::string ws;
        if (json* e = session_entry_(session_); e != nullptr)
            ws = e->value("workspace", "");
        return {{"id", session_},
                {"title", title},
                {"turns", turns},
                {"tokens", tokens},
                {"workspace", ws},
                {"in_flight", in_flight_.load()},   // 后端原子真值：前端重放后对账防 zombie
                {"records", records_.size()},
                {"pending", pending_turn_ >= 0}};
    }

    // ---- 状态 -------------------------------------------------------------------

    nostos::abi::Host host_{};
    nostos_poster poster_{};
    std::shared_ptr<http::Server> server_;   // shared：pick 线程持有引用防悬垂
    std::vector<nostos::abi::Host::Subscription> subs_;

    const vc_api::LlmTable* llm_ = nullptr;  // activate 时解析（模型热切用）
    const vc_api::ToolTable* tools_ = nullptr;  // workspace 工具表（文件树后端，见 activate）

    // ---- 兄弟实例探测（聚合 alive 真值，见 start_prober_）----------------------
    std::thread prober_;
    std::atomic<bool> prober_run_{false};
    std::mutex probe_mu_;
    std::map<std::string, std::pair<bool, int>> probe_cache_;   // key → {alive, port}
    fs::path manifest_path_;
    std::string llm_mode_ = "cloud";   // 供应商模式：mock（测试）| cloud（默认，见 activate）
    fs::path home_{};      // %USERPROFILE%\.ithaca
    fs::path web_root_{};  // SPA 目录
    std::string session_;          // 活跃会话 id；"" = 无会话态（前端草稿态兜底）
    json config_ = json::object();
    std::vector<json> records_;      // 会话记录（内存镜像 = 重放源）
    std::ofstream out_{};            // transcript 追加写
    int pending_turn_ = -1;          // 待裁决提案的 turn_id
    long long token_total_ = 0;
    json index_ = json::object();          // 会话注册表（工作区分组，
    std::string host_workspace_;           // 宿主工作目录（新会话归属）

    // ---- 文件树 watcher 状态（见 start_fs_watcher_）---------------------------
    HANDLE fs_watch_handle_ = nullptr;     // 主线程开/关；线程只持它调 ReadDirectoryChangesW
    std::thread fs_watch_thread_;
    std::atomic<bool> fs_watch_running_{false};
    std::atomic<bool> in_flight_{false};   // turn 进行中（禁切换/新建）
};

}  // namespace

NOSTOS_PLUGIN(WebuiPlugin)
