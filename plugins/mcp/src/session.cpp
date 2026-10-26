// plugin/mcp · stdio 传输与 JSON-RPC 会话。子进程原语 = kit::PipedProcess（三管道、
// Job 树杀、env 块按名合并全在 kit）：本文件只剩 mcp 侧的命令行拼装（引号策略）与
// 会话方法——request 同步等 future，feed 分发响应，stdout 逐行分帧，stderr 独立
// 线程收集尾部诊断。声明见 include/mcp_plugin.hpp。

#include "mcp_plugin.hpp"

#include <nostos/kit/piped_process.hpp>  // kit::LineSplitter：流式行分帧（全仓唯一实现）

namespace vc_mcp {
namespace {

// 命令行拼装：command 恒加引（路径可含空格），参数仅含空白时加引——cmd /c 型命令
// （npx 系 server 的 Windows 标准接法）对全参数加引有「剥首尾引号」改写规则，会把
// 命令行改写坏（踩过：cmd /c npx 链上 npm 前缀解析炸掉）。kit 收完整命令行串，
// 引号责任在调用方——本函数即 mcp 侧的引号策略落点。
std::string compose_command_line(const std::string& command,
                                 const std::vector<std::string>& args) {
    std::string cmd = "\"" + command + "\"";
    for (const auto& a : args) {
        if (a.find_first_of(" \t") != std::string::npos)
            cmd += " \"" + a + "\"";
        else
            cmd += " " + a;
    }
    return cmd;
}

}  // namespace

// ---------- MCP server 会话（拉起 + stdio + 读线程 + 在途请求）-----------------

bool McpSession::spawn(const std::string& command, const std::vector<std::string>& args,
                       const json& env_over, const std::string& cwd) {
    nostos::kit::PipedSpawnOptions o;
    o.cwd_utf8 = cwd;
    o.stdin_pipe = true;   // JSON-RPC 请求通道：父进程主动写子进程 stdin
    for (auto it = env_over.begin(); it != env_over.end(); ++it)
        if (it.value().is_string())
            o.env_overrides_utf8[it.key()] = it.value().get<std::string>();
    std::string err;
    if (!proc.start(compose_command_line(command, args), o, err)) {
        diag = err;
        return false;
    }
    return true;
}

bool McpSession::write_line(const std::string& line) {
    return proc.write_stdin(line.data(), line.size());
}

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
    if (!write_line(req.dump() + "\n")) {
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
        // 行分帧用 kit::LineSplitter（全仓唯一流式切行实现；runner 同款）
        nostos::kit::LineSplitter splitter;
        std::string chunk;
        for (;;) {
            chunk.clear();
            if (!nostos::kit::PipedProcess::read_chunk(proc.stdout_read(), chunk)) break;
            splitter.feed(chunk.data(), chunk.size(), [this](const std::string& line) {
                if (!line.empty()) feed(line);
            });
        }
        splitter.flush([this](const std::string& line) {
            if (!line.empty()) feed(line);
        });
#endif
        alive.store(false);
    });
}

void McpSession::start_stderr_drain() {
    stderr_drain = std::thread([this] {
#ifdef _WIN32
        std::string acc;
        std::string chunk;
        for (;;) {
            chunk.clear();
            if (!nostos::kit::PipedProcess::read_chunk(proc.stderr_read(), chunk)) break;
            if (acc.size() < 400) acc.append(chunk);
        }
#endif
        std::lock_guard lk(mu);
        if (!acc.empty()) stderr_log = acc;
    });
}

void McpSession::shutdown() {
    alive.store(false);
    // Job 整树终止（kit 内建）：cmd /c npx 链的孙进程一并死、管道写端随之关闭，
    // 读线程才能退出——只杀直接子会留孤儿持写端 ⇒ 读线程永挂（热重载死锁，踩过）
    proc.terminate_tree();
    if (reader.joinable()) reader.join();
    if (stderr_drain.joinable()) stderr_drain.join();
    // 句柄不在此收：会话对象经 shared_ptr 存活至在飞 execute 结束，PipedProcess
    // 析构统一收口——与在飞读写不相遇
}

}  // namespace vc_mcp
