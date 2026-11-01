// plugin/webui —— Web 前端插件（正式 UI）。
//
// 翻译单元职责地图（按功能切片，类声明为本文件）：
//   src/plugin.cpp     —— 生命周期（activate/deactivate）、HTTP 路由 handle、
//                         通用工具（parse_body_/json_resp/mime_of/file_/
//                         broadcast_；query 解码用内核 Request::query 单源）+ NOSTOS_PLUGIN 入口
//   src/picker.cpp     —— 原生选目录对话框（IFileDialog + CBT 置顶钩子）+ /api/pick-folder
//   src/events.cpp     —— 事件总线订阅（15 个事件回调 → 落盘 + SSE 广播）
//   src/sessions.cpp   —— 会话注册表/transcript 持久化 + /api/session* + 上下文构成端点
//   src/config.cpp     —— 供应商两层配置（共享 + 实例覆盖层）+ /api/config + 模型清单拉取
//   src/fs_watcher.cpp —— 文件树真监听（ReadDirectoryChangesW → fs_changed 广播）
//   src/prober.cpp     —— 兄弟实例健康探测（聚合 alive 真值）+ /api/shell/sessions
//   src/api.cpp        —— 对话动作端点（输入/裁决/权限模式/模型热切/会话命令）
//   src/attachments.cpp —— 附件管线（上传落盘 + 多模态载荷 + base64 编解码 +
//                         附件图片字节出口）
//   src/api_fs.cpp     —— 工作区代理端点（文件树/查看器/MCP 状态）
//
// 总职责：kit::http serve SPA + REST + SSE；订阅事件总线（tui 同款事件面）⇒
// JSONL 持久化 + SSE 广播；输入/裁决经 poster 回总线。会话持久化的两层：
//   · transcript.jsonl（本插件写，展示事件流，每条 flush）
//   · state.json（agent 写，LLM 上下文；路径由启动器按活跃会话给定）
//
// 线程纪律（全量单线程化）：Handler::handle 在 select 线程上只做解析、校验、
// 投递与挂起返回——静态文件与 /events 零共享态，同步直答；其余端点经 defer_
// 把业务体投宿主线程后挂起，真响应走 kit 挂起-补响应原语补发。数据面触碰
// （records_/session_/pending_turn_/index_/out_/agent_logs_*/ui_registry_/
// config_ 等）全部收敛宿主线程：poster 闭包与事件回调同经宿主排水串行化，
// 无共享无锁。prober 线程自持 probe_mu_ 守护探测缓存。
// 例外：两条一次性 detach 线（供应商模型清单、选目录对话框）——网络往返/模态
// 对话框不能占宿主排水，停机也不可为它们 join（对话框要等人点）。停机纪律 =
// 线程对插件零触碰：spawn 前在宿主线程快照 server_（shared_ptr 值捕获），广播
// 直调快照；deactivate 的 server_.reset() 与之无竞态（快照保活，stop 后
// broadcast 为 no-op——连接表已清）。

#pragma once

#include "vc_llm.hpp"
#include "vc_tool.hpp"  // vc_api::ToolTable：文件树代理调用的 workspace 工具表形状
#include "vcoffice/iface.hpp"  // vcoffice::PreviewTable：/api/fs/preview 的预览转换表

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/http.hpp>
#include <nostos/kit/poster.hpp>  // poster_submit：多个翻译单元共用

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // HANDLE：fs_watcher 状态成员（宏守卫自带——kit/win.hpp 曾经
                      // 前置供 NOMINMAX，下沉到消费 TU 后守卫跟到这里）

#include <atomic>
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
#include <type_traits>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace http = nostos::kit::http;

namespace vc_webui {

using json = nlohmann::json;

// ---- 原生选目录对话框（UI 原语，自内核 kit::win 下沉到唯一消费方；置顶修复随迁）--

// 弹出"选择文件夹"对话框（IFileDialog + FOS_PICKFOLDERS：Vista 现代样式，
// Explorer 同款面包屑/侧栏/搜索；老 SHBrowseForFolderW 是 Win95 树形对话框）。
// 返回选中绝对路径（UTF-8）；取消/失败返回空串。调用线程需 COM 已初始化。
//
// 宿主是无窗口进程，无主对话框会沉底（被浏览器页面盖住 ⇒ "点了没反应"）——
// 激活瞬间经本线程挂的 CBT 钩子置顶（真实踩过）。
// 内置 mock 供应商默认模型目录（定义在 config.cpp；迁移建档/非数组回种共用）
json default_mock_models();

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

