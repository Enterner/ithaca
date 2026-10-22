// 配置读取与 server 拉起（plugin/mcp 的配置切片）。
// env ITHACA_MCP_CONFIG 指定独立文件 > webui 数据目录 config.json 的
// mcp.servers 数组；逐条校验（name 字符集/command 必填）→ spawn → initialize
// → tools/list 握手（≤30s），成功后进会话表并回发状态。

#include "mcp_plugin.hpp"

#include <cstdlib>  // std::getenv：配置文件/数据目录定位

#include <fstream>  // std::ifstream：config.json 读取

namespace vc_mcp {

// ---- 配置：env ITHACA_MCP_CONFIG 指定独立文件 > webui 数据目录 config.json ----

fs::path McpPlugin::config_path_() const {
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

void McpPlugin::load_config_and_start_() {
    json servers = json::array();
    std::error_code ec;
    const fs::path f = config_path_();
    json cfg = json::object();
    if (fs::exists(f, ec)) {
        std::ifstream in(f, std::ios::binary);
        cfg = json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (!cfg.is_object()) cfg = json::object();
        if (cfg.contains("mcp") && cfg["mcp"].is_object() &&
            cfg["mcp"].contains("servers") && cfg["mcp"]["servers"].is_array())
            servers = cfg["mcp"]["servers"];
    }
    // 插件 UI 贡献的值域：webui 设置面板写 plugins.mcp，此处读出
    // （未配置 = 契约级默认；非法值由 value() 类型守卫兜底）。
    if (cfg.contains("plugins") && cfg["plugins"].is_object() &&
        cfg["plugins"].contains("mcp") && cfg["plugins"]["mcp"].is_object()) {
        const json& vals = cfg["plugins"]["mcp"];
        init_timeout_ms_ = vals.value("init_timeout_ms", kInitTimeoutMs);
        call_timeout_ms_ = vals.value("call_timeout_ms", kCallTimeoutMs);
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
        // NOTE: deactivate 后到达的配置循环到此止步——不再对已清场的插件发起拉起
        if (stopping_.load()) return;
        start_server_(name, command, args, s.value("env", json::object()),
                      s.value("cwd", ""));
    }
    emit_status_();
}

void McpPlugin::start_server_(const std::string& name, const std::string& command,
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
    if (!s->request("initialize", init_params, init_timeout_ms_, resp)) {
        host_.log(3, "ithaca.mcp: server「" + name + "」initialize 失败: " + s->diag +
                         " | server stderr=[" + s->stderr_log + "]");
        s->shutdown();
        return;
    }
    s->proc.write_line(json{{"jsonrpc", "2.0"},
                            {"method", "notifications/initialized"}}.dump() + "\n");

    if (!s->request("tools/list", json::object(), init_timeout_ms_, resp) ||
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
    if (const auto& live = find_session_(name))
        host_.log(2, "ithaca.mcp: server「" + name + "」就绪（工具 " +
                         std::to_string(live->tools.size()) + " 个）");
}

}  // namespace vc_mcp
