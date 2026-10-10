// MCP stdio 传输与 JSON-RPC 会话（plugin/mcp 的协议切片）。
// ChildPipes：子进程 stdio 管道重定向（env 覆盖合并进环境块）；McpSession：
// 进程 + 读线程 + 在途请求——request 同步等 future，feed 分发响应，
// stdout 逐行分帧，stderr 独立线程收集尾部诊断。声明见 include/mcp_plugin.hpp。

#include "mcp_plugin.hpp"

#include <nostos/kit/win.hpp>  // nostos::kit::win::utf8_to_wide：命令行/环境块宽字符化

namespace vc_mcp {

namespace {

std::wstring wide(const std::string& s) {
    return nostos::kit::win::utf8_to_wide(s);
}

}  // namespace

// ---------- 子进程（stdio 管道重定向；MCP stdio 传输载体）--------------------

bool ChildPipes::spawn(const std::string& command, const std::vector<std::string>& args,
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

bool ChildPipes::write_line(const std::string& line) {
#ifdef _WIN32
    if (stdin_w == nullptr) return false;
    DWORD written = 0;
    return WriteFile(stdin_w, line.data(), static_cast<DWORD>(line.size()),
                     &written, nullptr) && written == line.size();
#else
    (void)line; return false;
#endif
}

void ChildPipes::kill() {
#ifdef _WIN32
    if (proc != nullptr) TerminateProcess(proc, 1);
#endif
}

// ---------- 一个 MCP server 会话（进程 + 读线程 + 在途请求）-------------------

bool McpSession::request(const std::string& method, const json& params, int timeout_ms, json& out) {
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

void McpSession::feed(const std::string& line) {   // 读线程：分发响应
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

void McpSession::start_reader() {
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

void McpSession::start_stderr_drain() {
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

void McpSession::shutdown() {
    alive.store(false);
    proc.kill();
    if (reader.joinable()) reader.join();
    if (stderr_drain.joinable()) stderr_drain.join();
}

}  // namespace vc_mcp