    // 挂起-补响应样板出口：fn 在宿主线程执行（注入连接 id，闭包内自行 respond），
    // 本调用立即挂起返回；post 失败（activate 漏领/会话收尾）没人会补响应 ⇒
    // 同步 503 防连接悬挂。fn 必须返回 void——闭包里顺手 `return json_resp(...)`
    // 会被 auto 推导合法化成"构造完丢弃"，连接永不补响应（编译期挡下）。
    // NOTE: 闭包异常经 poster 的异常钩子就地兜成 500 补发——poster 吞异常且挂起
    // 连接豁免 stall 超时，不补即永久占一个连接槽（max_clients 占满 = 整个服务
    // 停摆）；磁盘字段类型漂移（json::value<T> 严格取值抛 type_error）是真实
    // 触发源。钩子原语在内核（kit poster_submit 第三参），本插件只提供补发出口。
    template <typename F>
        requires std::is_void_v<std::invoke_result_t<F&, std::uint64_t>>
    http::Response defer_(const http::Request& req, F&& fn) {
        const std::uint64_t conn = req.conn_id;
        const nostos_status st = nostos::kit::poster_submit(
            poster_, [this, conn, fn = std::forward<F>(fn)]() mutable { fn(conn); },
            [this, conn](std::exception_ptr e) {
                try {
                    if (e) std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    fail_suspended_(conn, std::string("内部错误: ") + ex.what());
                } catch (...) {
                    fail_suspended_(conn, "内部错误（未知异常）");
                }
            });
        if (st != NOSTOS_OK)
            return json_resp({{"ok", false}, {"error", "宿主未接收投递"}}, 503);
        http::Response r;
        r.suspended = true;
        return r;
    }

    // 挂起连接的失败补发（defer_ 专用出口）：连接已挂起，常规 respond 补 500。
    void fail_suspended_(std::uint64_t conn, const std::string& what) {
        if (!server_) return;
        try {
            server_->respond(conn, json_resp({{"ok", false}, {"error", what}}, 500));
        } catch (...) {
        }
    }

    // 只读端点的 defer 样板收敛：fn 在宿主线程执行、返回值即响应体（json_resp
    // 补头/状态码），conn 寻址与补发收在闭包内。fn 无参——读取端点用不到 conn；
    // 需要 conn 寻址的写路径仍走 defer_。
    template <typename F>
    http::Response defer_json_(const http::Request& req, F&& fn) {
        return defer_(req, [this, fn = std::forward<F>(fn)](std::uint64_t conn) {
            server_->respond(conn, json_resp(fn()));
        });
    }

    // defer_ 闭包内的错误补发出口：ok:false + error（与 respond 直调同语义）
    void respond_err_(std::uint64_t conn, const std::string& msg, int status = 200) {
        server_->respond(conn, json_resp({{"ok", false}, {"error", msg}}, status));
    }

    static std::string mime_of(const fs::path& p);
    http::Response file_(const std::string& rel);
    void broadcast_(json rec);

    // ---- 会话记录（src/sessions.cpp）--------------------------------------------

    // owner = 记录归属会话（事件尾部 session 标解析，多会话并行各自落盘）：
    // 活跃会话走内存镜像 + out_ 句柄（重放源）；其余会话直写各自 transcript
    // 文件（sink_ids_ 水位独立续号，切回时整卷重放恰好接续）
    void persist_(json& rec, const std::string& owner);
    // tag = 事件的 session 标（契约尾部字段；null/空回落活跃会话）：落盘按归属，
    // 广播副本带归属标（前端按标过滤非当前会话的 turn 事件）
    void record_(json rec, const char* tag);
    // turn 事件的广播出口（只广播不落盘的流式增量/明细）：同 record_ 打归属标
    void broadcast_turn_(json rec, const char* tag);
    // 事件 session 标 → 归属会话（空标回落活跃会话——旧生产者组合兜底）
    std::string ev_owner_(const char* tag) const {
        return (tag != nullptr && *tag != '\0') ? std::string(tag) : session_;
    }
    void load_transcript_();
    void open_transcript_();
    // 子代理工作明细存档（sessions/agents/<会话id>/agent_<turn>.jsonl 追加写 +
    // SSE 广播；行号 seq 单调续接，前端历史+实时合并不重不漏）。owner = 父会话
    void agent_log_append_(const std::string& owner, int turn_id, const json& rec);
    void close_agent_logs_();
    json api_agent_log_(int turn);   // 数据源：路由 defer 包装（turn 由路由层 req.query 解出）

    // ---- 配置（src/config.cpp）----------------------------------------------------

