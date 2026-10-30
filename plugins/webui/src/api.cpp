// 对话动作端点（plugin/webui 的输入/裁决切片）。
// /api/input、/api/decision、/api/permission-mode、/api/model、/api/session/cmd
// ——全部经 defer_ 投宿主线程后向总线 emit 契约事件（文件树/附件等非动作端点
// 各在其切片文件）。全部跑在 select 线程（必须快）：解析 + 投递 + 挂起，不等待
// 排水。

#include "webui_plugin.hpp"

#include "vc_events.hpp"  // vc_api::TurnInput/TurnDecision/PermissionMode + normalize_permission_mode

namespace vc_webui {

// 发送管线（/api/session/new 从 select 线程调；/api/input 从宿主线程挂起闭包调）：
// 落盘用户记录 + 延迟广播/开 turn。NOTE: 持连接表锁的 handle 栈内禁直接广播/
// 发射——跨线程锁序倒挂即死锁（踩过：POST /api/input 即 500）；广播一律经
// poster defer 到宿主线程。多会话并行：turn 立即发车（归属 = 活跃会话），其余
// 会话的在跑 turn 不受影响；跨工作区并发已被 open/new 门卫拦截。
void WebuiPlugin::start_turn_(const std::string& text, const std::string& attachments_json) {
    const std::string owner = session_;
    json rec = {{"type", "user"}, {"text", text}};
    json att = json::parse(attachments_json.empty() ? "[]" : attachments_json.c_str(),
                           nullptr, /*allow_exceptions=*/false);
    if (att.is_array() && !att.empty()) {
        // 用户记录只带展示元数据（name/path/mime）——data_url 体量大，不进 transcript
        json meta = json::array();
        for (const auto& a : att)
            if (a.is_object())
                meta.push_back({{"name", a.value("name", "")},
                                {"path", a.value("path", "")},
                                {"mime", a.value("mime", "")}});
        if (!meta.empty()) rec["attachments"] = std::move(meta);
    }
    const bool pool_start = running_.empty();
    ++running_[owner];
    persist_status_();   // turn 开始（聚合徽标同步）
    persist_(rec, owner);   // 只落盘；广播 defer 到宿主线程
    nostos::kit::poster_submit(poster_, [this, text, rec, att_json = attachments_json,
                                         owner, pool_start]() mutable {
        reasoning_buf_.erase(owner);   // 轮边界兜底：取消/异常残留的思考不串进新轮
        rec["session"] = owner;
        broadcast_(rec);                       // 先广播用户消息（副本打归属标，落盘副本不带）
        if (pool_start) broadcast_turn_state_(owner);
        const vc_api::TurnInput e{text.c_str(), att_json.c_str(), owner.c_str()};
        host_.emit(vc_api::turn_input, &e);    // 再开 turn（顺序保证）
    });
}

/// POST /api/input {line} → 发送后续消息（挂起-补响应）：select 线程只解析并
/// 投递，读会话态/落盘/广播/开 turn 全在宿主线程闭包内——数据面触碰离开
/// select 线程，闭包与事件回调同线程串行化（机制详见 defer_）。
http::Response WebuiPlugin::api_input_(const http::Request& req) {
    try {
        json in = parse_body_(req.body);
        std::string line = in.value("line", "");
        if (line.empty()) return json_resp({{"ok", false}, {"error", "空输入"}});
        // 附件相对路径数组（多模态）：载荷构建（读图/编码）在宿主线程闭包内做，
        // select 线程只收集路径字符串
        json att_paths = in.contains("attachments") && in["attachments"].is_array()
                             ? in["attachments"] : json::array();
        return defer_(req, [this, line = std::move(line),
                            att_paths = std::move(att_paths)](std::uint64_t conn) {
            if (session_.empty()) {   // 无活跃会话：400 经补发通道回（挂起后唯一出口）
                respond_err_(conn, "无活跃会话——发送首条消息时自动创建", 400);
                return;
            }
            start_turn_(line, attachments_payload_(att_paths).dump());
            server_->respond(conn, json_resp({{"ok", true}}));
        });
    } catch (const std::exception& e) {
        return json_resp({{"ok", false}, {"error", e.what()}});
    }
}

/// POST /api/decision {decision 0-4, paths?, msg?} → 裁决提案：0 批准所选 /
/// 1 拒绝 / 2 免批 / 3 撤销 / 4 取消；任意裁决终结当前 pending。select 线程只
/// 解析校验即挂起，pending 读取与复位在宿主线程闭包内——与 edit_proposed 回调
/// 同线程串行化。
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
    // 裁决目标会话（body.session；缺省 = 活跃会话。body 值在闭包外解析——in 未
    // 捕获进闭包）
    const std::string target_in = in.contains("session") && in["session"].is_string()
                                      ? in["session"].get<std::string>() : "";
    return defer_(req, [this, d, target_in, paths_json = std::move(paths_json),
                        msg](std::uint64_t conn) {
        // 裁决目标会话：决策坞只挂当前浏览会话的提案（其他会话的提案按标路由，
        // 不进当前坞）——undo/cancel 同样按它路由（多会话下「当前 turn」不再全局
        // 唯一）。缺省回落活跃会话（旧前端/裸调用兼容）
        const std::string target = target_in.empty() ? session_ : target_in;
        const int turn = (d == 0 || d == 1)
                             ? (pendings_.count(target) ? pendings_[target] : 0)
                             : 0;  // undo/cancel 不看 turn
        // 任意裁决（含免批/撤销/取消）都终结当前提案——重启后 pending 恒为
        // false，前端决策坞以后端事实为准（防幻影）
        pendings_.erase(target);
        // 取消的 UI 解锁走链收口记账：agent 的 d=4 处理同 drain 内发 llm_error
        //（outstanding 减一 → 归零即链收口）——此处不再手工置位
        persist_status_();   // 全部状态变更落定后一次同步（聚合徽标：运行 + 挂起）
        server_->respond(conn, json_resp({{"ok", true}}));
        // 响应先行、emit 收尾：agent 侧裁决处理可能起进程等重活，不能拖住 UI
        // 的 POST 应答（原同步形态即立即应答+异步 emit，此处保序）。
        const vc_api::TurnDecision e{turn, d, paths_json.c_str(), msg.c_str(),
                                     target.c_str()};
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
            return respond_err_(conn, "供应商不存在或未启用");
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
        persist_(rec, session_);  // 只落盘；广播随响应后内联（已在宿主线程）
        broadcast_(rec);
    }
    server_->respond(conn, json_resp({{"ok", true}, {"live", live}}));
    });
}

/// POST /api/session/cmd {cmd} → 会话级命令转发（契约尾部追加事件 vc.session.cmd；
/// 当前 cmd=1 压缩上下文）。agent 侧按 Idle/主会话门卫受理，结果以 notice 回流。
http::Response WebuiPlugin::api_session_cmd_(const http::Request& req) {
    json in = parse_body_(req.body);
    const int cmd = in.value("cmd", 0);
    return defer_(req, [this, cmd](std::uint64_t conn) {
        // cmd=2（撤回中断回合）：先回退转录面（webui 数据面），agent 随令回退
        // 模型上下文——两侧守卫同款（尾部中断现场），本侧不适格时不发射，避免
        // 双侧各发一条「没有现场」通告
        if (cmd == 2 && !rollback_interrupt_transcript_())
            return respond_err_(conn, "没有可撤回的中断现场（中断后已有新对话）");
        const vc_api::SessionCmd e{cmd, session_.c_str()};
        host_.emit(vc_api::session_cmd, &e);
        server_->respond(conn, json_resp({{"ok", true}}));
    });
}

}  // namespace vc_webui
