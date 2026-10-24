// 会话持久化与端点（plugin/webui 的会话切片）。
// transcript.jsonl（事件流追加写，每条 flush——展示重放源）与 index.json
// （注册表，工作区分组，轮次边界落盘）两层；/api/session* 端点族同在本文件。

#include "webui_plugin.hpp"

#include "vc_events.hpp"  // vc_api::SessionLoad/WorkspaceSet：会话切换的上下文加载/工作区热切

#include "../../workspace/text.hpp"  // vcws::trim_utf8：码点安全截断（收敛点）

#include <cstdlib>  // std::atoi：/api/agent-log 的 turn 查询参数

namespace vc_webui {

// ---- 会话记录（落盘 + 广播；id = transcript 行号，页面据此续传）--------------

// 只落盘（select 线程用）。NOTE: handle() 内严禁 broadcast——select 循环持
// kit 内部锁调 handle，再抢同一把锁 = EDEADLOCK（踩过：POST /api/input 即 500）。
// persist_ 是 select/宿主两线程共用的唯一持久化路径（计数/标题更新在内）。
// NOTE: 按**引用**收参——id 写回调用方的 rec，广播/延迟广播因此都带 id，前端
// 按 id 去重；按值收参时广播无 id，SSE 与重放会重复渲染/重复计数。
void WebuiPlugin::persist_(json& rec) {
    rec["id"] = records_.size();
    records_.push_back(rec);
    if (out_.is_open()) {
        out_ << rec.dump() << "\n";
        out_.flush();
    }
    // 更新活跃会话的计数/标题（工作区分组侧栏的数据源）
    if (json* e = session_entry_(session_); e != nullptr) {
        const std::string type = rec.value("type", "");
        if (type == "user") {
            (*e)["turns"] = (*e).value("turns", 0) + 1;
            if ((*e).value("title", "") == "(新会话)" ||
                (*e).value("title", "").empty()) {
                // NOTE: 按字节切 48 会劈开多字节 UTF-8，index_.dump() 必抛 ⇒
                // index.json 永久保存失败——必须走码点边界回退
                std::string t = rec.value("text", "");
                if (t.size() > 48) t = vcws::trim_utf8(t, 48) + "…";
                if (!t.empty()) (*e)["title"] = t;
            }
        }
        if (type == "assistant" && rec.contains("usage") && rec["usage"].is_object())
            (*e)["tokens"] = (*e).value("tokens", 0LL) +
                             rec["usage"].value("total_tokens", 0LL);
        (*e)["updated"] = now_epoch_();
        // NOTE: index.json 只在轮次边界落盘（user=开头 / assistant=收口）——
        // 每条记录都全量重写注册表是纯浪费（几百条转录 = 几十次写盘），元数据
        // 最多滞后半个 turn；改名/删除/新建/切换等路径直接调 save_index_ 不受此限
        if (type == "user" || type == "assistant") save_index_();
    }
}

// 落盘 + 广播（宿主线程用：事件回调里调用，不在 handle 栈内）
void WebuiPlugin::record_(json rec) {
    persist_(rec);
    if (server_) server_->broadcast(rec.dump());
}

void WebuiPlugin::load_transcript_() {
    std::error_code ec;
    const fs::path f = home_ / "sessions" / (session_ + ".transcript.jsonl");
    if (!fs::exists(f, ec)) return;
    std::ifstream in(f, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json rec = json::parse(line, nullptr, false);
        if (rec.is_object() && rec.contains("id") && rec["id"].is_number_unsigned() &&
            rec["id"] == records_.size())  // 尾行损坏/截断 ⇒ 从断点起忽略
            records_.push_back(rec);
    }
}

void WebuiPlugin::open_transcript_() {
    if (session_.empty()) return;   // 无会话态：不建任何 transcript 文件
    std::error_code ec;
    fs::create_directories(home_ / "sessions", ec);
    out_.open(home_ / "sessions" / (session_ + ".transcript.jsonl"),
              std::ios::app | std::ios::binary);
}

// ---- 子代理明细存档（sessions/agents/<会话id>/agent_<turn>.jsonl）------------

// 追加写 + SSE 广播（宿主线程串行化，事件回调专用）。每条 flush 与 transcript
// 同纪律；行号 id 按 turn 单调递增，重启后按现存行数续接——前端据此合并
// 「/api/agent-log 拉取的历史」与「实时增量」不重不漏。
void WebuiPlugin::agent_log_append_(int turn_id, const json& rec) {
    if (session_.empty()) return;
    std::ofstream& out = agent_logs_[turn_id];
    if (!out.is_open()) {
        std::error_code ec;
        const fs::path dir = home_ / "sessions" / "agents" / session_;
        fs::create_directories(dir, ec);
        const fs::path f = dir / ("agent_" + std::to_string(turn_id) + ".jsonl");
        long long seq = 0;
        if (fs::exists(f, ec)) {
            std::ifstream in(f, std::ios::binary);
            std::string line;
            while (std::getline(in, line))
                if (!line.empty()) ++seq;
        }
        agent_log_seq_[turn_id] = seq;
        out.open(f, std::ios::app | std::ios::binary);
    }
    json line = rec;
    line["id"] = ++agent_log_seq_[turn_id];
    if (out.is_open()) {
        out << line.dump() << "\n";
        out.flush();
    }
    // 广播副本必须换类型名：type=subagent 会被前端当主会话记录 push 进主对话流，
    // subagent_detail 才进右栏实时缓冲（踩过：类型未换 ⇒ 右栏永不实时更新，
    // 刷新后明细反而"出现在主会话"）
    json evt = line;
    evt["type"] = "subagent_detail";
    if (server_) server_->broadcast(evt.dump());   // 明细只广播：主 transcript 不落
}

// 会话切换/删除时收口写句柄——句柄绑定旧会话的存档文件，跨会话复用会写串
void WebuiPlugin::close_agent_logs_() {
    for (auto& [turn, out] : agent_logs_)
        if (out.is_open()) out.close();
    agent_logs_.clear();
    agent_log_seq_.clear();
}

/// GET /api/agent-log?turn=N → 子代理工作明细存档（JSONL 解析数组）。右栏过程
/// 视图点开时按需拉取；无会话/无存档返回空数组（老会话的明细仍在主 transcript）。
http::Response WebuiPlugin::api_agent_log_(const std::string& target) {
    int turn = 0;
    if (const auto q = target.find("?turn="); q != std::string::npos)
        turn = std::atoi(target.c_str() + q + 6);
    json records = json::array();
    if (!session_.empty() && turn > 0) {
        std::error_code ec;
        const fs::path f = home_ / "sessions" / "agents" / session_ /
                           ("agent_" + std::to_string(turn) + ".jsonl");
        if (fs::exists(f, ec)) {
            std::ifstream in(f, std::ios::binary);
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                json rec = json::parse(line, nullptr, false);
                if (rec.is_object()) records.push_back(rec);
            }
        }
    }
    return json_resp({{"ok", true}, {"records", records}});
}

