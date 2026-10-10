// plugin/webui —— Web 前端插件（正式 UI，方案 docs/webui-方案.md）。
//
// 翻译单元职责地图（按功能切片，类声明为本文件）：
//   src/plugin.cpp     —— 生命周期（activate/deactivate）、HTTP 路由 handle、
//                         通用工具（parse_body_/json_resp/mime_of/file_/broadcast_）+ NOSTOS_PLUGIN 入口
//   src/picker.cpp     —— 原生选目录对话框（IFileDialog + CBT/WinEvent 钩子）
//   src/events.cpp     —— 事件总线订阅（13 个事件回调 → 落盘 + SSE 广播）
//   src/sessions.cpp   —— 会话注册表/transcript 持久化 + /api/session* 端点族
//   src/config.cpp     —— 供应商两层配置（共享 + 实例覆盖层）+ /api/config
//   src/fs_watcher.cpp —— 文件树真监听（ReadDirectoryChangesW → fs_changed 广播）
//   src/prober.cpp     —— 兄弟实例健康探测（聚合 alive 真值）+ /api/shell/sessions
//   src/api.cpp        —— 输入/裁决/模型/选目录/文件树/MCP 状态端点
//
// 总职责：kit::http serve SPA + REST + SSE；订阅事件总线（tui 同款事件面）⇒
// JSONL 持久化 + SSE 广播；输入/裁决经 poster 回总线。会话持久化的两层：
//   · transcript.jsonl（本插件写，展示事件流，每条 flush）
//   · state.json（agent 写，LLM 上下文；路径由启动器按活跃会话给定）
//
// 线程纪律（方案 §5）：Handler::handle 在 select 线程（必须快——poster_submit/
// 小文件写都非阻塞）；事件回调在宿主线程（drain_posted 串行化）；broadcast
// 内核注明线程安全。锁：无（各线程只触碰自己的数据面）。

#pragma once

#include "vc_llm.hpp"
#include "vc_tool.hpp"  // vc_api::ToolTable：文件树代理调用的 workspace 工具表形状

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/http.hpp>
#include <nostos/kit/poster.hpp>  // poster_submit：多个翻译单元共用

#include <windows.h>  // HANDLE：fs_watcher 状态成员

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace http = nostos::kit::http;

namespace vc_webui {

using json = nlohmann::json;

// ---- 原生选目录对话框（UI 原语，自内核 kit::win 下沉到唯一消费方；置顶修复随迁）--

std::wstring utf8_to_wide(const std::string& text);
std::string wide_to_utf8(const wchar_t* text);

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
std::string pick_folder_dialog(const std::string& title, const std::string& initial_dir = {});

class WebuiPlugin : public http::Handler {
public:
    static constexpr std::string_view name = "vc.webui";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 1, 0);

    nostos_status activate(const nostos::abi::Host& host);
    void deactivate() noexcept;
    http::Response handle(const http::Request& req) override;

private:
    // ---- 文件树 watcher（VSCode 式真监听，src/fs_watcher.cpp）---------------------

    void start_fs_watcher_(const std::string& root);
    void stop_fs_watcher_();

    // ---- 事件订阅（宿主线程串行化，src/events.cpp）---------------------------------

    void subscribe_();

    // ---- 兄弟实例健康探测（聚合 alive 真值，src/prober.cpp）--------------------

    void start_prober_();
    void stop_prober_();
    void persist_status_();

    // ---- 通用工具（src/plugin.cpp）----------------------------------------------

    static json parse_body_(const std::string& body);
    http::Response json_resp(json obj, int status = 200);
    static std::string mime_of(const fs::path& p);
    http::Response file_(const std::string& rel);
    void broadcast_(json rec);

    // ---- 会话记录（src/sessions.cpp）--------------------------------------------

    void persist_(json& rec);
    void record_(json rec);
    void load_transcript_();
    void open_transcript_();

    // ---- 配置（src/config.cpp）----------------------------------------------------

    void load_config_();
    fs::path config_path_() const;
    bool config_shared() const noexcept;
    void merge_local_overlay_();
    bool persist_config_();
    json config_public_();
    http::Response api_config_save_(const std::string& body);

    // ---- 会话注册表（src/sessions.cpp）------------------------------------------

    void load_index_();
    void save_index_();
    json* session_entry_(const std::string& id);
    static long long now_epoch_();
    void ensure_session_(const std::string& id, const std::string& workspace);
    void sweep_uninitialized_();
    void switch_to_(const std::string& id);
    http::Response api_sessions_();
    http::Response api_session_new_(const std::string& body);
    http::Response api_session_open_(const std::string& body);
    http::Response api_session_rename_(const std::string& body);
    http::Response api_session_delete_(const std::string& body);
    json session_meta_();

    // ---- 输入与裁决（src/api.cpp）-------------------------------------------------

    bool host_running_() const;
    void start_turn_(const std::string& text);
    http::Response api_input_(const std::string& body);
    http::Response api_decision_(const std::string& body);
    http::Response api_permission_mode_(const std::string& body);
    http::Response api_model_(const std::string& body);
    http::Response api_pick_folder_();
    http::Response api_shell_sessions_();   // 壳模式聚合（实现在 src/prober.cpp）
    http::Response api_fs_list_(const std::string& target);
    http::Response api_mcp_status_();
    http::Response api_ui_registry_();   // 插件 UI 贡献清单（vc.ui.register 累积）

    // ---- 状态 -------------------------------------------------------------------

    nostos::abi::Host host_{};
    nostos_poster poster_{};
    std::shared_ptr<http::Server> server_;   // shared：pick 线程持有引用防悬垂
    std::vector<nostos::abi::Host::Subscription> subs_;

    const vc_api::LlmTable* llm_ = nullptr;     // activate 时解析（模型热切用）
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

    // ---- 插件 UI 贡献（vc.ui.register 累积；内存态，/api/ui/registry 数据源）----
    // 插件重发随 vc.ui.query 补拉兜底，重启后由各插件 activate 重建，无需落盘。
    std::map<std::string, json> ui_registry_;   // 插件名 → {label, schema:[…]}
};

}  // namespace vc_webui
