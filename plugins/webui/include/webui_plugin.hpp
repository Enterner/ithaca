// plugin/webui —— Web 前端插件（正式 UI）。
//
// 翻译单元职责地图（按功能切片，类声明为本文件）：
//   src/plugin.cpp     —— 生命周期（activate/deactivate）、HTTP 路由 handle、
//                         通用工具（parse_body_/json_resp/mime_of/file_/broadcast_）+ NOSTOS_PLUGIN 入口
//   src/picker.cpp     —— 原生选目录对话框（IFileDialog + CBT 置顶钩子）
//   src/events.cpp     —— 事件总线订阅（14 个事件回调 → 落盘 + SSE 广播）
//   src/sessions.cpp   —— 会话注册表/transcript 持久化 + /api/session* 端点族
//   src/config.cpp     —— 供应商两层配置（共享 + 实例覆盖层）+ /api/config
//   src/fs_watcher.cpp —— 文件树真监听（ReadDirectoryChangesW → fs_changed 广播）
//   src/prober.cpp     —— 兄弟实例健康探测（聚合 alive 真值）+ /api/shell/sessions
//   src/api.cpp        —— 输入/裁决/模型/选目录/文件树/MCP 状态/模型清单/上下文构成端点
//
// 总职责：kit::http serve SPA + REST + SSE；订阅事件总线（tui 同款事件面）⇒
// JSONL 持久化 + SSE 广播；输入/裁决经 poster 回总线。会话持久化的两层：
//   · transcript.jsonl（本插件写，展示事件流，每条 flush）
//   · state.json（agent 写，LLM 上下文；路径由启动器按活跃会话给定）
//
// 线程纪律：Handler::handle 在 select 线程（必须快——poster_submit/
// 小文件写都非阻塞）；事件回调在宿主线程（drain_posted 串行化）；broadcast
// 内核注明线程安全。两线程共享的数据面（records_/session_/pending_turn_/index_/
// out_/agent_logs_* 等，见 data_mu_）以递归互斥串行化；**锁内禁止 broadcast_ 与
// host_.emit**——handle 在 kit select 循环内部锁内被调，持 data_mu_ 再广播/发射
// = 与之成 ABBA 死锁（EDEADLOCK 同族）。config_ 只在 select 线程触碰（activate
// 完成于服务起请求前），prober 面自带 probe_mu_。

#pragma once

#include "vc_llm.hpp"
#include "vc_tool.hpp"  // vc_api::ToolTable：文件树代理调用的 workspace 工具表形状

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/http.hpp>
#include <nostos/kit/poster.hpp>  // poster_submit：多个翻译单元共用
#include <nostos/kit/win_http.hpp>  // 一发 HTTP 请求小件（探测/模型清单）

#include <windows.h>  // HANDLE：fs_watcher 状态成员

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <nostos/nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace http = nostos::kit::http;
namespace win = nostos::kit::win;  // 一发 HTTP 请求小件（探测/模型清单）

namespace vc_webui {

using json = nlohmann::json;

// ---- 原生选目录对话框（UI 原语，自内核 kit::win 下沉到唯一消费方；置顶修复随迁）--

// 弹出"选择文件夹"对话框（IFileDialog + FOS_PICKFOLDERS：Vista 现代样式，
// Explorer 同款面包屑/侧栏/搜索；老 SHBrowseForFolderW 是 Win95 树形对话框）。
// 返回选中绝对路径（UTF-8）；取消/失败返回空串。调用线程需 COM 已初始化。
//
// 宿主是无窗口进程，无主对话框会沉底（被浏览器页面盖住 ⇒ "点了没反应"）——
// 激活瞬间经本线程挂的 CBT 钩子置顶（真实踩过）。
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
    // 子代理工作明细存档（ZCode 式独立存放，src/sessions.cpp）：sessions/agents/
    // <会话id>/agent_<turn>.jsonl 追加写 + SSE 广播——明细不进主 transcript，
    // 主会话只留状态卡；右栏过程视图经 /api/agent-log 按需拉取，行号 seq 单调
    // （重启按现存行数续接），前端据此合并「拉取历史」与「实时增量」不重不漏
    void agent_log_append_(int turn_id, const json& rec);
    void close_agent_logs_();
    http::Response api_agent_log_(const std::string& target);

    // ---- 配置（src/config.cpp）----------------------------------------------------

    void load_config_();
    fs::path config_path_() const;
    bool config_shared() const noexcept;
    void merge_local_overlay_();
    bool persist_config_();
    json config_public_();
    http::Response api_config_save_(const std::string& body);
    // 组装并投递 vc.llm.set_active（provider/key/base_url + 当前模型窗口）——
    // activate 对齐、配置保存热切、模型热切补发共用一个出口（窗口字段单点维护）。
    void post_llm_set_active_();

    // ---- 会话注册表（src/sessions.cpp）------------------------------------------

