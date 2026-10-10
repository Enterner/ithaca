// plugin/mcp —— MCP 客户端插件：通用 MCP 工具接入（方案 docs/mcp-插件-方案.md）。
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

#include "vc_tool.hpp"
#include "vc_events.hpp"

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/poster.hpp>
#include <nostos/kit/win.hpp>
#include <nostos/svc_id.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

using json = nlohmann::json;

constexpr int kInitTimeoutMs = 30000;   // initialize/tools-list 握手超时（Windows npx/cmd 启动链可达 10s+）
constexpr int kCallTimeoutMs = 120000;  // tools/call 超时（MVP 固定）

std::wstring wide(const std::string& s) {
    return nostos::kit::win::utf8_to_wide(s);
}

// ---------- 子进程（stdio 管道重定向；MCP stdio 传输载体）--------------------

struct ChildPipes {
    HANDLE proc = nullptr;
    HANDLE stdin_w = nullptr;    // 写：JSON-RPC 请求/通知
    HANDLE stdout_r = nullptr;   // 读：JSON-RPC 响应/通知
    HANDLE stderr_r = nullptr;   // 读：server 诊断输出（独立管道，不污染 JSON 流）

    bool spawn(const std::string& command, const std::vector<std::string>& args,
               const json& env_over, const std::string& cwd) {
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        if (!CreatePipe(&stdin_r_, &stdin_w, &sa, 0)) return false;
        if (!CreatePipe(&stdout_r, &stdout_w_, &sa, 0)) return false;
        if (!CreatePipe(&stderr_r, &stderr_w_, &sa, 0)) return false;
        // 子进程用不到的父端句柄关掉继承位，防句柄泄漏进子进程树
        SetHandleInformation(stdin_w, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stdout_r, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stderr_r, HANDLE_FLAG_INHERIT, 0);

        std::wstring cmd = L"\"" + wide(command) + L"\"";
        for (const auto& a : args) cmd += L" \"" + wide(a) + L"\"";

        std::wstring env_block;
        const bool has_env = env_over.is_object() && !env_over.empty();
        if (has_env) {
            std::map<std::wstring, std::wstring> merged;
            if (LPWCH block = ::GetEnvironmentStringsW()) {
                for (LPWCH p = block; *p != L'\0';) {
                    const std::wstring kv(p);
                    p += kv.size() + 1;
                    const auto eq = kv.find(L'=');
                    if (eq != std::wstring::npos && eq != 0)
                        merged[kv.substr(0, eq)] = kv.substr(eq + 1);
                }
                FreeEnvironmentStringsW(block);
            }
            for (auto it = env_over.begin(); it != env_over.end(); ++it)
                if (it.value().is_string())
                    merged[wide(it.key())] = wide(it.value().get<std::string>());
            for (const auto& kv : merged) {
                env_block += kv.first + L"=" + kv.second;
                env_block.push_back(L'\0');
            }
            env_block.push_back(L'\0');   // 环境块以双 NUL 结尾
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = stdin_r_;
        si.hStdOutput = stdout_w_;
        si.hStdError = stderr_w_;   // server 日志走独立 stderr 管道，读线程丢弃非 JSON 行
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(
            nullptr, cmd.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | (has_env ? CREATE_UNICODE_ENVIRONMENT : 0),
            has_env ? env_block.data() : nullptr,
            cwd.empty() ? nullptr : wide(cwd).c_str(), &si, &pi);
        CloseHandle(stdout_w_);
        CloseHandle(stdin_r_);
        CloseHandle(stderr_w_);
        if (!ok) return false;
        CloseHandle(pi.hThread);
        proc = pi.hProcess;
        return true;
#else
        (void)command; (void)args; (void)env_over; (void)cwd;
        return false;
#endif
    }

    bool write_line(const std::string& line) {
#ifdef _WIN32
        if (stdin_w == nullptr) return false;
        DWORD written = 0;
        return WriteFile(stdin_w, line.data(), static_cast<DWORD>(line.size()),
                         &written, nullptr) && written == line.size();
#else
        (void)line; return false;
#endif
    }

    void kill() {
#ifdef _WIN32
        if (proc != nullptr) TerminateProcess(proc, 1);
#endif
    }

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

    bool request(const std::string& method, const json& params, int timeout_ms, json& out) {
        if (!alive.load()) { diag = "server 未就绪"; return false; }
        const long long id = next_id++;
        std::promise<json> pr;
        auto fut = pr.get_future();
        {
            std::lock_guard lk(mu);
            pending[id] = std::move(pr);
        }
        json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
        if (!params.is_null()) req["params"] = params;
        if (!proc.write_line(req.dump() + "\n")) {
            std::lock_guard lk(mu);
            pending.erase(id);
            diag = "写入 MCP server 失败（进程可能已退出）";
            return false;
        }
        if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
            std::lock_guard lk(mu);
            pending.erase(id);
            diag = "MCP 请求超时（" + method + "，" + std::to_string(timeout_ms) + "ms）";
            return false;
        }
        out = fut.get();
        return true;
    }

