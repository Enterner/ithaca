// HTTP API 端点（plugin/webui 的输入/裁决/模型/选目录/文件树切片）。
// /api/input、/api/decision、/api/permission-mode、/api/model、/api/pick-folder、
// /api/fs/list、/api/fs/read、/api/mcp/status。全部跑在 select 线程（必须快）：
// 持久化/广播经 poster defer，不等待排水。

#include "webui_plugin.hpp"

#include "vc_events.hpp"  // vc_api::TurnInput/TurnDecision/PermissionMode + normalize_permission_mode

#include <objbase.h>  // CoInitializeEx/CoUninitialize：选目录线程的 COM 初始化

#include <nostos/kit/win_http.hpp>  // 一发 HTTP 请求小件 + ERROR_WINHTTP_*（供应商模型清单拉取）

#include <algorithm>  // std::clamp/std::find：depth 钳制、黑名单过滤

#include "../../workspace/text.hpp"  // vcws::trim_utf8：错误体截断走码点边界（全仓纪律）

#include <cstdlib>  // std::atoi：query depth 解析

namespace vc_webui {

// 发送管线（/api/session/new 从 select 线程调；/api/input 从宿主线程挂起闭包调）：
// 落盘用户记录 + 延迟广播/开 turn。NOTE: 持连接表锁的 handle 栈内禁直接广播/
// 发射——跨线程锁序倒挂即死锁（踩过：POST /api/input 即 500）；广播一律经
// poster defer 到宿主线程。
void WebuiPlugin::start_turn_(const std::string& text) {
    in_flight_ = true;
    persist_status_();   // turn 开始（聚合徽标同步）
    json rec = {{"type", "user"}, {"text", text}};
    persist_(rec);   // 只落盘；广播 defer 到宿主线程
    nostos::kit::poster_submit(poster_, [this, text, rec] {
        reasoning_buf_.clear();                // 轮边界兜底：取消/异常残留的思考不串进新轮
        broadcast_(rec);                       // 先广播用户消息
        const vc_api::TurnInput e{text.c_str()};
        host_.emit(vc_api::turn_input, &e);    // 再开 turn（顺序保证）
    });
}

/// POST /api/input {line} → 发送后续消息（挂起-补响应试点端点）。select 线程只
/// 解析参数并经 poster 投递闭包即挂起返回——读会话态/落盘/广播/开 turn 全在宿主
/// 线程闭包内完成，响应经 Server::respond 补发。收益：文件写与数据面触碰离开
/// select 线程（handle 持连接表锁期间只剩快进快出），闭包与宿主线程事件回调
/// 同线程串行化，跨线程竞态面收窄。
http::Response WebuiPlugin::api_input_(const http::Request& req) {
    try {
        json in = parse_body_(req.body);
        std::string line = in.value("line", "");
        if (line.empty()) return json_resp({{"ok", false}, {"error", "空输入"}});
        return defer_(req, [this, line = std::move(line)](std::uint64_t conn) {
            if (session_.empty()) {   // 无活跃会话：400 经补发通道回（挂起后唯一出口）
                server_->respond(conn,
                                 json_resp({{"ok", false},
                                            {"error",
                                             "无活跃会话——发送首条消息时自动创建"}},
                                           400));
                return;
            }
            start_turn_(line);
            server_->respond(conn, json_resp({{"ok", true}}));
        });
    } catch (const std::exception& e) {
        return json_resp({{"ok", false}, {"error", e.what()}});
    }
}

/// POST /api/decision {decision 0-4, paths?, msg?} → 裁决提案：0 批准所选 /
/// 1 拒绝 / 2 免批 / 3 撤销 / 4 取消；任意裁决终结当前 pending。select 线程
/// 只解析校验即挂起，pending 读取与复位在宿主线程闭包内完成——与 edit_proposed
/// 回调同线程串行化，原「select 读复位 vs 宿主写」的竞态对已不存在。
http::Response WebuiPlugin::api_decision_(const http::Request& req) {
    json in = parse_body_(req.body);
    const int d = in.value("decision", -1);
    if (d < 0 || d > 4) return json_resp({{"ok", false}, {"error", "decision 越界"}});
    // 逐文件批准：{paths:["a","b"]} ⇒ 只落盘接受的文件；缺省 = 全部
    std::string paths_json;
    if (in.contains("paths") && in["paths"].is_array() && !in["paths"].empty())
        paths_json = in["paths"].dump();   // 裸数组：agent 侧 keep.is_array() 直读
    // 拒绝反馈：{msg:"..."} 随拒绝回灌给 agent（ZCode 式"拒绝并告诉它怎么做"）
    const std::string msg = in.contains("msg") && in["msg"].is_string()
                                ? in["msg"].get<std::string>() : "";
    return defer_(req, [this, d, paths_json = std::move(paths_json),
                        msg](std::uint64_t conn) {
        const int turn = (d == 0 || d == 1) ? pending_turn_ : 0;  // undo/cancel 不看 turn
        // 任意裁决（含免批/撤销/取消）都终结当前提案——重启后 pending 恒为
        // false，前端决策坞以后端事实为准（防幻影）
        pending_turn_ = -1;
        // NOTE: 取消（d=4）后 provider 侧不发 done/error（mock 直接静默终止），agent
        // 只发 notice——in_flight_ 无人清零 ⇒ UI 永远卡「生成中」（踩过：停止键卡死）。
        // 用户主动终止 = UI 必须立即解锁。
        if (d == 4) in_flight_ = false;
        persist_status_();   // 全部状态变更落定后一次同步（聚合徽标：in_flight + pending）
        server_->respond(conn, json_resp({{"ok", true}}));
        // 响应先行、emit 收尾：agent 侧裁决处理可能起进程等重活，不能拖住 UI
        // 的 POST 应答（原同步形态即立即应答+异步 emit，此处保序）。
        const vc_api::TurnDecision e{turn, d, paths_json.c_str(), msg.c_str()};
        host_.emit(vc_api::turn_decision, &e);
    });
}

/// POST /api/permission-mode {mode} → 权限模式持久化 + 热切。
http::Response WebuiPlugin::api_permission_mode_(const http::Request& req) {
    json in = parse_body_(req.body);
    const std::string mode = vc_api::normalize_permission_mode(in.value("mode", ""));
    return defer_(req, [this, mode](std::uint64_t conn) {
        config_["permission_mode"] = mode;
        persist_config_();
        const vc_api::PermissionMode e{mode.c_str()};
        host_.emit(vc_api::permission_mode, &e);
        server_->respond(conn, json_resp({{"ok", true}, {"mode", mode}}));
    });
}

/// POST /api/model {model} → 模型热切 + 配置持久化 + 未知模型自动建档；
/// 缺槽/失败不报错，落盘通告重启生效（热切成功完全静默，防通告刷屏）。
http::Response WebuiPlugin::api_model_(const http::Request& req) {
    json in = parse_body_(req.body);
    std::string model = in.value("model", "");
    if (model.empty()) return json_resp({{"ok", false}, {"error", "模型为空"}});
    return defer_(req, [this, in = std::move(in),
                        model = std::move(model)](std::uint64_t conn) {
    // 跨供应商切换（输入栏模型选择器，body 带 provider）：存在且启用才切——
    // current_provider 一改，下方未知模型建档、persist、post_llm_set_active_
    // （按当前供应商快照凭据/窗口）自然全部落在新供应商上，无需旁路逻辑。
    const std::string provider = in.value("provider", "");
    if (!provider.empty()) {
        bool usable = false;
        for (const auto& p : config_.value("providers", json::array()))
            if (p.is_object() && p.value("name", "") == provider) {
                usable = p.value("enabled", true);
                break;
            }
        if (!usable)
            return server_->respond(conn, json_resp(
                {{"ok", false}, {"error", "供应商不存在或未启用"}}));
        config_["current_provider"] = provider;
    }
    const auto* llm = llm_;   // activate 时已解析（宿主线程闭包内直用）
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
                p["models"].push_back(json{{"id", model},
                                           {"context", vc_api::kDefaultContextWindow},
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
        persist_(rec);  // 只落盘；广播随响应后内联（已在宿主线程）
        broadcast_(rec);
    }
    server_->respond(conn, json_resp({{"ok", true}, {"live", live}}));
    });
}

// ---- 供应商模型清单拉取（OpenAI 兼容 GET /models）---------------------------

// WinHTTP 失败 → 人话（与 llm_cloud 的 last_error_text 同口径；复制而非共享——
// 两插件无公共 TU）。错误码显式传入：调用点已离开失败现场，GetLastError 不可再读。
static std::string winhttp_error_text_(unsigned long gle) {
    switch (gle) {
        case ERROR_WINHTTP_TIMEOUT: return "超时";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "域名解析失败";
        case ERROR_WINHTTP_CANNOT_CONNECT: return "无法连接（断网，或本地代理未启动）";
        case ERROR_WINHTTP_CONNECTION_ERROR: return "与服务器的连接被重置";
        default: return "WinHTTP 错误 " + std::to_string(gle);
    }
}

/// POST /api/provider/models {name, base_url, api_key?} → 请求平台的 OpenAI 兼容
/// GET /models，把模型 ID 清单经 SSE provider_models 事件回设置面（自动填充模型
/// 表）。select 线程只做参数解析；key 定位在宿主线程闭包（api_key 非空 = 刚粘贴
/// 未保存；留空 = 按 name 取后端已存——前端掩码拿不到明文，明文只在 config_），
/// 网络在 detach 线程：外网往返秒级，不能占住宿主排水（事件回调全在它上面排队）。
/// 瞬态事件不落 transcript（folder_picked 同款）。
http::Response WebuiPlugin::api_provider_models_(const http::Request& req) {
    json in = parse_body_(req.body);
    const std::string name = in.value("name", "");
    std::string base = in.value("base_url", "");
    std::string key = in.value("api_key", "");
    if (base.empty()) return json_resp({{"ok", false}, {"error", "Base URL 为空"}});
    return defer_(req, [this, name, base = std::move(base),
                        key = std::move(key)](std::uint64_t conn) mutable {
        if (key.empty())
            for (const auto& p : config_.value("providers", json::array()))
                if (p.is_object() && p.value("name", "") == name) {
                    key = p.value("api_key", "");
                    break;
                }
        if (key.empty())
            return server_->respond(conn, json_resp(
                {{"ok", false},
                 {"error", "API Key 为空——先粘贴 key，或该供应商已保存过 key"}}));
        // 停机纪律（见 webui_plugin.hpp 文件头）：detach 线对插件零触碰——spawn
        // 前在宿主线程快照 server_ 值捕获，广播直调快照；deactivate 的 reset 不
        // 与本线程竞态（快照保活，stop 后 broadcast 为安全 no-op）
        std::shared_ptr<http::Server> srv = server_;
        std::thread([srv, base = std::move(base), key = std::move(key)] {
        // base 尾斜杠归一后拼 /models；llm_cloud 同款四段解析口径
        std::string url = base;
        while (!url.empty() && url.back() == '/') url.pop_back();
        url += "/models";
        const std::wstring wurl = win::utf8_to_wide(url);
        const std::wstring wkey = win::utf8_to_wide(key);
        json result = {{"ok", false}, {"error", std::string("请求未发起")}};
        const win::HttpReply reply = win::http_fetch(wurl, {
            .headers = {L"Authorization: Bearer " + wkey},
            // 短超时：清单拉取是设置面动作，用户在等，失败要快回
            .timeouts = {5000, 5000, 10000, 30000},
        });
        if (!reply.sent || reply.status == 0) {
            result["error"] = winhttp_error_text_(reply.gle);
        } else {
            const json doc = json::parse(reply.body, nullptr,
                                         /*allow_exceptions=*/false);
            if (reply.status == 200 && doc.is_object() && doc.contains("data") &&
                doc["data"].is_array()) {
                json ids = json::array();
                for (const auto& m : doc["data"])
                    if (m.is_object() && m.contains("id") && m["id"].is_string())
                        ids.push_back(m["id"].get<std::string>());
                result = {{"ok", true}, {"models", std::move(ids)}};
            } else {
                // 错误消息人话化：平台 OpenAI 形态 error.message 优先，
                // 原文兜底（截 200 字节防超长错误体刷屏）
                std::string msg;
                if (doc.is_object()) {
                    if (doc.contains("error") && doc["error"].is_object())
                        msg = doc["error"].value("message", "");
                    else if (doc.contains("message") && doc["message"].is_string())
                        msg = doc["message"].get<std::string>();
                }
                if (msg.empty()) msg = vcws::trim_utf8(reply.body, 200);   // 按字节切会劈开 UTF-8 ⇒ dump 抛 316
                result = {{"ok", false},
                          {"error", "HTTP " + std::to_string(reply.status) +
                                        (msg.empty() ? "" : "：" + msg)}};
            }
        }
        json evt = {{"type", "provider_models"}};
        evt.merge_patch(result);
        if (srv) srv->broadcast(evt.dump());   // kit 层线程安全；已停实例 = no-op
    }).detach();
        server_->respond(conn, json_resp({{"ok", true}, {"pending", true}}));
    });
}

// UTF-8 字符数折算 token 的粗估：ASCII ≈ 4 字符/token，非 ASCII（CJK 为主）
// ≈ 0.7 token/字。仅用于构成占比——卡片上明确标「分段估算」；总量以 LLM 返回
// 的实测 last_prompt_tokens 为准（分段按占比等比修正，合计不失真）。
static size_t utf8_tokens(const std::string& s) {
    size_t ascii = 0, non = 0;
    for (const unsigned char b : s) {
        if ((b & 0xC0) == 0x80) continue;   // UTF-8 后续字节
        if (b < 0x80) ++ascii; else ++non;
    }
    return ascii / 4 + static_cast<size_t>(non * 0.7);
}

/// GET /api/context/breakdown → 上下文构成分解（悬停指示器的数据源）。
/// 读活跃会话 state.json（agent 写的 LLM 上下文），messages 按 role 分四桶：
/// 系统提示(system) / 用户消息(user) / 模型回复(assistant 正文) /
/// 工具调用(tool_calls 参数 + tool 结果——用户口径合并为一段)。
/// 分段为估算，实测总量存在时（last_prompt_tokens>0）按各桶占比等比修正使
/// 合计=实测；舍入漂移记入最大的工具桶。mtime 缓存：同一版本文件只解析一次。
/// 数据源：路由 defer 包装（宿主线程闭包内调用）。
json WebuiPlugin::api_context_breakdown_() {
    if (session_.empty())
        return {{"ok", false}, {"error", "无活跃会话"}};
    const fs::path f = home_ / "sessions" / (session_ + ".state.json");
    std::error_code ec;
    if (!fs::exists(f, ec))
        return {{"ok", false}, {"error", "会话文件不存在"}};
    const auto mtime = fs::last_write_time(f, ec);
    const std::string key = f.string() + "@" +
        std::to_string(mtime.time_since_epoch().count());
    if (bd_key_ == key && !bd_cache_.is_null()) return bd_cache_;
    std::ifstream in(f, std::ios::binary);
    const json st = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!st.is_object())
        return {{"ok", false}, {"error", "会话文件解析失败"}};
    const auto toklen = [](const json& c) -> size_t {
        if (c.is_string()) return utf8_tokens(c.get<std::string>());
        if (c.is_null()) return 0;
        return utf8_tokens(c.dump());   // 结构化内容（分段文本等）整体折算
    };
    size_t sys = 0, user = 0, asst = 0, call = 0, tool = 0;
    size_t n_sys = 0, n_user = 0, n_asst = 0, n_calls = 0, n_tool = 0;
    if (st.contains("messages") && st["messages"].is_array())
        for (const auto& m : st["messages"]) {
            if (!m.is_object()) continue;
            const std::string role = m.value("role", "");
            if (role == "system") { sys += toklen(m.value("content", json())); ++n_sys; }
            else if (role == "user") { user += toklen(m.value("content", json())); ++n_user; }
            else if (role == "assistant") {
                asst += toklen(m.contains("content") ? m["content"] : json());
                ++n_asst;
                if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                    call += utf8_tokens(m["tool_calls"].dump());
                    n_calls += m["tool_calls"].size();
                }
            } else if (role == "tool") {
                tool += toklen(m.value("content", json()));
                ++n_tool;
            }
        }
    const size_t sum = sys + user + asst + call + tool;
    const long long measured_ll = st.value("last_prompt_tokens", 0LL);
    const bool measured = measured_ll > 0 && sum > 0;
    const auto scale = [&](size_t v) -> long long {
        if (!measured) return static_cast<long long>(v);
        return static_cast<long long>(
            static_cast<double>(v) * static_cast<double>(measured_ll) / sum + 0.5);
    };
    long long s_sys = scale(sys), s_user = scale(user), s_asst = scale(asst), s_call = scale(call);
    // 舍入漂移记入工具桶（四段里通常最大，漂移占比最小）
    long long s_tool = measured ? measured_ll - (s_sys + s_user + s_asst + s_call)
                                : static_cast<long long>(tool);
    if (s_tool < 0) s_tool = 0;
    const long long total = measured ? measured_ll : static_cast<long long>(sum);
    // 输出四段（用户口径）：工具调用 = tool_calls 参数 + tool 结果合并
    json out = {{"ok", true}, {"total", total}, {"measured", measured},
                {"segs", json{{"sys", s_sys}, {"user", s_user},
                              {"asst", s_asst}, {"tool", s_call + s_tool}}},
                {"counts", json{{"sys", n_sys}, {"user", n_user}, {"asst", n_asst},
                                {"calls", n_calls}, {"tools", n_tool}}}};
    bd_key_ = key;
    bd_cache_ = out;
    return std::move(out);
}

