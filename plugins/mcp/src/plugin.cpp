// plugin/mcp —— MCP 客户端插件：通用 MCP 工具接入。
//
// 本翻译单元：插件生命周期（activate/reload_async_/deactivate）与状态可见性
// （emit_status_：vc.mcp.status 回发 + mcp-status.json 落盘）+ NOSTOS_PLUGIN
// 入口。总职责/范围/线程纪律与各切片职责地图见 include/mcp_plugin.hpp。

#include "mcp_plugin.hpp"

#include "vc_events.hpp"  // vc_api::McpReload/McpStatus：热重载订阅 + 状态回发事件

#include <nostos/svc_id.hpp>  // nostos::svc_id_of：工具表服务名 → svc id

#include <cstdlib>  // std::getenv：数据目录定位

#include <fstream>  // std::ofstream：mcp-status.json 落盘

namespace vc_mcp {

nostos_status McpPlugin::activate(const nostos::abi::Host& host) {
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
    // 插件 UI 贡献：activate 时声明设置 schema；webui 后启动的场景由
    // vc.ui.query 补拉（收到即重发）。载荷为静态串，调用期有效纪律天然满足。
    subs_.push_back(host_.on(vc_api::ui_query, [this](const void*) { emit_ui_register_(); }));
    emit_ui_register_();
    reload_async_();   // 后台拉起：MCP 是可选能力，绝不阻塞宿主激活与后续插件
    return NOSTOS_OK;
}

void McpPlugin::reload_async_() {
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

void McpPlugin::deactivate() noexcept {
    stopping_.store(true);
    std::lock_guard lk(mu_);
    for (auto& s : sessions_) s->shutdown();
    sessions_.clear();
    host_.log(2, "ithaca.mcp: 已停用");
}

// 插件 UI 贡献：本插件设置 schema——握手/调用超时（ms）。webui 据此
// 动态生成设置节，值存 config.json 的 plugins.mcp（本插件启动/reload 时读取）。
void McpPlugin::emit_ui_register_() {
    static constexpr const char* kSchema =
        R"([{"key":"init_timeout_ms","label":"握手超时（毫秒）","type":"number","default":30000},)"
        R"({"key":"call_timeout_ms","label":"工具调用超时（毫秒）","type":"number","default":120000}])";
    const vc_api::UiRegister e{"mcp", "MCP 超时", kSchema};
    host_.emit(vc_api::ui_register, &e);
}

// ---- 状态回发 + 落盘（activate/reload/server 变化后调用；宿主线程独占）-----

void McpPlugin::emit_status_() {
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

}  // namespace vc_mcp

NOSTOS_PLUGIN(vc_mcp::McpPlugin)