    void load_index_();
    void save_index_();
    json* session_entry_(const std::string& id);
    static long long now_epoch_();
    void ensure_session_(const std::string& id, const std::string& workspace);
    void sweep_uninitialized_();
    void switch_to_(const std::string& id);
    // 工作区补发（tool_register 兜底）：workspace 工具表后到 ⇒ 启动恢复时
    // switch_to_ 发出的 WorkspaceSet 可能先于其订阅被丢（事件总线无保留语义），
    // 按当前活跃会话重发一次（同值热切幂等）
    void backfill_workspace_();
    http::Response api_session_new_(const std::string& body);
    http::Response api_session_open_(const std::string& body);
    http::Response api_session_rename_(const std::string& body);
    http::Response api_session_delete_(const std::string& body);
    json session_meta_();

    // ---- 输入与裁决（src/api.cpp）-------------------------------------------------

    void start_turn_(const std::string& text);
    http::Response api_input_(const std::string& body);
    http::Response api_decision_(const std::string& body);
    http::Response api_permission_mode_(const std::string& body);
    http::Response api_model_(const std::string& body);
    http::Response api_provider_models_(const std::string& body);   // 拉平台模型清单（SSE 回发）
    http::Response api_context_breakdown_();   // 上下文构成分解（悬停指示器的数据源）
    http::Response api_pick_folder_();
    http::Response api_shell_sessions_();   // 壳模式聚合（实现在 src/prober.cpp）
    http::Response api_fs_list_(const std::string& target);
    http::Response api_fs_read_(const std::string& target);   // 代码查看器的文件内容
    http::Response api_mcp_status_();
    http::Response api_ui_registry_();   // 插件 UI 贡献清单（vc.ui.register 累积）

    // ---- 状态 -------------------------------------------------------------------

    nostos::abi::Host host_{};
    nostos_poster poster_{};
    std::shared_ptr<http::Server> server_;   // shared：pick 线程持有引用防悬垂
    std::vector<nostos::abi::Host::Subscription> subs_;

    const vc_api::LlmTable* llm_ = nullptr;     // activate 时解析（模型热切用）
    std::atomic<const vc_api::ToolTable*> tools_{nullptr};  // workspace 工具表：
        // select 线程读（/api/fs/*）、宿主线程 tool_register 兜底写——原子装载

    // ---- 兄弟实例探测（聚合 alive 真值，见 start_prober_）----------------------
    std::thread prober_;
    std::atomic<bool> prober_run_{false};
    std::mutex probe_mu_;
    std::map<std::string, std::pair<bool, int>> probe_cache_;   // key → {alive, port}
    fs::path manifest_path_;
    std::string llm_mode_ = "cloud";   // 供应商模式：mock（测试）| cloud（默认，见 activate）
    bool mcp_reload_pending_ = false;  // mcp/plugins 节变化 → 落盘成功后才发射 McpReload（时序见 config.cpp 踩过②）
    fs::path home_{};      // %USERPROFILE%\.ithaca
    fs::path web_root_{};  // SPA 目录
    std::string session_;          // 活跃会话 id；"" = 无会话态（前端草稿态兜底）
    // select 线程（handle）与宿主线程（事件回调）共享面的互斥（递归：persist_ →
    // save_index_ → session_entry_ 等调用链同线程重入）。守护：records_/session_/
    // pending_turn_/index_/out_/agent_logs_/agent_log_seq_/host_workspace_/ui_registry_。
    // 纪律：锁内禁止 broadcast_/host_.emit（ABBA 死锁，见文件头线程纪律）。
    std::recursive_mutex data_mu_;
    json config_ = json::object();
    std::vector<json> records_;      // 会话记录（内存镜像 = 重放源）
    std::ofstream out_{};            // transcript 追加写
    std::string reasoning_buf_;      // 当前轮深度思考累积（llm_reasoning 只广播不落盘，
                                     // llm_done 收口时并入 assistant 记录——重放可回看）
    // 子代理明细存档的写句柄与序号水位（宿主线程串行化，事件回调专用；
    // 会话切换/删除时 close_agent_logs_ 统一收口）
    std::map<int, std::ofstream> agent_logs_;
    std::map<int, long long> agent_log_seq_;
    int pending_turn_ = -1;          // 待裁决提案的 turn_id
    json index_ = json::object();          // 会话注册表（工作区分组，
    std::string host_workspace_;           // 宿主工作目录（新会话归属）
    bool ws_backfilled_ = false;           // 工作区补发一次性标志（tool_register 兜底）

    // ---- 文件树 watcher 状态（见 start_fs_watcher_）---------------------------
    HANDLE fs_watch_handle_ = nullptr;     // 主线程开/关；线程只持它调 ReadDirectoryChangesW
    std::thread fs_watch_thread_;
    std::atomic<bool> fs_watch_running_{false};
    std::atomic<bool> in_flight_{false};   // turn 进行中（禁切换/新建）

    // ---- 插件 UI 贡献（vc.ui.register 累积；内存态，/api/ui/registry 数据源）----
    // 插件重发随 vc.ui.query 补拉兜底，重启后由各插件 activate 重建，无需落盘。
    std::map<std::string, json> ui_registry_;   // 插件名 → {label, schema:[…]}

    // ---- 上下文构成分解缓存（/api/context/breakdown）---------------------------
    // key = 会话文件路径@mtime：同一版本文件只解析一次（select 线程必须快）。
    std::mutex bd_mu_;
    std::string bd_key_;
    json bd_cache_;
};

}  // namespace vc_webui