/// POST /api/pick-folder → 原生目录对话框（独立线程弹，防模态阻塞），结果经
/// SSE folder_picked 事件返回。ITHACA_TEST_PICK_PATH 时跳过对话框直接返回
/// （零共享态，同步直答）。
http::Response WebuiPlugin::api_pick_folder_(const http::Request& req) {
    // UI 自动化测试钩子：设置 ITHACA_TEST_PICK_PATH ⇒ 跳过原生对话框，沿同一
    // folder_picked 事件路径返回（poster defer 广播）
    if (const char* v = std::getenv("ITHACA_TEST_PICK_PATH"); v != nullptr && *v != '\0') {
        nostos::kit::poster_submit(poster_, [this, p = std::string(v)] {
            broadcast_({{"type", "folder_picked"}, {"path", p}});
        });
        return json_resp({{"ok", true}, {"path", std::string(v)}});
    }
    return defer_(req, [this, initial = config_.value("workspace", "")](
                           std::uint64_t conn) mutable {
        // 停机纪律同 api_provider_models_：快照 server_ 值捕获，零触碰插件状态
        std::shared_ptr<http::Server> srv = server_;
        std::thread([srv, initial = std::move(initial)] {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool com = SUCCEEDED(hr);
        const std::string picked =
            com ? pick_folder_dialog("选择工作文件夹", initial)
                : std::string();
        if (com) CoUninitialize();
        if (!picked.empty() && srv)
            srv->broadcast(json{{"type", "folder_picked"}, {"path", picked}}.dump());
    }).detach();
        server_->respond(conn, json_resp({{"ok", true}}));
    });
}