// ---- 会话注册表（index.json：工作区分组的元数据源；--------------------

void WebuiPlugin::load_index_() {
    std::error_code ec;
    const fs::path f = home_ / "index.json";
    if (!fs::exists(f, ec)) {
        index_ = json{{"active", session_}, {"sessions", json::array()}};
        return;
    }
    std::ifstream in(f, std::ios::binary);
    json d = json::parse(in, nullptr, false);
    index_ = (d.is_object() && d.contains("sessions") && d["sessions"].is_array())
                 ? d
                 : json{{"active", session_}, {"sessions", json::array()}};
}

void WebuiPlugin::save_index_() {
    std::ofstream out(home_ / "index.json", std::ios::binary | std::ios::trunc);
    if (out) out << index_.dump();
}

json* WebuiPlugin::session_entry_(const std::string& id) {
    for (auto& s : index_["sessions"])
        if (s.is_object() && s.value("id", "") == id) return &s;
    return nullptr;
}

long long WebuiPlugin::now_epoch_() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void WebuiPlugin::ensure_session_(const std::string& id, const std::string& workspace) {
    if (session_entry_(id) != nullptr) return;
    json s = {{"id", id},
              {"title", "(新会话)"},
              {"workspace", workspace},
              {"created", now_epoch_()},
              {"updated", now_epoch_()},
              {"turns", 0},
              {"tokens", 0}};
    index_["sessions"].push_back(s);
    save_index_();
}