    void feed(const std::string& line) {   // 读线程：分发响应
        json msg = json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (msg.is_discarded() || !msg.is_object()) return;   // server 日志/噪音行跳过
        if (!msg.contains("id") || msg.contains("method")) return;   // 通知：MVP 不处理
        if (!msg["id"].is_number_integer()) return;
        std::lock_guard lk(mu);
        const auto it = pending.find(msg["id"].get<long long>());
        if (it != pending.end()) {
            it->second.set_value(std::move(msg));
            pending.erase(it);
        }
    }

    void start_reader() {
        alive.store(true);
        reader = std::thread([this] {
#ifdef _WIN32
            std::string buf;
            char ch[4096] = {};
            DWORD n = 0;
            for (;;) {
                if (!ReadFile(proc.stdout_r, ch, sizeof(ch), &n, nullptr) || n == 0) break;
                buf.append(ch, n);
                std::size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    const std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    if (!line.empty()) feed(line);
                }
            }
#endif
            alive.store(false);
        });
    }

    void start_stderr_drain() {
        stderr_drain = std::thread([this] {
#ifdef _WIN32
            std::string acc;
            char ch[2048] = {};
            DWORD n = 0;
            for (;;) {
                if (!ReadFile(proc.stderr_r, ch, sizeof(ch), &n, nullptr) || n == 0) break;
                if (acc.size() < 400) acc.append(ch, n);
            }
#endif
            std::lock_guard lk(mu);
            if (!acc.empty()) stderr_log = acc;
        });
    }

    void shutdown() {
        alive.store(false);
        proc.kill();
        if (reader.joinable()) reader.join();
        if (stderr_drain.joinable()) stderr_drain.join();
    }
};

// ---------- 插件 ------------------------------------------------------------

class McpPlugin {
public:
    static constexpr std::string_view name = "vc.mcp";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 1, 0);

    nostos_status activate(const nostos::abi::Host& host) {
        host_ = host;
        nostos::abi::init_table(table_);
        table_.self = this;
        table_.list = &McpPlugin::list_thunk;
        table_.execute = &McpPlugin::exec_thunk;
        if (host_.publish(nostos::svc_id_of(kToolSvcName), &table_) != NOSTOS_OK) {
            host_.log(4, "ithaca.mcp: 工具表发布失败");
            return NOSTOS_ERR_FAILED;
        }
        if (host_.acquire_poster(&poster_) != NOSTOS_OK) {
            host_.log(4, "ithaca.mcp: poster 领取失败");
            return NOSTOS_ERR_FAILED;
        }
        // 配置热重载（设置面板保存 → webui 发 vc.mcp.reload）
        subs_.push_back(host_.on(vc_api::mcp_reload, [this](const void*) { reload_async_(); }));
        reload_async_();   // 后台拉起：MCP 是可选能力，绝不阻塞宿主激活与后续插件
        return NOSTOS_OK;
    }

    // 首拉/热重载入口：立即回发当前状态（拉起中），server 全部后台拉起
    void reload_async_() {
        emit_status_();
        std::thread([this] {
            {
                std::lock_guard lk(mu_);
                for (auto& s : sessions_) s->shutdown();
                sessions_.clear();
            }   // 锁只包清理；拉起（含 ≤30s 握手）绝不持锁——否则 discover/list
                // 与 execute 全部被堵死（踩过：自死锁，turn 卡在 user 记录）
            load_config_and_start_();
            emit_status_();
        }).detach();
    }

    void deactivate() noexcept {
        stopping_.store(true);
        std::lock_guard lk(mu_);
        for (auto& s : sessions_) s->shutdown();
        sessions_.clear();
        host_.log(2, "ithaca.mcp: 已停用");
    }

