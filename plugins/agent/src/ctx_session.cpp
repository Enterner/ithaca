// 会话持久化与 mission 卡（plugin/agent 的 M6 session 切片）。
// state.json：历史 messages + turn 计数 + token 用量（activate 恢复与 webui
// 会话切换共用加载路径；每 turn 收口/压缩后落盘，失败仅告警）；
// mission 任务目标卡：turn 边界原位重写，turn 内 append-only（缓存前缀纪律）。

#include "agent_plugin.hpp"

#include <algorithm>  // std::max：turns 记全局计数器高水位
#include <cerrno>     // errno：落盘失败的系统层原因
#include <chrono>     // std::chrono：落盘瞬时锁重试等待
#include <fstream>    // std::ifstream/ofstream：state.json 读写
#include <thread>     // std::this_thread：300ms 重试间隔

namespace vc_agent {

// state 文件加载（activate 与会话切换共用；缺文件 = 全新会话）
void AgentCtx::load_session_state_(const std::string& path) {
    session_path_ = path;
    session_id_ = vc_agent::session_id_of_state(path);
    // raw 外置目录随会话走（与会话切换同源）：多会话共享宿主时归档名会跨会话
    // 撞车——归档名加会话 stem 前缀隔离（同一 parent/raw 内 ref 仍为裸文件名）
    auto [dir, prefix] = vcagent::raw_spool_paths(path);
    raw_dir_ = std::move(dir);
    raw_prefix_ = std::move(prefix);
    std::error_code ec;
    fs::create_directories(raw_dir_, ec);
    std::ifstream in(session_path_, std::ios::binary);
    json doc = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_discarded() && doc.is_object()) {
        // turns/last_prompt_tokens 与 usage 同款类型守卫：磁盘文件类型漂移
        //（手编/外部改写）时 value() 抛 302 ⇒ activate 期线程 terminate
        if (const auto it = doc.find("turns");
            it != doc.end() && it->is_number_integer()) {
            const long long turns = it->get<long long>();
            turn_no_ = adopt_turn_floor(turns > INT32_MAX ? INT32_MAX
                                                          : static_cast<int>(turns));
        }
        const json usage = doc.value("usage", json::object());
        if (usage.is_object()) {
        usage_prompt_ = usage.value("prompt_tokens", 0LL);
        usage_completion_ = usage.value("completion_tokens", 0LL);
        usage_total_ = usage.value("total_tokens", 0LL);
        usage_cache_ = usage.value("cache_tokens", 0LL);   // 缓存命中计量随会话恢复
        }
        // 判定信号随会话恢复：恢复后首轮压缩判定不再失明
        if (const auto it = doc.find("last_prompt_tokens");
            it != doc.end() && it->is_number_integer())
            last_prompt_tokens_ = it->get<long long>();
        if (doc.contains("messages") && doc["messages"].is_array())
            messages_ = doc["messages"];
        host_.log(2, "ithaca.agent: 会话已加载（turns=" + std::to_string(turn_no_) +
                         ", messages=" + std::to_string(messages_.size()) + "）");
    } else {
        messages_ = json::array();
        turn_no_ = 0;
        usage_prompt_ = usage_completion_ = usage_total_ = 0;
        host_.log(2, "ithaca.agent: 全新会话（" + path + "）");
    }
}