/// GET /api/fs/list?path=<相对路径,空=根>&depth=<1..6> → 文件树栏的目录列举。
/// 代理 workspace 的 list 工具（jail 越界防护/上限/symlink 跳过全在提供方），
/// 本端点只做：黑名单目录过滤（树视图不需要的噪音，agent 的 list 保持无偏）、
/// query 的 %XX 解码。直调工具表——跨线程安全由 workspace 的全工具串行锁保证
/// （调用在宿主线程闭包内）。
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

http::Response WebuiPlugin::api_fs_list_(const http::Request& req) {
    const std::string rel = query_val(req.target, "path");
    const std::string depth_s = query_val(req.target, "depth");
    const json args = json{{"path", rel},
                           {"depth", depth_s.empty() ? 1 : std::clamp(std::atoi(depth_s.c_str()), 1, 6)},
                           {"pattern", query_val(req.target, "pattern")},
                           {"mode", query_val(req.target, "mode")},
                           {"per_dir", std::atoi(query_val(req.target, "per_dir").c_str())}};
    return defer_(req, [this, args = std::move(args)](std::uint64_t conn) {
        json r = call_tool_json_("list", args.dump());
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
        server_->respond(conn, json_resp(std::move(r)));
    });
}

/// GET /api/fs/read?path=<相对路径> → 右栏代码查看器的文件内容。
/// 代理 workspace 的 read 工具并原样透传：jail 越界防护、NUL 拒二进制、UTF-8
/// 校验、256KB 封顶（truncated 标记）全在提供方。副作用：无（纯读）。
/// 线程纪律同 api_fs_list_（见上）。
http::Response WebuiPlugin::api_fs_read_(const http::Request& req) {
    const json args = json{{"path", query_val(req.target, "path")}};
    return defer_(req, [this, args = std::move(args)](std::uint64_t conn) {
        server_->respond(conn, json_resp(call_tool_json_("read", args.dump())));
    });
}