    void load_config_();
    fs::path config_path_() const;
    bool config_shared() const noexcept;
    void merge_local_overlay_();
    bool persist_config_();
    json config_public_();
    http::Response api_config_save_(const http::Request& req);
    // 插件 UI 贡献清单（vc.ui.register 累积；前端设置面板的动态节）
    json api_ui_registry_();
    // 供应商模型清单拉取（设置面动作，SSE provider_models 回发；网络在 detach 线程）
    http::Response api_provider_models_(const http::Request& req);
    // 组装并投递 vc.llm.set_active（activate 对齐/配置保存热切/模型热切共用出口）
    void post_llm_set_active_();

    // ---- 会话注册表（src/sessions.cpp）------------------------------------------

    void load_index_();
    void save_index_();
    json* session_entry_(const std::string& id);
    // 会话条目的 workspace（未注册 = 空串）
    std::string session_ws_(const std::string& id) const;
    // 撤回转录面（会话命令 cmd=2，与 agent 的上下文撤回同令并行）：从最后一条
    // user 记录起整体回退——transcript 文件截断 + 内存镜像裁剪 + 会话计数回退 +
    // 子代理存档删除，广播回退信号（前端按位裁剪与游标对齐）。守卫：最后一条
    // 记录必须是中断卡（尾部中断现场，agent 侧同款）——返回 false = 不适格
    bool rollback_interrupt_transcript_();
    // 运行池工作区一致性：任一在跑 turn 的会话工作区与 ws 不同 ⇒ true
    //（workspace 插件单实例 jail——跨工作区并行会文件面错配）。空 ws 不换
    // jail，恒 false（新会话/未设置工作区的会话不构成冲突）
    bool ws_conflicts_running_(const std::string& ws) const;
    static long long now_epoch_();
    void ensure_session_(const std::string& id, const std::string& workspace);
    void sweep_uninitialized_();
    void switch_to_(const std::string& id);
    // 工作区补发（tool_register 兜底）：启动恢复的 WorkspaceSet 可能先于 workspace
    // 订阅被丢（总线无保留语义），按当前活跃会话重发一次（同值热切幂等）
    void backfill_workspace_();
    http::Response api_session_new_(const http::Request& req);
    http::Response api_session_open_(const http::Request& req);
    http::Response api_session_rename_(const http::Request& req);
    http::Response api_session_delete_(const http::Request& req);
    json session_meta_();
    // 上下文构成分解（悬停指示器的数据源；读活跃会话 state.json，mtime 缓存）
    json api_context_breakdown_();

    // ---- 输入与裁决（src/api.cpp）-------------------------------------------------

    void start_turn_(const std::string& text, const std::string& attachments_json);
    http::Response api_input_(const http::Request& req);
    http::Response api_decision_(const http::Request& req);
    http::Response api_permission_mode_(const http::Request& req);
    http::Response api_model_(const http::Request& req);
    http::Response api_session_cmd_(const http::Request& req);   // 会话级命令（vc.session.cmd 转发）

    // ---- 附件管线（src/attachments.cpp）-------------------------------------------

    http::Response api_upload_(const http::Request& req);   // 附件上传（base64 → 工作区 .ithaca/uploads）
    // 附件图片字节（前端缩略图/对话流预览的数据源；uploads 白名单内的图片）
    http::Response api_attachment_(const http::Request& req);
    // 附件路径 → 多模态载荷（data_url 内联；上传落点 .ithaca/uploads/ 即白名单）
    json attachments_payload_(const json& rel_paths);

    // ---- 工作区代理（src/api_fs.cpp）----------------------------------------------

    http::Response api_fs_list_(const http::Request& req);
    http::Response api_fs_read_(const http::Request& req);   // 代码查看器的文件内容
    // 版式预览（docx/pptx/doc/ppt/pdf → PDF 流）：提交 office 转换（异步回调）
    // 并挂起连接，回调里补发——见 api_fs.cpp 的实现注释
    http::Response api_fs_preview_(const http::Request& req);
    // /api/fs/* 代理共用：table 由调用方按分流选定（workspace / office）
    json call_tool_json_(const vc_api::ToolTable* table, const char* tool,
                         const std::string& args_json);
    json api_mcp_status_();

    // ---- 原生选目录（src/picker.cpp）----------------------------------------------

    http::Response api_pick_folder_(const http::Request& req);
    json api_shell_sessions_();   // 壳模式聚合（实现在 src/prober.cpp）

    // ---- 状态 -------------------------------------------------------------------

    nostos::abi::Host host_{};
    nostos_poster poster_{};
    std::shared_ptr<http::Server> server_;   // deactivate reset；两条 detach 线
        // spawn 前各自快照持引用（线程纪律见文件头）
    std::vector<nostos::abi::Host::Subscription> subs_;

