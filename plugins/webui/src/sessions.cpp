// 会话持久化与端点（plugin/webui 的会话切片）。
// transcript.jsonl（事件流追加写，每条 flush——展示重放源）与 index.json
// （注册表，工作区分组，轮次边界落盘）两层；/api/session* 端点族与上下文构成
// 分解（读活跃会话 state.json 的分析端点）同在本文件。

#include "webui_plugin.hpp"

#include "vc_events.hpp"  // vc_api::SessionLoad/WorkspaceSet：会话切换的上下文加载/工作区热切

#include "vcws/text.hpp"  // vcws::trim_utf8：码点安全截断（收敛点）

#include <cstdlib>  // std::atoi：/api/agent-log 的 turn 查询参数

namespace vc_webui {

// 首条 user 消息 → 侧栏标题：48 字节护栏 + 码点安全截断。persist_ 的落库与
// session_meta_ 的重放两条路共用一份口径（按字节切会劈开多字节 UTF-8，
// index_.dump() 必抛 ⇒ index.json 永久保存失败）
static std::string clip_title(std::string t) {
    return t.size() > 48 ? vcws::trim_utf8(t, 48) + "…" : t;
}

// ---- 会话记录（落盘 + 广播；id = transcript 行号，页面据此续传）--------------

// 只落盘。owner = 归属会话：活跃会话走内存镜像 + out_ 句柄（records_ 是重放源，
// /api/transcript 服务它）；后台会话直写其 transcript 文件——id 水位独立续号
// （切离运行中会话时按其镜像行数捕获，切回时整卷重放进镜像，id 恰好接续）。
// NOTE: 按**引用**收参——id 写回调用方的 rec，广播/延迟广播因此都带 id，前端按
// id 去重。宿主线程专用（poster 闭包与事件回调串行化，无并发）。
void WebuiPlugin::persist_(json& rec, const std::string& owner) {
    if (owner == session_) {
        rec["id"] = records_.size();
        records_.push_back(rec);
        if (out_.is_open()) {
            out_ << rec.dump() << "\n";
            out_.flush();
        }
    } else {
        // 非活跃归属：懒开会话自己的 transcript 追加写，id 按水位续号（水位在
        // 切离活跃会话时按其镜像行数捕获——切回时整卷重放恰好接续）。崩溃半行
        // 自愈由 open_transcript_ 的常规路径覆盖——sink 只追加不重开
        long long& nid = sink_ids_[owner];
        std::ofstream& out = sinks_[owner];
        rec["id"] = nid++;
        if (!out.is_open()) {
            std::error_code ec;
            fs::create_directories(home_ / "sessions", ec);
            out.open(home_ / "sessions" / (owner + ".transcript.jsonl"),
                     std::ios::app | std::ios::binary);
        }
        if (out.is_open()) {
            out << rec.dump() << "\n";
            out.flush();
        }
    }
    // 更新归属会话的计数/标题（工作区分组侧栏的数据源）
    if (json* e = session_entry_(owner); e != nullptr) {
        const std::string type = rec.value("type", "");
        if (type == "user") {
            (*e)["turns"] = (*e).value("turns", 0) + 1;
            if ((*e).value("title", "") == "(新会话)" ||
                (*e).value("title", "").empty()) {
                const std::string t = clip_title(rec.value("text", ""));
                if (!t.empty()) (*e)["title"] = t;
            }
        }
        if (type == "assistant" && rec.contains("usage") && rec["usage"].is_object())
            (*e)["tokens"] = (*e).value("tokens", 0LL) +
                             rec["usage"].value("total_tokens", 0LL);
        (*e)["updated"] = now_epoch_();
        // NOTE: index.json 只在轮次边界落盘（user=开头 / assistant=收口）——每条
        // 记录都全量重写注册表是纯浪费，元数据最多滞后半个 turn；改名/删除/新建/
        // 切换等路径直接调 save_index_ 不受此限
        if (type == "user" || type == "assistant") save_index_();
    }
}

// 落盘 + 广播（宿主线程用：事件回调里调用，不在 handle 栈内）。广播副本带归属
// 会话标——前端据此过滤非当前会话的 turn 事件（切走不杀 turn：事件仍按归属
// 落盘广播，切回时 HTTP 重放补齐）。落盘副本不带标（transcript 按会话分文件，
// 重放不需要）
void WebuiPlugin::record_(json rec, const char* tag) {
    const std::string owner = ev_owner_(tag);
    persist_(rec, owner);
    if (server_) {
        rec["session"] = owner;
        server_->broadcast(rec.dump());
    }
}

// turn 事件的广播出口（只广播不落盘的流式增量/明细）：同 record_ 打归属标
void WebuiPlugin::broadcast_turn_(json rec, const char* tag) {
    if (server_ == nullptr) return;
    rec["session"] = ev_owner_(tag);
    server_->broadcast(rec.dump());
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
    const fs::path f = home_ / "sessions" / (session_ + ".transcript.jsonl");
    fs::create_directories(home_ / "sessions", ec);
    // 追加前确保文件以换行收尾：崩溃中断的半行残留若不隔离，下一条记录会粘在
    // 半行上（getline 拼成一行、两条一起解析失败）——重启自愈加载跳过了损坏行，
    // 但此后每条新记录在下次加载时全部丢失。补一个换行把损坏封在它自己那行里。
    if (fs::file_size(f, ec) > 0) {
        std::ifstream in(f, std::ios::binary);
        in.seekg(-1, std::ios::end);
        char last = 0;
        if (in.get(last) && last != '\n') {
            std::ofstream out(f, std::ios::app | std::ios::binary);
            out << '\n';
        }
    }
    out_.open(f, std::ios::app | std::ios::binary);
}

// ---- 子代理明细存档（sessions/agents/<会话id>/agent_<turn>.jsonl）------------

// 追加写 + SSE 广播（宿主线程串行化，事件回调专用）。行号 id 按 turn 单调递增，
// 重启后按现存行数续接；每条 flush 与 transcript 同纪律。owner = 父会话（事件
// 尾部 session 标解析）——子代理明细落父会话目录。
void WebuiPlugin::agent_log_append_(const std::string& owner, int turn_id, const json& rec) {
    json line;
    if (owner.empty()) return;
    std::ofstream& out = agent_logs_[turn_id];
    if (!out.is_open()) {
        std::error_code ec;
        const fs::path dir = home_ / "sessions" / "agents" / owner;
        fs::create_directories(dir, ec);
        const fs::path f = dir / ("agent_" + std::to_string(turn_id) + ".jsonl");
        long long seq = 0;
        if (fs::exists(f, ec)) {
            std::ifstream in(f, std::ios::binary);
            std::string ln;
            while (std::getline(in, ln))
                if (!ln.empty()) ++seq;
        }
        agent_log_seq_[turn_id] = seq;
        out.open(f, std::ios::app | std::ios::binary);
    }
    line = rec;
    line["id"] = ++agent_log_seq_[turn_id];
    if (out.is_open()) {
        out << line.dump() << "\n";
        out.flush();
    }
    // 广播副本必须换类型名：type=subagent 会被前端当主会话记录 push 进主对话流，
    // subagent_detail 才进右栏实时缓冲（踩过：类型未换 ⇒ 右栏永不实时更新，
    // 刷新后明细反而"出现在主会话"）。打归属标：后台链的明细不进当前视图
    json evt = line;
    evt["type"] = "subagent_detail";
    evt["session"] = owner;
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
json WebuiPlugin::api_agent_log_(int turn) {
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
    return {{"ok", true}, {"records", records}};
}

// ---- 会话注册表（index.json：工作区分组的元数据源）--------------------

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

// 落盘注册表（宿主线程单线程，调用点自序）。temp + rename 原子替换：trunc 直写
// 中途崩溃 ⇒ index.json 截断，load_index_ 解析失败即静默重置为空注册表（全部
// 会话标题/工作区关联丢失、transcript 变孤儿）——MSVC 的 fs::rename 即
// MoveFileEx(REPLACE_EXISTING)，同卷原子覆盖。
void WebuiPlugin::save_index_() {
    const fs::path f = home_ / "index.json";
    const fs::path tmp = home_ / "index.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;   // 打不开多半是目录不可写：与旧直写同口径静默（读侧容错）
        out << index_.dump();
        out.flush();
    }
    std::error_code ec;
    fs::rename(tmp, f, ec);
    if (ec) fs::remove(tmp, ec);   // 换名失败（目标被占等）：清残件，保底旧文件完好
}

// 返回的指针指向 index_ 内部，调用方当轮使用即弃（单线程下无失效风险）
json* WebuiPlugin::session_entry_(const std::string& id) {
    for (auto& s : index_["sessions"])
        if (s.is_object() && s.value("id", "") == id) return &s;
    return nullptr;
}

// 会话条目的 workspace（未注册 = 空串；返回值即用即弃，单线程无失效风险）
std::string WebuiPlugin::session_ws_(const std::string& id) const {
    for (const auto& s : index_["sessions"])
        if (s.is_object() && s.value("id", "") == id)
            return s.value("workspace", std::string());
    return {};
}

// 运行池工作区一致性（语义见 webui_plugin.hpp 声明处）
bool WebuiPlugin::ws_conflicts_running_(const std::string& ws) const {
    if (ws.empty()) return false;
    for (const auto& [rid, n] : running_)
        if (session_ws_(rid) != ws) return true;
    return false;
}

// 撤回转录面（语义见 webui_plugin.hpp 声明处）。transcript 的 id = 行号的设计
// 让回退就是尾部截断：文件重写为前 cut 行、镜像裁剪、id 水位自然对齐（加载侧的
// 密集校验与尾部自愈都兼容）；被撤轮次的子代理存档随删（句柄先收口）。
// 文件副作用不在回退面——文件树经 fsRev 刷新对齐磁盘真态
bool WebuiPlugin::rollback_interrupt_transcript_() {
    if (records_.empty()) return false;
    // 守卫：尾记录 = 中断卡（尾部中断现场的充分条件；中断后已续聊则尾记录是
    // 后续内容 ⇒ 拒绝，agent 侧同款）
    const json& last = records_.back();
    if (!(last.value("type", "") == "error" &&
          last.value("msg", "") == "当前会话中断"))
        return false;
    std::size_t cut = records_.size();
    for (std::size_t i = records_.size(); i-- > 0;)
        if (records_[i].value("type", "") == "user") { cut = i; break; }
    if (cut >= records_.size()) return false;   // 无 user 记录（不该发生：中断必有输入）
    // 被撤记录携带的子代理轮号：收口明细句柄后删存档文件
    std::set<int> turns;
    for (std::size_t i = cut; i < records_.size(); ++i)
        if (records_[i].contains("turn_id") && records_[i]["turn_id"].is_number_integer())
            turns.insert(records_[i]["turn_id"].get<int>());
    for (int t : turns) {
        if (auto it = agent_logs_.find(t); it != agent_logs_.end()) {
            if (it->second.is_open()) it->second.close();
            agent_logs_.erase(it);
        }
        std::error_code ec;
        fs::remove(home_ / "sessions" / "agents" / session_ /
                   ("agent_" + std::to_string(t) + ".jsonl"), ec);
        agent_log_seq_.erase(t);
    }
    // 会话计数回退（与 persist_ 的递增同口径反向）
    if (json* e = session_entry_(session_); e != nullptr) {
        for (std::size_t i = cut; i < records_.size(); ++i) {
            const std::string type = records_[i].value("type", "");
            if (type == "user")
                (*e)["turns"] = std::max(0, (*e).value("turns", 0) - 1);
            if (type == "assistant" && records_[i].contains("usage") &&
                records_[i]["usage"].is_object())
                (*e)["tokens"] = std::max(
                    0LL, (*e).value("tokens", 0LL) -
                             records_[i]["usage"].value("total_tokens", 0LL));
        }
        (*e)["updated"] = now_epoch_();
    }
    // transcript 重写为前 cut 行（active 会话句柄先收口再重开——尾部自愈逻辑复用）
    if (out_.is_open()) out_.close();
    {
        std::ofstream f(home_ / "sessions" / (session_ + ".transcript.jsonl"),
                        std::ios::binary | std::ios::trunc);
        for (std::size_t i = 0; i < cut; ++i) f << records_[i].dump() << "\n";
        f.flush();
    }
    open_transcript_();
    records_.resize(cut);
    save_index_();
    // 回退信号：前端按位裁剪记录流并对齐游标（session 标 = 当前会话，恒过门卫）
    if (server_ != nullptr)
        server_->broadcast(json{{"type", "records_rollback"},
                                {"session", session_},
                                {"count", cut}}
                               .dump());
    return true;
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
// 用户记录）；turns==0 条目连同空 transcript 一并移除。
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

// 会话切换：换 transcript + 通知 agent 加载上下文（vc.session.load）。
// 切走不杀 turn：运行中链继续跑在原会话（事件经 turn_owner_ 归属路由落盘广播），
// 本函数只换视图面；agent 忙时 session_load 在 agent 侧挂起、链收口自动应用。
void WebuiPlugin::switch_to_(const std::string& id) {
    const fs::path sp = home_ / "sessions" / (id + ".state.json");
    std::string ws;
    bool ws_changed = false;
    // 水位交接：切离活跃会话 ⇒ 捕获其镜像行数作该会话 sink 的 id 水位（后续
    // 后台记录直写文件续号，切回时整卷重放恰好接续）；进入的会话若留有 sink
    //（上一轮后台期开的）随即退役——活跃面归 out_ 句柄
    if (id != session_) sink_ids_[session_] = static_cast<long long>(records_.size());
    if (auto it = sinks_.find(id); it != sinks_.end() && it->second.is_open())
        it->second.close();
    // 同步段：session_/index_/records_/日志句柄的换会话变更是复合操作，按序完成
    session_ = id;
    index_["active"] = id;
    if (json* e = session_entry_(id); e != nullptr) (*e)["updated"] = now_epoch_();
    save_index_();
    records_.clear();
    close_agent_logs_();   // 旧会话的明细存档句柄收口，防跨会话写串
    load_transcript_();
    if (out_.is_open()) out_.close();
    open_transcript_();
    // 工作区随会话：会话记录的 workspace ≠ 当前 ⇒ 热切 jail/runner
    ws = host_workspace_;
    if (json* e = session_entry_(id); e != nullptr)
        ws = e->value("workspace", host_workspace_);
    ws_changed = !ws.empty() && ws != host_workspace_;
    if (ws_changed) host_workspace_ = ws;
    // 广播与 emit 投宿主线程排水（activate 期 server 未建 = 广播 no-op，
    // SSE 客户端连上后由 /api/transcript 重放补齐——不丢事实）
    nostos::kit::poster_submit(poster_, [this, path = sp.string(), id, ws, ws_changed] {
        // 会话边界：旧会话残留的思考不串进新会话收口记录
        reasoning_buf_.erase(id);
        broadcast_({{"type", "session"}, {"id", id}, {"workspace", ws}});
        const vc_api::SessionLoad e{path.c_str()};
        host_.emit(vc_api::session_load, &e);   // 多会话槽：加载即建槽，与在跑 turn 无关
        if (ws_changed) {
            const vc_api::WorkspaceSet w{ws.c_str()};
            host_.emit(vc_api::workspace_set, &w);
            start_fs_watcher_(ws);   // 监听随工作区迁移（同工作区切会话无需重启）
        }
    });
}

// 会话在途 turn 收口：running_ 减一；归零 = 该会话空闲（多会话并行的其余槽不
// 受影响）⇒ 非活跃 sink 句柄收口。返回 true = 恰好归零（调用方发状态帧）
bool WebuiPlugin::chain_settle_(const std::string& owner) {
    const auto it = running_.find(owner);
    if (it == running_.end()) return false;
    if (--it->second > 0) return false;
    running_.erase(it);
    if (owner != session_) {
        if (auto sit = sinks_.find(owner); sit != sinks_.end() && sit->second.is_open())
            sit->second.close();
    }
    return true;
}

// turn_state 状态帧（SSE 瞬态不落盘）：侧栏徽标与前端 inFlight 的实时源——
// 运行状态任何变更后广播一次，前端就地改会话行，不再拉接口
void WebuiPlugin::broadcast_turn_state_(const std::string& sid) {
    if (server_ == nullptr) return;
    server_->broadcast(json{{"type", "turn_state"},
                            {"session", sid},
                            {"working", running_.count(sid) > 0}}
                           .dump());
}

/// POST /api/session/new {message 必须, workspace?} → 创建即初始化：注册 + 切换
/// + 首条落盘 + 开 turn 一次完成，返回 {ok, id, workspace}。裸调用 400。
/// 副作用：写注册表/transcript、SSE session 广播、vc.session.load。
/// select 线程只解析校验；注册表/落盘/切换全在宿主线程闭包后补发响应。
http::Response WebuiPlugin::api_session_new_(const http::Request& req) {
    json in = parse_body_(req.body);
    // 创建即初始化（前端草稿态的唯一出口）——必须携带首条消息，裸调用
    // 400 ⇒ 服务端结构上不可能产生未初始化会话（无撤回/清扫逻辑可写）。
    const std::string msg = in.contains("message") && in["message"].is_string()
                                ? in["message"].get<std::string>() : "";
    if (msg.empty())
        return json_resp({{"ok", false},
                          {"error", "缺少 message——会话仅在首条消息发出时创建"}}, 400);
    // NOTE: 新会话的工作区由请求显式指定，不继承宿主当前工作区——继承会让切过
    // 带工作区会话后新建的会话全被带偏（踩过）。语义：主「＋新会话」不带
    // workspace（hero 引导选择）；侧栏组「＋」带 {workspace}（该工作区下新建）。
    std::string ws;
    if (in.contains("workspace") && in["workspace"].is_string() &&
        !in["workspace"].get<std::string>().empty())
        ws = in["workspace"].get<std::string>();
    // 附件相对路径数组（多模态）：载荷构建（读图/编码）在宿主线程闭包内做——
    // 首条消息的工作区此刻才创建，uploads 内通常无文件，条目按路径注记兜底
    json att_paths = in.contains("attachments") && in["attachments"].is_array()
                         ? in["attachments"] : json::array();
    return defer_(req, [this, msg, ws, att_paths = std::move(att_paths)](std::uint64_t conn) {
        if (ws_conflicts_running_(ws))
            return respond_err_(conn, "有会话正在运行 turn 且工作区不同——"
                                      "先停止它或等完成后再新建");
        // 毫秒级 id——同秒内连点不撞车
        const std::string id = "s" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        std::error_code ec;
        if (!ws.empty()) fs::create_directories(fs::path(ws), ec);
        if (!ws.empty() && ec)
            return respond_err_(conn, "无法创建工作目录：" + ws + "（" + ec.message() + "）");
        ensure_session_(id, ws);   // 此刻注册（turns=0）；首条消息落盘即 turns=1，
                                   // 中途崩溃由启动清扫兜底
        switch_to_(id);
        start_turn_(msg, attachments_payload_(att_paths).dump());
        server_->respond(conn, json_resp({{"ok", true}, {"id", id}, {"workspace", ws}}));
    });
}

/// POST /api/session/open {id} → 切换活跃会话，返回 {ok, id}；id 不存在被拒，
/// 后台链运行中照常切换（turn 继续跑在原会话）。副作用：SSE session 广播、
/// vc.session.load（agent 忙则挂起）、工作区随会话热切（链运行中限同工作区）。
http::Response WebuiPlugin::api_session_open_(const http::Request& req) {
    json in = parse_body_(req.body);
    const std::string id = in.value("id", "");
    return defer_(req, [this, id](std::uint64_t conn) {
        if (session_entry_(id) == nullptr)
            return respond_err_(conn, "会话不存在");
        if (id == session_)
            return server_->respond(conn, json_resp({{"ok", true}, {"id", id}}));
        // 前端已先行确认（停止或等待），此处兜底拒绝跨工作区并行
        if (ws_conflicts_running_(session_ws_(id)))
            return respond_err_(conn, "有会话正在运行 turn 且工作区不同——"
                                      "先停止它或等完成后再切换");
        switch_to_(id);
        server_->respond(conn, json_resp({{"ok", true}, {"id", id}}));
    });
}

/// POST /api/session/rename {id, title} → 改注册表 title 并落盘；活跃/非活跃均可。
http::Response WebuiPlugin::api_session_rename_(const http::Request& req) {
    json in = parse_body_(req.body);
    const std::string id = in.value("id", "");
    const std::string title = in.value("title", "");
    if (id.empty() || title.empty())
        return json_resp({{"ok", false}, {"error", "id 与 title 不能为空"}});
    return defer_(req, [this, id, title](std::uint64_t conn) {
        json* e = session_entry_(id);
        if (e == nullptr)
            return respond_err_(conn, "会话不存在");
        (*e)["title"] = title;
        (*e)["updated"] = now_epoch_();
        save_index_();
        server_->respond(conn, json_resp({{"ok", true}, {"title", title}}));
    });
}

/// POST /api/session/delete {id} → 删注册表条目 + transcript + state 并落盘；
/// 删活跃会话则自动切最近更新者，全空进入无会话态。
http::Response WebuiPlugin::api_session_delete_(const http::Request& req) {
    json in = parse_body_(req.body);
    const std::string id = in.value("id", "");
    return defer_(req, [this, id](std::uint64_t conn) {
        if (session_entry_(id) == nullptr)
            return respond_err_(conn, "会话不存在");
        // 运行中禁删：删后该会话的 transcript 句柄/水位作废，而 turn 仍在跑——
        // 后续事件会写向已删文件。非运行会话不受限
        if (running_.count(id) > 0)
            return respond_err_(conn, "该会话正在运行 turn，请先停止再删除");
        // 活跃会话也可删（用户定义）：删后自动切到最近更新的会话；全空则新建无工作区会话
        json arr = json::array();
        for (auto& s : index_["sessions"])
            if (s.is_object() && s.value("id", "") != id) arr.push_back(s);
        index_["sessions"] = arr;
        save_index_();
        close_agent_logs_();   // 先收口句柄（可能是活跃会话的存档），再删文件
        const bool is_active = id == session_;
        std::error_code ec;
        fs::remove(home_ / "sessions" / (id + ".transcript.jsonl"), ec);
        fs::remove(home_ / "sessions" / (id + ".state.json"), ec);
        fs::remove_all(home_ / "sessions" / "agents" / id, ec);   // 子代理明细随会话删除
        sinks_.erase(id); sink_ids_.erase(id); pendings_.erase(id);   // 归属面随删
        if (is_active) {
            long long best = -1;
            std::string next;
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
        server_->respond(conn, json_resp({{"ok", true}}));
    });
}

// ---- 会话元信息（/api/session 的响应源；多会话/切换经 vc.session.load）----

json WebuiPlugin::session_meta_() {
    // pending：后端当前真有挂起提案（运行时事实，重启即 false）——前端决策坞
    // 以此为门卫，防止任何来源的幻影提案弹审批
    std::string ws, title = "(新会话)";
    int turns = 0;
    long long tokens = 0;
    for (const auto& r : records_) {
        const std::string type = r.value("type", "");
        if (type == "user") {
            ++turns;
            if (title == "(新会话)") title = clip_title(r.value("text", ""));
        }
        // 累计 token = assistant 记录自带 usage（events.cpp 组记录时必带）。
        // NOTE: turns/tokens 在 persist_ 里还有一份同步累加（写 index_ 条目，
        // 侧栏持久值）——两处口径必须同升同降；本函数给活跃会话即时值
        // （records_ 是事实源），index_ 在轮次边界才落盘
        if (type == "assistant")
            tokens += r.value("usage", json::object()).value("total_tokens", 0LL);
    }
    if (json* e = session_entry_(session_); e != nullptr)
        ws = e->value("workspace", "");
    return {{"id", session_},
            {"title", title},
            {"turns", turns},
            {"tokens", tokens},
            {"workspace", ws},
            // 后端真值：前端重放后对账防 zombie。in_flight/pending 均按会话归属
            // 判定（其他会话的 turn 在跑不影响本会话——真并行）
            {"in_flight", running_.count(session_) > 0},
            {"records", records_.size()},
            {"pending", pendings_.count(session_) > 0}};
}

// ---- 上下文构成分解（/api/context/breakdown；读 agent 写的 state.json）-------

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

}  // namespace vc_webui
