// plugin/mcp —— MCP 客户端插件：通用 MCP 工具接入。
//
// 翻译单元职责地图（按功能切片，数据结构与类声明为本文件）：
//   src/plugin.cpp   —— 生命周期（activate/reload_async_/deactivate）、状态回发
//                       落盘（emit_status_）+ NOSTOS_PLUGIN 入口
//   src/session.cpp  —— stdio 传输与 JSON-RPC 会话（ChildPipes/McpSession 方法定义）
//   src/config.cpp   —— config.json 读取与 server 拉起握手（initialize + tools/list）
//   src/tools.cpp    —— 统一工具表（list/execute thunk）+ 会话查找
//
// 职责：按 config.json 的 mcp.servers 拉起 MCP server 子进程（JSON-RPC 2.0 /
// stdio 换行分帧），initialize + tools/list 发现工具，以统一工具表
// （ithaca.tool.mcp）发布给 agent——工具全名 mcp.<server>__<tool>，execute 同步
// 转发 tools/call（120s 超时）。MCP 协议细节不进内核；审批/沙箱/快照等基础设施
// 对 MCP 工具与内置工具一视同仁（内核边界：MCP 属应用层）。
//
// 范围（MVP）：仅 stdio 传输 + tools；resources/prompts、HTTP 传输、
// tools/list_changed 热刷新、server→client 请求（sampling/roots）不做。
//
// 线程纪律：activate/reload 在宿主线程序列化拉起各 server（握手阻塞 ≤30s/server，
// 冷启动容忍）；读线程逐 server 一个（stdout 阻塞读，进程退出即管道断裂退出）；
// execute 在 agent 线程同步等 future。锁：pending/写串行化各一把会话锁；
// sessions_ 表由 activate/deactivate/reload 独占变更（unique_ptr 持有，指针稳定）。
//
// 状态可见性：activate/reload/server 变化后，把 {servers:[{name,alive,tools,diag}]}
// 回发为 vc.mcp.status 事件（webui 订阅缓存，供设置面板状态行）并落盘
// home/mcp-status.json（webui /api/mcp/status 直接读取，跨重启可用）。

#pragma once

#include "vc_tool.hpp"  // vc_api::ToolTable：统一工具表契约（publish 给宿主）

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/poster.hpp>  // nostos_poster + poster_submit：状态回发 defer

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // HANDLE：子进程管道句柄
#endif

namespace fs = std::filesystem;

namespace vc_mcp {

using json = nlohmann::json;

inline constexpr int kInitTimeoutMs = 30000;   // initialize/tools-list 握手超时（Windows npx/cmd 启动链可达 10s+）
inline constexpr int kCallTimeoutMs = 120000;  // tools/call 超时（MVP 固定）

// ---------- 子进程（stdio 管道重定向；MCP stdio 传输载体）--------------------

struct ChildPipes {
    HANDLE proc = nullptr;
    HANDLE stdin_w = nullptr;    // 写：JSON-RPC 请求/通知
    HANDLE stdout_r = nullptr;   // 读：JSON-RPC 响应/通知
    HANDLE stderr_r = nullptr;   // 读：server 诊断输出（独立管道，不污染 JSON 流）

    bool spawn(const std::string& command, const std::vector<std::string>& args,
               const json& env_over, const std::string& cwd);
    bool write_line(const std::string& line);
    void kill();

#ifdef _WIN32
private:
    HANDLE stdin_r_ = nullptr;   // 子进程的 stdin 读端
    HANDLE stdout_w_ = nullptr;  // 子进程的 stdout 写端
    HANDLE stderr_w_ = nullptr;  // 子进程的 stderr 写端
public:
#endif
};

// ---------- 一个 MCP server 会话（进程 + 读线程 + 在途请求）-------------------

struct McpSession {
    std::string name;                  // 配置名（工具全名 mcp.<name>__<tool>）
    ChildPipes proc;
    std::thread reader;
    std::thread stderr_drain;
    std::atomic<long long> next_id{1};
    std::mutex mu;                     // 保护 pending 与 stdin 写串行化
    std::map<long long, std::promise<json>> pending;
    json tools = json::array();        // tools/list 结果（name/description/inputSchema）
    std::atomic<bool> alive{false};
    std::string diag;                  // 最后一次失败原因（状态/日志点名用）
    std::string stderr_log;            // server stderr 前 400 字节（失败诊断）

    bool request(const std::string& method, const json& params, int timeout_ms, json& out);
    void feed(const std::string& line);   // 读线程：分发响应
    void start_reader();
    void start_stderr_drain();
    void shutdown();
};

// ---------- 插件 ------------------------------------------------------------

class McpPlugin {
public:
    static constexpr std::string_view name = "vc.mcp";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 1, 0);

    nostos_status activate(const nostos::abi::Host& host);
    // 首拉/热重载入口：立即回发当前状态（拉起中），server 全部后台拉起
    void reload_async_();
    void deactivate() noexcept;

private:
    static constexpr const char* kToolSvcName = "ithaca.tool.mcp";

    // ---- 配置：env ITHACA_MCP_CONFIG 指定独立文件 > webui 数据目录 config.json
    //      （src/config.cpp）-------------------------------------------------

    fs::path config_path_() const;
    void load_config_and_start_();
    void start_server_(const std::string& name, const std::string& command,
                       const std::vector<std::string>& args, const json& env,
                       const std::string& cwd);
    // 插件 UI 贡献：向 webui 声明本插件设置 schema（握手/调用超时）
    void emit_ui_register_();

    // ---- 状态回发 + 落盘（activate/reload/server 变化后调用；宿主线程独占）---

    void emit_status_();

    // ---- 统一工具表（契约 vc_tool.hpp：list 自述 + execute 转发，src/tools.cpp）----

    static nostos_status list_thunk(void* self, const char** out_json);
    static nostos_status exec_thunk(void* self, const char* name, const char* args_json,
                                    int /*turn_id——MCP 无快照组语义，忽略*/,
                                    const char** out_json);
    std::string execute(const std::string& name, const std::string& args_json);
    McpSession* find_session_(const std::string& name);
    std::size_t live_count() const;

    // ---- 状态 -------------------------------------------------------------------

    nostos::abi::Host host_{};
    nostos_poster poster_{};
    std::atomic<bool> stopping_{false};   // 停用/再重载置位：过期的后台拉起结果丢弃
    vc_api::ToolTable table_{};
    std::string list_buf_, exec_buf_, status_buf_;   // 调用期有效（消费方同步拷贝）
    std::mutex mu_;                     // 保护 sessions_（activate/deactivate/reload 与 agent 线程交叠）
    std::vector<std::unique_ptr<McpSession>> sessions_;
    std::vector<nostos::abi::Host::Subscription> subs_;

    // 超时可配置（webui 设置面板 plugins.mcp 节；默认 = 契约级 MVP 固定值）
    int init_timeout_ms_ = kInitTimeoutMs;
    int call_timeout_ms_ = kCallTimeoutMs;
};

}  // namespace vc_mcp