    const vc_api::LlmTable* llm_ = nullptr;     // activate 时解析（模型热切用）
    const vc_api::ToolTable* tools_ = nullptr;  // workspace 工具表：activate 解析、
        // tool_register 兜底重解析；读写全在宿主线程（/api/fs/* 已迁闭包）
    const vc_api::ToolTable* office_tools_ = nullptr;  // office 工具表（可选项）：
        // /api/fs/read 的文档分流数据源；解析与重解析同 tools_ 款
    const vcoffice::PreviewTable* preview_ = nullptr;  // office 预览转换表（可选项）：
        // /api/fs/preview 的转换提交面；解析与重解析同 office_tools_ 款。
        // 异步回调触不到插件成员（per-request 堆状态，见 api_fs.cpp），本指针
        // 与 inflight_ 的读写全在宿主线程闭包内
    std::atomic<int> preview_inflight_{0};   // 在途预览转换数（闭包 ++，回调 --；
        // atomic——回调在 office 转换线程）。上限 = office 队列容量，防吃满
        // max_streams 的文件流槽位

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
    // 数据面字段（records_/session_/pendings_/running_/sinks_/sink_ids_/
    // reasoning_buf_/index_/out_/agent_logs_*/host_workspace_/ui_registry_/config_）
    // 读写全在宿主线程：poster 闭包与事件回调同经宿主排水串行化，无共享无锁
    //（线程纪律见文件头）。
    json config_ = json::object();
    std::vector<json> records_;      // 会话记录（内存镜像 = 重放源）
    std::ofstream out_{};            // 活跃会话 transcript 追加写（非活跃走 sinks_）
    // 子代理明细存档的写句柄与序号水位（宿主线程串行化，事件回调专用；
    // 会话删除时 close_agent_logs_ 统一收口）
    std::map<int, std::ofstream> agent_logs_;
    std::map<int, long long> agent_log_seq_;
    int pending_turn_ = -1;          // 待裁决提案的 turn_id
    json index_ = json::object();          // 会话注册表（工作区分组的元数据源，落盘 index.json）
    std::string host_workspace_;           // 宿主工作目录（新会话归属）
    bool ws_backfilled_ = false;           // 工作区补发一次性标志（tool_register 兜底）

    // ---- 多会话并行记账（事件归属按 session 标；读写全在宿主线程）--------------
    // turn_state 状态帧广播（SSE 瞬态不落盘）：侧栏徽标与前端 inFlight 的实时源
    void broadcast_turn_state_(const std::string& sid);
    // 会话在途 turn 收口（llm_done 终态/llm_error 两处调用）：running_ 减一，
    // 归零 = 该会话空闲 ⇒ 非活跃 sink 句柄收口。返回 true = 恰好归零
    bool chain_settle_(const std::string& owner);

    // 会话 → 在途 turn 数（turn_input 发车 +1，llm_done 终态/llm_error 收口 -1；
    // 0/缺省 = 该会话空闲）。多会话可同时 >0——真并行
    std::map<std::string, int> running_;

    // 会话 → 待裁决提案的 turn_id（提案事件随归属落；裁决即摘除）
    std::map<std::string, int> pendings_;
    // 非活跃会话的 transcript 追加句柄与 id 水位（懒开；水位在切离活跃会话时
    // 按其镜像行数捕获，后台记录续号，切回时整卷重放恰好接续）
    std::map<std::string, std::ofstream> sinks_;
    std::map<std::string, long long> sink_ids_;
    // 会话 → 当前轮深度思考累积（llm_reasoning 只广播不落盘，收口并入 assistant
    // 记录——多会话并行流交错，必须按会话分桶）
    std::map<std::string, std::string> reasoning_buf_;

    // ---- 文件树 watcher 状态（见 start_fs_watcher_）---------------------------
    HANDLE fs_watch_handle_ = nullptr;     // 主线程开/关；线程只持它调 ReadDirectoryChangesW
    std::thread fs_watch_thread_;
    std::atomic<bool> fs_watch_running_{false};

    // ---- 插件 UI 贡献（vc.ui.register 累积；内存态，/api/ui/registry 数据源）----
    // 插件重发随 vc.ui.query 补拉兜底，重启后由各插件 activate 重建，无需落盘。
    std::map<std::string, json> ui_registry_;   // 插件名 → {label, schema:[…]}

    // ---- 上下文构成分解缓存（/api/context/breakdown）---------------------------
    // key = 会话文件路径@mtime：同一版本文件只解析一次（宿主线程闭包内访问，无锁）。
    std::string bd_key_;
    json bd_cache_;
};

}  // namespace vc_webui