void AgentCtx::persist_session_() {
    if (session_path_.empty()) return;
    try {
        json usage = {{"prompt_tokens", usage_prompt_},
                      {"completion_tokens", usage_completion_},
                      {"total_tokens", usage_total_},
                      {"cache_tokens", usage_cache_}};
        // turns = 全局 turn 计数器高水位（含子代理消耗的号段），而非主会话轮号
        // ——恢复地板据此抬升；只记主轮号会让重启后的新子代理复用历史 transcript
        // 里的 turn_id，UI 分组新旧混组（踩过）
        json doc = {{"turns", std::max(turn_no_, turn_seq().load())},
                    {"usage", usage},
                    {"last_prompt_tokens", last_prompt_tokens_},
                    {"messages", messages_}};
        std::error_code ec;
        fs::create_directories(fs::path(session_path_).parent_path(), ec);
        // temp + rename 原子替换：trunc 直写中途崩溃 ⇒ state.json 截断，下次
        // load 解析失败即整个会话历史静默归零（load 的"全新会话"兜底分不清
        // 「没有历史」与「历史损坏」）
        const fs::path sp(session_path_);
        const fs::path tmp = sp.string() + ".tmp";
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            // 杀软/索引器/残留宿主造成的瞬时锁：等 300ms 重试一次
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            out.open(tmp, std::ios::binary | std::ios::trunc);
        }
        if (!out) {
            host_.log(3, "ithaca.agent: 会话落盘失败（无法打开 " + tmp.string() +
                             "；目录创建: " +
                             (ec ? ec.message() : "ok") + "；系统: " +
                             std::generic_category().message(errno) + "）");
            return;
        }
        out << doc.dump();
        out.flush();
        out.close();
        std::error_code rec;
        fs::rename(tmp, sp, rec);
        if (rec) {
            host_.log(3, "ithaca.agent: 会话落盘替换失败（" + sp.string() +
                             "）: " + rec.message());
            fs::remove(tmp, rec);
        }
    } catch (const std::exception& e) {
        host_.log(3, "ithaca.agent: 会话落盘异常: " + std::string(e.what()));
    }
}

// 撤回中断回合（语义见 agent_plugin.hpp 声明处）。truncate 与异常兜底同一清理面：
// 弹出 tool 结果段与悬挂 assistant 后，中断轮的 user 消息一并撤出（「从未出现过」；
// 尾判定保证它属于被撤轮次）——下一请求前缀在撤回点截断（用户显式丢弃该段，缓存
// 分叉即预期）。文件副作用不在撤回面（磁盘回滚走撤销快照）
void AgentCtx::revoke_interrupt_() {
    if (subagent_) return;   // 子会话无 UI 命令面
    if (state_ != State::Idle) {
        notice("会话运行中，无法撤回中断现场");
        return;
    }
    // 尾部现场判定：tool 结尾（有工具交互的取消）或 user 结尾（首个响应前即取消，
    // 或排队输入丢弃后的形态）——两者都可整体回退到该轮之前；中断后已续聊则尾部
    // 是 assistant/其他 ⇒ 拒绝
    if (messages_.empty()) {
        notice("没有可撤回的中断现场");
        return;
    }
    const std::string tail = messages_.back().value("role", "");
    if (tail != "tool" && tail != "user") {
        notice("没有可撤回的中断现场（中断后已有新对话）");
        return;
    }
    truncate_dangling_tool_exchange_();
    if (!messages_.empty() && messages_.back().value("role", "") == "user")
        messages_.erase(messages_.size() - 1);
    persist_session_();
    notice("已撤回中断回合（模型上下文与显示记录一并回退；文件改动未回滚，需要时用撤销处理）");
}

// 任务卡建档（仅缺卡时插到 system 之后）：跨轮冻结是缓存前缀纪律——卡坐在
// system 之后，逐轮重写会让其后整段历史前缀失效（Ark GLM 实测轮边界命中率塌到
// 个位数）；锚点的吸收点在压缩折叠（replace_with_digest_ 切割前合成）。策略缝：
// 表接管时卡内容由 mission 槽产出（null/失败 = 本轮不建档）；产出须沿用
// is_mission 标记形态（契约注明），原位替换靠标记识别。
bool AgentCtx::mission_from_slot_or_builtin_(json& out) {
    if (ctx_tbl_ == nullptr || ctx_tbl_->mission == nullptr) {
        out = vcagent::build_mission(messages_);
        return true;
    }
    const std::string msgs = messages_.dump();
    const char* o = nullptr;
    if (ctx_tbl_->mission(ctx_tbl_->self, msgs.c_str(), &o) != NOSTOS_OK || o == nullptr)
        return false;
    out = json::parse(o, nullptr, /*allow_exceptions=*/false);
    return out.is_object();
}

void AgentCtx::ensure_mission_() {
    for (std::size_t k = 0; k < messages_.size(); ++k)
        if (vcagent::is_mission(messages_[k])) return;
    json mission;
    if (!mission_from_slot_or_builtin_(mission))
        return;   // 表接管：null/失败/非对象 = 本轮不动（策略的显式决策）
    const std::size_t at = (!messages_.empty() && messages_[0].is_object() &&
                            messages_[0].value("role", "") == "system") ? 1 : 0;
    messages_.insert(messages_.begin() + static_cast<long>(at), mission);
}

}  // namespace vc_agent
