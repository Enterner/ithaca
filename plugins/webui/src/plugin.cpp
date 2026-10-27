// plugin/webui —— Web 前端插件（正式 UI）。
//
// 本翻译单元：插件生命周期（activate/deactivate）、HTTP 路由 handle（select
// 线程入口）、通用工具（parse_body_/json_resp/mime_of/file_/broadcast_）与
// NOSTOS_PLUGIN 入口。其余端点与后台线程按职责切片，地图见 include/webui_plugin.hpp。
//
// 总职责：kit::http serve SPA + REST + SSE；订阅事件总线（tui 同款事件面）⇒
// JSONL 持久化 + SSE 广播；输入/裁决经 poster 回总线。会话持久化的两层：
//   · transcript.jsonl（本插件写，展示事件流，每条 flush）
//   · state.json（agent 写，LLM 上下文；路径由启动器按活跃会话给定）
//
// 线程纪律：Handler::handle 在 select 线程（必须快——poster_submit/
// 小文件写都非阻塞）；事件回调在宿主线程（drain_posted 串行化）；broadcast
// 内核注明线程安全。锁：无（各线程只触碰自己的数据面）。

#include "webui_plugin.hpp"

#include "vc_events.hpp"  // vc_api::LlmSetActive/TurnInput/PermissionMode/SessionLoad/WorkspaceSet + normalize_permission_mode

#include "../../workspace/iface.hpp"  // vcws::kToolSvc：workspace 工具表的稳定契约名

#include <cstdlib>  // std::getenv/atoi/atol/exit：环境变量、端口解析与关停

#include <fstream>  // std::ofstream：实际端口落盘