// 启动兜底清扫：注册表只允许已初始化会话（turns ≥ 1 = transcript 至少 1 条
// 用户记录）。turns==0 的条目（历史首装种子 / 组合端点中途崩溃的残留）移除，
// 其空 transcript 一并清掉——注册表结构上不可能留未初始化条目。
void WebuiPlugin::sweep_uninitialized_() {
    bool changed = false;
    json arr = json::array();
    for (auto& s : index_["sessions"]) {
        if (s.is_object() && s.value("turns", 0) > 0) { arr.push_back(s); continue; }
        std::error_code ec;
        fs::remove(home_ / "sessions" / (s.value("id", "") + ".transcript.jsonl"), ec);
        fs::remove_all(home_ / "sessions" / "agents" / s.value("id", ""), ec);
        changed = true;
    }
    if (changed) index_["sessions"] = arr;
}

// 会话切换：换 transcript + 通知 agent 加载上下文（vc.session.load）
void WebuiPlugin::switch_to_(const std::string& id) {
    session_ = id;
    index_["active"] = id;
    if (json* e = session_entry_(id); e != nullptr) (*e)["updated"] = now_epoch_();
    save_index_();
    records_.clear();
    close_agent_logs_();   // 旧会话的明细存档句柄收口，防跨会话写串
    load_transcript_();
    if (out_.is_open()) out_.close();
    open_transcript_();
    const fs::path sp = home_ / "sessions" / (id + ".state.json");
    // 工作区随会话：会话记录的 workspace ≠ 当前 ⇒ 热切 jail/runner
    std::string ws = host_workspace_;
    if (json* e = session_entry_(id); e != nullptr)
        ws = e->value("workspace", host_workspace_);
    const bool ws_changed = !ws.empty() && ws != host_workspace_;
    if (ws_changed) host_workspace_ = ws;
    // 广播与 emit 都 defer 到宿主线程（handle 内禁 broadcast——EDEADLOCK）
    nostos::kit::poster_submit(poster_, [this, path = sp.string(), id, ws, ws_changed] {
        broadcast_({{"type", "session"}, {"id", id}, {"workspace", ws}});
        const vc_api::SessionLoad e{path.c_str()};
        host_.emit(vc_api::session_load, &e);
        if (ws_changed) {
            const vc_api::WorkspaceSet w{ws.c_str()};
            host_.emit(vc_api::workspace_set, &w);
            start_fs_watcher_(ws);   // 监听随工作区迁移（同工作区切会话无需重启）
        }
    });
}

/// POST /api/session/new {message 必须, workspace?} → 创建即初始化：注册 + 切换
/// + 首条落盘 + 开 turn 一次完成，返回 {ok, id, workspace}。裸调用 400。
/// 副作用：写注册表/transcript、SSE session 广播、vc.session.load。
http::Response WebuiPlugin::api_session_new_(const std::string& body) {
    if (in_flight_.load())
        return json_resp({{"ok", false}, {"error", "turn 进行中，稍后再试"}});
    json in = parse_body_(body);
    // 创建即初始化（前端草稿态的唯一出口）——必须携带首条消息，裸调用
    // 400 ⇒ 服务端结构上不可能产生未初始化会话（无撤回/清扫逻辑可写）。
    const std::string msg = in.contains("message") && in["message"].is_string()
                                ? in["message"].get<std::string>() : "";
    if (msg.empty())
        return json_resp({{"ok", false},
                          {"error", "缺少 message——会话仅在首条消息发出时创建"}}, 400);
    // 毫秒级 id——同秒内连点不撞车
    const std::string id = "s" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    // NOTE: 新会话的工作区由请求显式指定，不继承宿主当前工作区——继承会让切过
    // 带工作区会话后新建的会话全被带偏（踩过）。语义：主「＋新会话」不带
    // workspace（hero 引导选择）；侧栏组「＋」带 {workspace}（该工作区下新建）。
    std::string ws;
    if (in.contains("workspace") && in["workspace"].is_string() &&
        !in["workspace"].get<std::string>().empty()) {
        ws = in["workspace"].get<std::string>();
        std::error_code ec;
        fs::create_directories(fs::path(ws), ec);
        if (ec) return json_resp({{"ok", false},
                                  {"error", "无法创建工作目录：" + ws + "（" + ec.message() + "）"}});
    }
    ensure_session_(id, ws);   // 此刻注册（turns=0）；首条消息落盘即 turns=1，
                               // 中途崩溃由启动清扫兜底
    switch_to_(id);
    start_turn_(msg);
    return json_resp({{"ok", true}, {"id", id}, {"workspace", ws}});
}

