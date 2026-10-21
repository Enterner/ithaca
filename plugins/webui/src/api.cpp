// HTTP API 端点（plugin/webui 的输入/裁决/模型/选目录/文件树切片）。
// /api/input、/api/decision、/api/permission-mode、/api/model、/api/pick-folder、
// /api/fs/list、/api/fs/read、/api/mcp/status。全部跑在 select 线程（必须快）：
// 持久化/广播经 poster defer，不等待排水。

#include "webui_plugin.hpp"

#include "vc_events.hpp"  // vc_api::TurnInput/TurnDecision/PermissionMode + normalize_permission_mode

#include <objbase.h>  // CoInitializeEx/CoUninitialize：选目录线程的 COM 初始化

#include <algorithm>  // std::clamp/std::find：depth 钳制、黑名单过滤

#include <cstdlib>  // std::atoi：query depth 解析

namespace vc_webui {

// ---- 输入与裁决 -------------------------------------------------------------

bool WebuiPlugin::host_running_() const {
    return true;  // 本插件活着 = 宿主活着（HTTP 服务在同进程内）
}

// 发送管线（select 线程共用；/api/input 与 /api/session/new 同走这一条）：
// 落盘用户记录 + 延迟广播/开 turn。NOTE: handle 内禁 broadcast——EDEADLOCK
// （踩过：POST /api/input 即 500）。
void WebuiPlugin::start_turn_(const std::string& text) {
    in_flight_.store(true);
    persist_status_();   // turn 开始（聚合徽标同步）
    json rec = {{"type", "user"}, {"text", text}};
    persist_(rec);   // 只落盘；广播 defer 到宿主线程
    nostos::kit::poster_submit(poster_, [this, text, rec] {
        broadcast_(rec);                       // 先广播用户消息
        const vc_api::TurnInput e{text.c_str()};
        host_.emit(vc_api::turn_input, &e);    // 再开 turn（顺序保证）
    });
}

/// POST /api/input {line} → 发送后续消息：落盘 + 延迟广播 + 开 turn；空输入或
/// 无会话态 400。
http::Response WebuiPlugin::api_input_(const std::string& body) {
    try {
        json in = parse_body_(body);
        std::string line = in.value("line", "");
        if (line.empty()) return json_resp({{"ok", false}, {"error", "空输入"}});
        if (session_.empty())
            return json_resp({{"ok", false},
                              {"error", "无活跃会话——发送首条消息时自动创建"}}, 400);
        start_turn_(line);
        return json_resp({{"ok", true}});
    } catch (const std::exception& e) {
        return json_resp({{"ok", false}, {"error", e.what()}});
    }
}

/// POST /api/decision {decision 0-4, paths?, msg?} → 裁决提案：0 批准所选 /
/// 1 拒绝 / 2 免批 / 3 撤销 / 4 取消；任意裁决终结当前 pending。
http::Response WebuiPlugin::api_decision_(const std::string& body) {
    json in = parse_body_(body);
    const int d = in.value("decision", -1);
    if (d < 0 || d > 4) return json_resp({{"ok", false}, {"error", "decision 越界"}});
    // 逐文件批准：{paths:["a","b"]} ⇒ 只落盘接受的文件；缺省 = 全部
    std::string paths_json;
    if (in.contains("paths") && in["paths"].is_array() && !in["paths"].empty())
        paths_json = in["paths"].dump();   // 裸数组：agent 侧 keep.is_array() 直读
    // 拒绝反馈：{msg:"..."} 随拒绝回灌给 agent（ZCode 式"拒绝并告诉它怎么做"）
    const std::string msg = in.contains("msg") && in["msg"].is_string()
                                ? in["msg"].get<std::string>() : "";
    const int turn = (d == 0 || d == 1) ? pending_turn_ : 0;  // undo/cancel 不看 turn
    // NOTE: 取消（d=4）后 provider 侧不发 done/error（mock 直接静默终止），agent
    // 只发 notice——in_flight_ 无人清零 ⇒ UI 永远卡「生成中」（踩过：停止键卡死）。
    // 用户主动终止 = UI 必须立即解锁。
    if (d == 4) in_flight_.store(false);
    persist_status_();   // 取消 ⇒ 解锁（聚合徽标同步）
    nostos::kit::poster_submit(poster_, [this, turn, d, paths_json, msg] {
        const vc_api::TurnDecision e{turn, d, paths_json.c_str(), msg.c_str()};
        host_.emit(vc_api::turn_decision, &e);
    });
    persist_status_();   // 裁决 ⇒ 提案清零（聚合徽标同步）
    pending_turn_ = -1;   // 任意裁决（含免批/撤销/取消）都终结当前提案——重启后
                          // pending 恒为 false，前端决策坞以后端事实为准（防幻影）
    return json_resp({{"ok", true}});
}

/// POST /api/permission-mode {mode} → 权限模式持久化 + 热切。
http::Response WebuiPlugin::api_permission_mode_(const std::string& body) {
    json in = parse_body_(body);
    const std::string mode = vc_api::normalize_permission_mode(in.value("mode", ""));
    config_["permission_mode"] = mode;
    persist_config_();
    nostos::kit::poster_submit(poster_, [this, mode] {
        const vc_api::PermissionMode e{mode.c_str()};
        host_.emit(vc_api::permission_mode, &e);
    });
    return json_resp({{"ok", true}, {"mode", mode}});
}

/// POST /api/model {model} → 模型热切 + 配置持久化 + 未知模型自动建档；
/// 缺槽/失败不报错，落盘通告重启生效（热切成功完全静默，防通告刷屏）。
http::Response WebuiPlugin::api_model_(const std::string& body) {
    json in = parse_body_(body);
    std::string model = in.value("model", "");
    if (model.empty()) return json_resp({{"ok", false}, {"error", "模型为空"}});
    const auto* llm = llm_;   // activate 时已解析（select 线程不能 service）
    bool live = false;
    std::string diag;
    if (llm == nullptr) diag = "provider 不在（llm 为空）";
    else if (llm->set_model == nullptr) diag = "provider 未实现 set_model";
    else {
        const nostos_status st = llm->set_model(llm->self, model.c_str());
        live = (st == NOSTOS_OK);
        if (!live) diag = "set_model 返回 " + std::to_string(static_cast<int>(st));
    }
    config_["model"] = model;  // 持久化（下次启动同模型）
    // 未知模型 ⇒ 自动建档进「当前使用供应商」的模型列表
    for (auto& p : config_["providers"])
        if (p.is_object() && p.value("name", "") == config_.value("current_provider", "")) {
            bool known = false;
            for (const auto& m : p["models"])
                if (m.is_object() && m.value("id", "") == model) { known = true; break; }
            if (!known)
                p["models"].push_back(json{{"id", model}, {"context", 128000},
                                           {"enabled", true}});
            break;
        }
    persist_config_();
    // 模型热切成功 ⇒ 补发 set_active：窗口随新模型更新（agent 0.9×W 动态压缩
    // 阈值的窗口源，阶段二）。缺槽/失败不补发——模型没真切，窗口不能跟。
    if (live) post_llm_set_active_();
    // 热切成功完全静默（下拉已反映，通告只会刷屏——用户要求"其他操作一律不显示"）；
    // 非实时生效（缺槽/失败）才落盘通告——这是必须可见的状态偏差
    if (!live) {
        json rec = json{{"type", "notice"},
                        {"level", 2},
                        {"text", "模型已保存为 " + model + "（重启后生效；" + diag + "）"}};
        persist_(rec);  // 只落盘；广播 defer 到宿主线程（handle 内禁 broadcast）
        nostos::kit::poster_submit(poster_, [this, rec] { broadcast_(rec); });
    }
    return json_resp({{"ok", true}, {"live", live}});
}

/// POST /api/pick-folder → 原生目录对话框（独立线程弹，防模态阻塞），结果经
/// SSE folder_picked 事件返回。ITHACA_TEST_PICK_PATH 时跳过对话框直接返回。
http::Response WebuiPlugin::api_pick_folder_() {
    // UI 自动化测试钩子：设置 ITHACA_TEST_PICK_PATH ⇒ 跳过原生对话框，沿同一
    // folder_picked 事件路径返回（poster defer——handle 内禁 broadcast）
    if (const char* v = std::getenv("ITHACA_TEST_PICK_PATH"); v != nullptr && *v != '\0') {
        nostos::kit::poster_submit(poster_, [this, p = std::string(v)] {
            broadcast_({{"type", "folder_picked"}, {"path", p}});
        });
        return json_resp({{"ok", true}, {"path", std::string(v)}});
    }
    const std::string initial = config_.value("workspace", "");
    std::thread([this, initial] {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool com = SUCCEEDED(hr);
        const std::string picked =
            com ? pick_folder_dialog("选择工作文件夹", initial)
                : std::string();
        if (com) CoUninitialize();
        if (!picked.empty()) broadcast_({{"type", "folder_picked"}, {"path", picked}});
    }).detach();
    return json_resp({{"ok", true}});
}

/// GET /api/fs/list?path=<相对路径,空=根>&depth=<1..6> → 文件树栏的目录列举。
/// 代理 workspace 的 list 工具（jail 越界防护/上限/symlink 跳过全在提供方），
/// 本端点只做：黑名单目录过滤（树视图不需要的噪音，agent 的 list 保持无偏）、
/// query 的 %XX 解码。NOTE: handle 在 select 线程上且 kit 持内部锁——禁止在
/// 此等待 poster 排水（其他任务体里的 broadcast 会跟内部锁成 ABBA 死锁），
/// 故直调工具表，跨线程安全由 workspace 的全工具串行锁保证。
// query 参数提取 + %XX 解码（encodeURIComponent 形态；'+' 保留——文件名可含加号）。
// /api/fs/list 与 /api/fs/read 共用：同一解码规则写两份必然漂移。
static std::string query_val(const std::string& target, const char* key) {
    const std::size_t q = target.find(std::string(key) + "=");
    if (q == std::string::npos) return "";
    std::size_t e = target.find('&', q);
    if (e == std::string::npos) e = target.size();
    std::string raw = target.substr(q + std::string(key).length() + 1, e - q - std::string(key).length() - 1);
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(raw[i + 1]), lo = hex(raw[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(raw[i]);
    }
    return out;
}

http::Response WebuiPlugin::api_fs_list_(const std::string& target) {
    const std::string rel = query_val(target, "path");
    const std::string depth_s = query_val(target, "depth");
    if (tools_ == nullptr) return json_resp({{"ok", false}, {"error", "workspace 不在"}});
    const json args = json{{"path", rel},
                           {"depth", depth_s.empty() ? 1 : std::clamp(std::atoi(depth_s.c_str()), 1, 6)}};
    const char* out_json = nullptr;
    // 直调工具表（线程安全见上）：NOSTOS_OK + {ok:false} = 工具级失败，原样透传
    const nostos_status st = tools_->execute(tools_->self, "list", args.dump().c_str(), 0,
                                             &out_json);
    json r = st == NOSTOS_OK && out_json != nullptr
                 ? json::parse(out_json, nullptr, false)
                 : json{{"ok", false}, {"error", "workspace execute 失败 (" +
                                              std::to_string(static_cast<int>(st)) + ")"}};
    if (r.is_discarded() || !r.is_object())
        r = json{{"ok", false}, {"error", "workspace 返回了无效 JSON"}};
    // 黑名单：一级目录名精确匹配（树视图噪音；agent 的 list 工具不受影响）
    static const char* kHide[] = {".git",           "node_modules", "build",
                                  "dist",           ".ithaca-data", "__pycache__",
                                  ".vscode",        ".zcode"};
    if (r.value("ok", false) && r.contains("entries") && r["entries"].is_array()) {
        json kept = json::array();
        for (auto& e : r["entries"]) {
            if (!e.is_object()) continue;
            const std::string p = e.value("path", "");
            const std::string seg = p.substr(0, p.find('/'));
            if (std::find(std::begin(kHide), std::end(kHide), seg) == std::end(kHide))
                kept.push_back(std::move(e));
        }
        r["entries"] = std::move(kept);
    }
    return json_resp(std::move(r));
}

/// GET /api/fs/read?path=<相对路径> → 右栏代码查看器的文件内容。
/// 代理 workspace 的 read 工具并原样透传：jail 越界防护、NUL 拒二进制、UTF-8
/// 校验、256KB 封顶（truncated 标记）全在提供方。副作用：无（纯读）。
/// 线程纪律同 api_fs_list_（见上）。
http::Response WebuiPlugin::api_fs_read_(const std::string& target) {
    if (tools_ == nullptr) return json_resp({{"ok", false}, {"error", "workspace 不在"}});
    const json args = json{{"path", query_val(target, "path")}};
    const char* out_json = nullptr;
    const nostos_status st = tools_->execute(tools_->self, "read", args.dump().c_str(), 0,
                                             &out_json);
    json r = st == NOSTOS_OK && out_json != nullptr
                 ? json::parse(out_json, nullptr, false)
                 : json{{"ok", false}, {"error", "workspace execute 失败 (" +
                                              std::to_string(static_cast<int>(st)) + ")"}};
    if (r.is_discarded() || !r.is_object())
        r = json{{"ok", false}, {"error", "workspace 返回了无效 JSON"}};
    return json_resp(std::move(r));
}
/// GET /api/mcp/status → mcp 插件回发的 server 状态（home/mcp-status.json，
// 由 mcp 插件在 activate/reload/server 变化时改写）。文件缺失/损坏 ⇒ 空清单。
http::Response WebuiPlugin::api_mcp_status_() {
    json fallback = {{"ok", true}, {"servers", json::array()}};
    std::error_code ec;
    const fs::path f = home_ / "mcp-status.json";
    if (!fs::exists(f, ec)) return json_resp(fallback);
    std::ifstream in(f, std::ios::binary);
    const json st = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!st.is_object() || !st.contains("servers") || !st["servers"].is_array())
        return json_resp(fallback);
    return json_resp(st);
}

}  // namespace vc_webui