namespace vc_webui {

// 内置云端供应商默认模型目录（单一出处）：迁移建档与非数组回种两个入口共用。
static json default_cloud_models() {
    return json::array({
        json{{"id", "deepseek-flash"},
             {"context", vc_api::kDefaultContextWindow}, {"enabled", true}},
        json{{"id", "deepseek-v4-pro"},
             {"context", vc_api::kDefaultContextWindow}, {"enabled", true}},
        json{{"id", "deepseek-reasoner"},
             {"context", vc_api::kDefaultContextWindow}, {"enabled", true}}});
}

nostos_status WebuiPlugin::activate(const nostos::abi::Host& host) {
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
        // 只选不切：会话激活统一走下方 switch_to_——工作区矫正（WorkspaceSet/
        // fs_watcher 迁移）与 transcript 装载都只在那条路径上，此处手工复刻
        // 前半段会漏掉矫正 ⇒ workspace 的 jail 根停在缺省（data 目录），启动即
        // 工作区/会话错位，须切走再切回才对齐（踩过）。后置原因有二：poster 领取
        // 在后（switch_to_ 经 poster defer 广播）；开 transcript 的副作用由
        // switch_to_ 统一做，双份是纯浪费。
        session_ = act;
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
    // workspace 工具表（文件树 /api/fs/* 的后端）：activate 时解析一次；
    // 装载顺序在 webui 之后的情况由 subscribe_ 的 tool_register 兜底重解析
    tools_ = host_.service<vc_api::ToolTable>(vcws::kToolSvc);
    if (tools_ == nullptr)
        host_.log(3, "ithaca.webui: workspace 工具表未就绪——文件树不可用");

    if (host.acquire_poster(&poster_) != NOSTOS_OK) {
        host_.log(4, "ithaca.webui: 领取 poster 失败");
        return NOSTOS_ERR_FAILED;
    }

    // 启动恢复 = 正经的会话切换：工作区随会话矫正 host_workspace_ + WorkspaceSet
    // + transcript 装载全走同一事实路径。广播在 server_ 建立前是 no-op（broadcast_
    // 判空），SSE 客户端连上后由 /api/transcript 重放补齐——不丢事实。
    if (!session_.empty()) switch_to_(session_);

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
        if (cloud_models.empty()) cloud_models = default_cloud_models();
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
            {"models", json::array({json{{"id", "mock-model"},
                                         {"context", vc_api::kDefaultContextWindow},
                                         {"enabled", true}}})}});
        providers.push_back(json{
            {"name", "DeepSeek 官方"}, {"type", "cloud"},
            {"base_url", config_.value("base_url", vc_api::kDefaultCloudBaseUrl)},
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
                {json{{"id", "mock-model"},
                      {"context", vc_api::kDefaultContextWindow}, {"enabled", true}}});
        else
            p["models"] = default_cloud_models();
    }
    // 启动对齐：当前使用供应商的 key/base_url/窗口 热切给 llm 插件（poster 异步）。
    // NOTE: 载荷以 llm_mode_ 为准（mock 模式恒发 mock）——config 的
    // current_provider 可能是另一模式的（共享 config 跨模式留存），对齐成
    // 另一模式会把本模式的插件休眠掉（踩过：mock 宿主启动即被对齐成 cloud
    // 休眠 ⇒ LLM 不可用）
    post_llm_set_active_();
    // 直连/便携启动（桌面壳直发宿主，不经启动器注入 env）：按 config 工作区
    // 热切——仅限 hero 态（无活跃会话）做引导；有活跃会话时工作区已随会话
    // switch_to_ 矫正，此处再发会把会话工作区盖成 config 残留（踩过：启动恢复
    // 会话后 config 旧工作区反向覆盖）。空 = hero 引导（首次 assign 再切）。
    if (session_.empty() && std::getenv("ITHACA_WORKSPACE_DIR") == nullptr &&
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
    // 插件 UI 贡献补拉：先于本插件激活的带 UI 插件已订阅本事件，
    // 收到后重发 vc.ui.register——两个方向合起来对装载顺序不敏感。
    {
        const vc_api::UiQuery q{};
        host_.emit(vc_api::ui_query, &q);
    }

    // HTTP 服务：端口 = env > config > 8790；占用则回退系统分配并广播实际端口
    int port = 8790;
    if (const char* v = std::getenv("ITHACA_WEBUI_PORT"); v != nullptr && *v != '\0')
        port = std::atoi(v);
    else if (config_.contains("port"))
        port = config_.value("port", 8790);
    std::string err;
    http::ServerOptions opts;
    // 默认 2MB 只够对话输入——附件上传走 base64 JSON（约 4/3 膨胀），按 20MB
    // 附件上限抬到 32MB；本机回环服务，请求缓冲随请求分配，非常驻
    opts.max_request = 32ull * 1024 * 1024;
    server_ = std::make_shared<http::Server>(static_cast<std::uint16_t>(port), *this,
                                             opts, &err);
    if (server_ == nullptr || !server_->ok()) {
        err = err.empty() ? "占用" : err;
        host_.log(3, "ithaca.webui: 端口 " + std::to_string(port) + " 失败（" + err +
                         "），回退系统分配");
        server_ = std::make_shared<http::Server>(0, *this, opts, &err);
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

void WebuiPlugin::deactivate() noexcept {
    stop_prober_();
    stop_fs_watcher_();   // 先停 watcher——它还在广播，退订/停服后不得再发
    subs_.clear();  // 先退订——此后不得再 broadcast
    if (server_) server_->stop();
    server_.reset();
    if (out_.is_open()) out_.close();
    host_.log(2, "ithaca.webui: 已停用");
}

// ---- HTTP 路由（select 线程：必须快）----------------------------------------

http::Response WebuiPlugin::handle(const http::Request& req) {
    const std::string& m = req.method;
    // 剥掉 query（"?v=..." 形态的缓存穿透参数）——否则带参请求全部 404
    const std::string t = req.target.substr(0, req.target.find('?'));

    // 静态文件与 /events 零共享态，select 线程同步直答；其余端点一律解析+
    // 投递后挂起，业务体与响应补发全在宿主线程闭包（线程纪律见文件头）。
    if (m == "GET" && (t == "/" || t == "/index.html")) return file_("index.html");
    if (m == "GET" && t.rfind("/web/", 0) == 0) return file_(t.substr(5));
    if (m == "GET" && t == "/events") {
        http::Response r;
        r.sse_stream = true;
        r.content_type = "text/event-stream";
        return r;
    }
    if (m == "GET" && t == "/api/health") {
        return defer_(req, [this](std::uint64_t conn) {
            server_->respond(conn, json_resp({{"ok", true}, {"session", session_}}));
        });
    }
    if (m == "GET" && t == "/api/config") {
        return defer_(req, [this](std::uint64_t conn) {
            server_->respond(conn, json_resp(config_public_()));
        });
    }
    if (m == "POST" && t == "/api/config") return api_config_save_(req);
    if (m == "GET" && t.rfind("/api/transcript", 0) == 0) {
        long after = 0;
        const auto q = req.target.find("after=");   // 从原始 target 解析（t 已剥 query）
        if (q != std::string::npos) after = std::atol(req.target.c_str() + q + 6);
        return defer_(req, [this, conn = req.conn_id, after](std::uint64_t) {
            json arr = json::array();
            for (std::size_t i = static_cast<std::size_t>(after); i < records_.size(); ++i)
                arr.push_back(records_[i]);
            server_->respond(conn, json_resp({{"records", std::move(arr)},
                                              {"next", records_.size()}}));
        });
    }
    if (m == "GET" && t == "/api/session") {
        return defer_(req, [this](std::uint64_t conn) {
            server_->respond(conn, json_resp(session_meta_()));
        });
    }
    if (m == "GET" && t.rfind("/api/agent-log", 0) == 0) {
        return defer_(req, [this, target = req.target](std::uint64_t conn) {
            server_->respond(conn, json_resp(api_agent_log_(target)));
        });
    }
    if (m == "GET" && t == "/api/sessions") {
        return defer_(req, [this](std::uint64_t conn) {
            server_->respond(conn, json_resp(index_.value("sessions", json::array())));
        });
    }
    if (m == "POST" && t == "/api/session/new") return api_session_new_(req);
    if (m == "POST" && t == "/api/session/open") return api_session_open_(req);
    if (m == "POST" && t == "/api/session/delete") return api_session_delete_(req);
    if (m == "POST" && t == "/api/session/rename") return api_session_rename_(req);
    if (m == "POST" && t == "/api/input") return api_input_(req);
    if (m == "POST" && t == "/api/decision") return api_decision_(req);
    if (m == "POST" && t == "/api/upload") return api_upload_(req);
    if (m == "POST" && t == "/api/session/cmd") return api_session_cmd_(req);
    if (m == "POST" && t == "/api/model") return api_model_(req);
    if (m == "POST" && t == "/api/provider/models") return api_provider_models_(req);
    if (m == "GET" && t == "/api/permission-mode") {
        return defer_(req, [this](std::uint64_t conn) {
            server_->respond(conn, json_resp(
                {{"mode", vc_api::normalize_permission_mode(
                              config_.value("permission_mode", "auto-edit"))}}));
        });
    }
    if (m == "POST" && t == "/api/permission-mode") return api_permission_mode_(req);
    // NOTE: 传 req.target（含 query）——上面的 t 已剥 query（静态资源缓存穿透
    // 用），带参 API 从原始 target 自行解析。踩过: 传剥完的 t ⇒ 参数永远解析
    // 不到（transcript 的 after= 曾因此静默失效，靠前端按 id 去重掩住）
    if (m == "GET" && t.rfind("/api/fs/list", 0) == 0) return api_fs_list_(req);
    if (m == "GET" && t.rfind("/api/fs/read", 0) == 0) return api_fs_read_(req);
    if (m == "POST" && t == "/api/pick-folder") return api_pick_folder_(req);
    if (m == "GET" && t == "/api/mcp/status") {
        return defer_(req, [this](std::uint64_t conn) {
            server_->respond(conn, json_resp(api_mcp_status_()));
        });
    }
    if (m == "GET" && t == "/api/context/breakdown") {
        return defer_(req, [this](std::uint64_t conn) {
            server_->respond(conn, json_resp(api_context_breakdown_()));
        });
    }
    if (m == "GET" && t == "/api/ui/registry") {
        return defer_(req, [this](std::uint64_t conn) {
            server_->respond(conn, json_resp(api_ui_registry_()));
        });
    }
    if (m == "GET" && t.rfind("/api/shell/sessions", 0) == 0) {
        return defer_(req, [this](std::uint64_t conn) {
            server_->respond(conn, json_resp(api_shell_sessions_()));
        });
    }
    if (m == "POST" && t == "/api/shutdown") {
        // 不落「宿主停止」通告：每次关窗写一条，重启重放必现纯噪音
        // （踩过：开关 10 次攒 10 条）。宿主线程闭包内退出——与原 select 线程
        // exit 等价（每条记录/每 turn 已落盘，立即退出安全；响应不送达同旧形态）。
        return defer_(req, [](std::uint64_t) { std::exit(0); });
    }
    host_.log(2, "ithaca.webui: 404 " + m + " " + t);   // 诊断：路由未命中点
    return http::Response::status_page(404, "not found");
}

// ---- 通用工具 ---------------------------------------------------------------

json WebuiPlugin::parse_body_(const std::string& body) {
    json d = json::parse(body, nullptr, false);
    return (!d.is_discarded() && d.is_object()) ? d : json::object();
}

http::Response WebuiPlugin::json_resp(json obj, int status) {
    std::string s = obj.dump();
    http::Response r = http::Response::text(status, "application/json; charset=utf-8",
                                            std::move(s));
    return r;
}

std::string WebuiPlugin::mime_of(const fs::path& p) {
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
http::Response WebuiPlugin::file_(const std::string& rel) {
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

// 只广播不落盘（流式增量；完整内容在 LlmDone 时以 assistant 记录收口）。
// NOTE: detach 线禁用本函数（读 server_ 成员与停机 reset 竞态）——用 spawn 前
// 的快照直调 Server::broadcast（停机纪律见 webui_plugin.hpp 文件头）。
void WebuiPlugin::broadcast_(json rec) { if (server_) server_->broadcast(rec.dump()); }

}  // namespace vc_webui

NOSTOS_PLUGIN(vc_webui::WebuiPlugin)