private:
    static constexpr const char* kToolSvcName = "ithaca.tool.mcp";

    // ---- 配置：env ITHACA_MCP_CONFIG 指定独立文件 > webui 数据目录 config.json ----

    fs::path config_path_() const {
        if (const char* v = std::getenv("ITHACA_MCP_CONFIG"); v != nullptr && *v != '\0')
            return fs::path(v);
        const char* home = std::getenv("ITHACA_WEBUI_HOME");
        const fs::path base = (home != nullptr && *home != '\0')
                                  ? fs::path(home)
                                  : fs::path(std::getenv("USERPROFILE") != nullptr
                                                 ? std::getenv("USERPROFILE")
                                                 : ".") /
                                        ".ithaca";
        return base / "config.json";
    }

    void load_config_and_start_() {
        json servers = json::array();
        std::error_code ec;
        const fs::path f = config_path_();
        if (fs::exists(f, ec)) {
            std::ifstream in(f, std::ios::binary);
            const json cfg = json::parse(in, nullptr, /*allow_exceptions=*/false);
            if (cfg.is_object() && cfg.contains("mcp") && cfg["mcp"].is_object() &&
                cfg["mcp"].contains("servers") && cfg["mcp"]["servers"].is_array())
                servers = cfg["mcp"]["servers"];
        }
        for (const auto& s : servers) {
            if (!s.is_object()) continue;
            const std::string name = s.value("name", "");
            const std::string command = s.value("command", "");
            const bool enabled = s.value("enabled", true);
            if (!enabled) continue;
            if (name.empty() || command.empty() ||
                name.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyz"
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
                host_.log(3, "ithaca.mcp: server 配置非法（name 须为 [A-Za-z0-9_-]，"
                             "command 必填），已跳过: " + name);
                continue;
            }
            std::vector<std::string> args;
            if (s.contains("args") && s["args"].is_array())
                for (const auto& a : s["args"])
                    if (a.is_string()) args.push_back(a.get<std::string>());
            start_server_(name, command, args, s.value("env", json::object()),
                          s.value("cwd", ""));
        }
        emit_status_();
    }

    void start_server_(const std::string& name, const std::string& command,
                       const std::vector<std::string>& args, const json& env,
                       const std::string& cwd) {
        auto s = std::make_unique<McpSession>();
        s->name = name;
        if (!s->proc.spawn(command, args, env, cwd)) {
            host_.log(3, "ithaca.mcp: server「" + name + "」拉起失败: " + command);
            return;
        }
        s->start_reader();
        s->start_stderr_drain();

        std::string err;
        json resp;
        json init_params = {
            {"protocolVersion", "2024-11-05"},
            {"capabilities", json::object()},
            {"clientInfo", {{"name", "ithaca-mcp"}, {"version", "0.1.0"}}}};
        if (!s->request("initialize", init_params, kInitTimeoutMs, resp)) {
            host_.log(3, "ithaca.mcp: server「" + name + "」initialize 失败: " + s->diag +
                             " | server stderr=[" + s->stderr_log + "]");
            s->shutdown();
            return;
        }
        s->proc.write_line(json{{"jsonrpc", "2.0"},
                                {"method", "notifications/initialized"}}.dump() + "\n");

        if (!s->request("tools/list", json::object(), kInitTimeoutMs, resp) ||
            !resp.contains("result") || !resp["result"].contains("tools") ||
            !resp["result"]["tools"].is_array()) {
            host_.log(3, "ithaca.mcp: server「" + name + "」tools/list 失败: " + s->diag);
            s->shutdown();
            return;
        }
        s->tools = resp["result"]["tools"];

        {
            std::lock_guard lk(mu_);
            sessions_.push_back(std::move(s));
        }
        emit_status_();
        host_.log(2, "ithaca.mcp: server「" + name + "」就绪（工具 " +
                         std::to_string(find_session_(name)->tools.size()) + " 个）");
    }

    McpSession* find_session_(const std::string& name) {
        for (auto& s : sessions_)
            if (s->name == name && s->alive.load()) return s.get();
        return nullptr;
    }

    std::size_t live_count() const {
        std::size_t n = 0;
        for (const auto& s : sessions_)
            if (s->alive.load()) ++n;
        return n;
    }

    // ---- 状态回发 + 落盘（activate/reload/server 变化后调用；宿主线程独占）-----

    void emit_status_() {
        json arr = json::array();
        for (const auto& s : sessions_) {
            std::vector<std::string> names;
            for (const auto& t : s->tools)
                if (t.is_object() && t.contains("name") && t["name"].is_string())
                    names.push_back(s->name + "__" + t["name"].get<std::string>());
            arr.push_back(json{{"name", s->name},
                               {"alive", s->alive.load()},
                               {"tools", names},
                               {"diag", s->diag}});
        }
        status_buf_ = json{{"ok", true}, {"servers", arr}}.dump();
        if (const char* home = std::getenv("ITHACA_WEBUI_HOME");
            home != nullptr && *home != '\0') {
            std::error_code ec;
            fs::create_directories(home, ec);
            std::ofstream out(fs::path(home) / "mcp-status.json",
                              std::ios::binary | std::ios::trunc);
            if (out) out << status_buf_;
        }
        // 回发经 poster：调用方可能在后台线程（直接 emit 违反线程纪律）
        nostos::kit::poster_submit(poster_, [this] {
            const vc_api::McpStatus e{status_buf_.c_str()};
            host_.emit(vc_api::mcp_status, &e);
        });
    }

    // ---- 统一工具表（契约 vc_tool.hpp：list 自述 + execute 转发）----------------

    static nostos_status list_thunk(void* self, const char** out_json) {
        auto* p = static_cast<McpPlugin*>(self);
        json arr = json::array();
        {
            std::lock_guard lk(p->mu_);
            for (const auto& s : p->sessions_) {
                if (!s->alive.load()) continue;
                for (const auto& t : s->tools) {
                    if (!t.is_object() || !t.contains("name") || !t["name"].is_string())
                        continue;
                    // 全名 mcp.<server>__<tool>：server 段规避跨 server 重名
                    arr.push_back(json{
                        {"name", s->name + "__" + t["name"].get<std::string>()},
                        {"description", t.value("description",
                                                "MCP 工具（server: " + s->name + "）")},
                        {"schema", t.value("inputSchema", json::object())}});
                }
            }
        }
        p->list_buf_ = json{{"ok", true}, {"tools", arr}}.dump();
        *out_json = p->list_buf_.c_str();
        return NOSTOS_OK;
    }

    static nostos_status exec_thunk(void* self, const char* name, const char* args_json,
                                    int /*turn_id——MCP 无快照组语义，忽略*/,
                                    const char** out_json) {
        auto* p = static_cast<McpPlugin*>(self);
        p->exec_buf_ = p->execute(name != nullptr ? name : "",
                                  args_json != nullptr ? args_json : "{}");
        *out_json = p->exec_buf_.c_str();
        return NOSTOS_OK;
    }

    // execute 转发：server__tool 拆分 → tools/call → 结果映射（方案「结果映射」表）
    std::string execute(const std::string& name, const std::string& args_json) {
        const auto us = name.find("__");
        if (us == std::string::npos)
            return json{{"ok", false}, {"error", "MCP 工具名非法: " + name}}.dump();
        McpSession* s = find_session_(name.substr(0, us));
        if (s == nullptr)
            return json{{"ok", false},
                        {"error", "MCP server 未连接: " + name.substr(0, us)}}.dump();
        json args = json::parse(args_json, nullptr, /*allow_exceptions=*/false);
        if (!args.is_object()) args = json::object();

        std::string err;
        json resp;
        if (!s->request("tools/call",
                        {{"name", name.substr(us + 2)}, {"arguments", args}},
                        kCallTimeoutMs, resp)) {
            return json{{"ok", false}, {"error", "MCP 调用失败: " + s->diag}}.dump();
        }
        if (!resp.contains("result") || !resp["result"].is_object())
            return json{{"ok", false}, {"error", "MCP 响应缺少 result"}}.dump();
        const json& r = resp["result"];
        std::string text;
        if (r.contains("content") && r["content"].is_array())
            for (const auto& c : r["content"])
                if (c.is_object() && c.value("type", "") == "text" &&
                    c.contains("text") && c["text"].is_string())
                    text += c["text"].get<std::string>();
        if (r.value("isError", false))
            return json{{"ok", false},
                        {"error", text.empty() ? "MCP 工具执行失败" : text}}.dump();
        return json{{"ok", true}, {"text", text}}.dump();
    }

    nostos::abi::Host host_{};
    nostos_poster poster_{};
    std::atomic<bool> stopping_{false};   // 停用/再重载置位：过期的后台拉起结果丢弃
    vc_api::ToolTable table_{};
    std::string list_buf_, exec_buf_, status_buf_;   // 调用期有效（消费方同步拷贝）
    std::mutex mu_;                     // 保护 sessions_（activate/deactivate/reload 与 agent 线程交叠）
    std::vector<std::unique_ptr<McpSession>> sessions_;
    std::vector<nostos::abi::Host::Subscription> subs_;
};

}  // namespace

NOSTOS_PLUGIN(McpPlugin)