/// POST /api/session/open {id} → 切换活跃会话，返回 {ok, id}；turn 进行中或
/// id 不存在被拒。副作用：SSE session 广播、vc.session.load、工作区随会话热切。
http::Response WebuiPlugin::api_session_open_(const std::string& body) {
    if (in_flight_.load())
        return json_resp({{"ok", false}, {"error", "轮次进行中，稍后再试"}});
    json in = parse_body_(body);
    const std::string id = in.value("id", "");
    if (session_entry_(id) == nullptr)
        return json_resp({{"ok", false}, {"error", "会话不存在"}});
    if (id == session_) return json_resp({{"ok", true}, {"id", id}});
    switch_to_(id);
    return json_resp({{"ok", true}, {"id", id}});
}

/// POST /api/session/rename {id, title} → 改注册表 title 并落盘；活跃/非活跃均可。
http::Response WebuiPlugin::api_session_rename_(const std::string& body) {
    json in = parse_body_(body);
    const std::string id = in.value("id", "");
    const std::string title = in.value("title", "");
    if (id.empty() || title.empty())
        return json_resp({{"ok", false}, {"error", "id 与 title 不能为空"}});
    json* e = session_entry_(id);
    if (e == nullptr) return json_resp({{"ok", false}, {"error", "会话不存在"}});
    (*e)["title"] = title;
    (*e)["updated"] = now_epoch_();
    save_index_();
    return json_resp({{"ok", true}, {"title", title}});
}

/// POST /api/session/delete {id} → 删注册表条目 + transcript + state 并落盘；
/// 删活跃会话则自动切最近更新者，全空进入无会话态。
http::Response WebuiPlugin::api_session_delete_(const std::string& body) {
    json in = parse_body_(body);
    const std::string id = in.value("id", "");
    if (session_entry_(id) == nullptr)
        return json_resp({{"ok", false}, {"error", "会话不存在"}});
    // 活跃会话也可删（用户定义）：删后自动切到最近更新的会话；全空则新建无工作区会话
    json arr = json::array();
    for (auto& s : index_["sessions"])
        if (s.is_object() && s.value("id", "") != id) arr.push_back(s);
    index_["sessions"] = arr;
    save_index_();
    std::error_code ec;
    close_agent_logs_();   // 先收口句柄（可能是活跃会话的存档），再删文件
    fs::remove(home_ / "sessions" / (id + ".transcript.jsonl"), ec);
    fs::remove(home_ / "sessions" / (id + ".state.json"), ec);
    fs::remove_all(home_ / "sessions" / "agents" / id, ec);   // 子代理明细随会话删除
    if (id == session_) {
        std::string next;
        long long best = -1;
        for (auto& s : index_["sessions"]) {
            const long long u = s.value("updated", 0LL);
            if (u > best) { best = u; next = s.value("id", ""); }
        }
        if (!next.empty()) {
            switch_to_(next);
        } else {
            // 全删 ⇒ 无会话态：注册表只含已初始化会话，前端 loadSessions 后进草稿态
            session_ = "";
            index_["active"] = "";
            save_index_();
            records_.clear();
            if (out_.is_open()) out_.close();
        }
    }
    return json_resp({{"ok", true}});
}

// ---- 会话元信息（/api/session 的响应源；多会话/切换经 vc.session.load）----

json WebuiPlugin::session_meta_() {
    // pending：后端当前真有挂起提案（运行时事实，重启即 false）——前端决策坞
    // 以此为门卫，防止任何来源的幻影提案弹审批

    int turns = 0;
    long long tokens = 0;
    std::string title = "(新会话)";
    for (const auto& r : records_) {
        const std::string type = r.value("type", "");
        if (type == "user") {
            ++turns;
            if (title == "(新会话)") {
                std::string t = r.value("text", "");
                if (t.size() > 48) t = vcws::trim_utf8(t, 48) + "…";  // 码点安全
                title = t;
            }
        }
        // 累计 token = assistant 记录自带 usage（恒真形态——events.cpp 组记录时
        // 必带）。这是前端「累计 tokens」显示的服务端源，重放后依然成立。
        if (type == "assistant")
            tokens += r.value("usage", json::object()).value("total_tokens", 0LL);
    }
    std::string ws;
    if (json* e = session_entry_(session_); e != nullptr)
        ws = e->value("workspace", "");
    return {{"id", session_},
            {"title", title},
            {"turns", turns},
            {"tokens", tokens},
            {"workspace", ws},
            {"in_flight", in_flight_.load()},   // 后端原子真值：前端重放后对账防 zombie
            {"records", records_.size()},
            {"pending", pending_turn_ >= 0}};
}

}  // namespace vc_webui