// workspace 工具表直调（/api/fs/* 代理共用）：判空 → execute → NOSTOS_OK 判定 →
// JSON parse → 无效兜底。返回值恒为 object；调用须在宿主线程闭包内（tools_ 读写
// 纪律见 webui_plugin.hpp）
json WebuiPlugin::call_tool_json_(const char* tool, const std::string& args_json) {
    const vc_api::ToolTable* tools = tools_;
    if (tools == nullptr)
        return {{"ok", false}, {"error", "workspace 不在"}};
    const char* out_json = nullptr;
    // NOSTOS_OK + {ok:false} = 工具级失败，原样透传给前端
    const nostos_status st = tools->execute(tools->self, tool, args_json.c_str(), 0, &out_json);
    json r = st == NOSTOS_OK && out_json != nullptr
                 ? json::parse(out_json, nullptr, false)
                 : json{{"ok", false}, {"error", "workspace execute 失败 (" +
                                              std::to_string(static_cast<int>(st)) + ")"}};
    if (r.is_discarded() || !r.is_object())
        r = json{{"ok", false}, {"error", "workspace 返回了无效 JSON"}};
    return r;
}
/// GET /api/mcp/status → mcp 插件回发的 server 状态（home/mcp-status.json，
// 由 mcp 插件在 activate/reload/server 变化时改写）。文件缺失/损坏 ⇒ 空清单。
// 数据源：路由 defer 包装（宿主线程闭包内调用）。
json WebuiPlugin::api_mcp_status_() {
    json fallback = {{"ok", true}, {"servers", json::array()}};
    std::error_code ec;
    const fs::path f = home_ / "mcp-status.json";
    if (!fs::exists(f, ec)) return fallback;
    std::ifstream in(f, std::ios::binary);
    const json st = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!st.is_object() || !st.contains("servers") || !st["servers"].is_array())
        return fallback;
    return st;
}

}  // namespace vc_webui
