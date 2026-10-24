// 会话持久化与 mission 卡（plugin/agent 的 M6 session 切片）。
// state.json：历史 messages + turn 计数 + token 用量（activate 恢复与 webui
// 会话切换共用加载路径；每 turn 收口/压缩后落盘，失败仅告警）；
// mission 任务目标卡：turn 边界原位重写，turn 内 append-only（缓存前缀纪律）。

#include "agent_plugin.hpp"

#include <algorithm>  // std::max：turns 记全局计数器高水位

#include <cerrno>  // errno：落盘失败的系统层原因

#include <chrono>  // std::chrono：落盘瞬时锁重试等待

#include <fstream>  // std::ifstream/ofstream：state.json 读写

#include <thread>  // std::this_thread：300ms 重试间隔

namespace vc_agent {

// ---- 会话持久化与 mission 卡（src/ctx_session.cpp）--------------------------

// M6 session 持久化：历史 + turn 计数 + 用量落盘（失败仅告警，不影响任务）
// 注意：撤销快照栈是内存态，恢复后撤销栈从零开始（文档明示）。
// state 文件加载（activate 与会话切换共用；缺文件 = 全新会话）
void AgentCtx::load_session_state_(const std::string& path) {
    session_path_ = path;
    std::ifstream in(session_path_, std::ios::binary);
    json doc = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_discarded() && doc.is_object()) {
        turn_no_ = adopt_turn_floor(doc.value("turns", 0));
        const json usage = doc.value("usage", json::object());
        usage_prompt_ = usage.value("prompt_tokens", 0LL);
        usage_completion_ = usage.value("completion_tokens", 0LL);
        usage_total_ = usage.value("total_tokens", 0LL);
        usage_cache_ = usage.value("cache_tokens", 0LL);   // 缓存命中计量随会话恢复
        // 判定信号随会话恢复：恢复后首轮压缩判定不再失明
        last_prompt_tokens_ = doc.value("last_prompt_tokens", 0LL);
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
        // turns = 全局 turn 计数器高水位（含本 turn 派出的子代理消耗的号段），
        // 而非主会话轮号——恢复地板据此抬升；只记主轮号会让重启后的新子代理
        // 复用历史 transcript 里子代理的 turn_id，UI 按 turn_id 分组即新旧混组
        // （踩过：新 spawn 卡被上一个进程的完成卡标记为「已结束」）
        json doc = {{"turns", std::max(turn_no_, turn_seq().load())},
                    {"usage", usage},
                    {"last_prompt_tokens", last_prompt_tokens_},
                    {"messages", messages_}};
        std::error_code ec;
        fs::create_directories(fs::path(session_path_).parent_path(), ec);
        std::ofstream out(session_path_, std::ios::binary | std::ios::trunc);
        if (!out) {
            // 杀软/索引器/残留宿主造成的瞬时锁：等 300ms 重试一次
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            out.open(session_path_, std::ios::binary | std::ios::trunc);
        }
        if (!out) {
            host_.log(3, "ithaca.agent: 会话落盘失败（无法打开 " + session_path_ + "；目录创建: " +
                             (ec ? ec.message() : "ok") + "；系统: " +
                             std::generic_category().message(errno) + "）");
            return;
        }
        out << doc.dump();
    } catch (const std::exception& e) {
        host_.log(3, "ithaca.agent: 会话落盘异常: " + std::string(e.what()));
    }
}

// mission 任务目标卡重建（turn 边界 = 重建点）：已存在则原位重写内容，否则
// 插到 system 之后（下标 1）。turn 内不改动——固定前缀纪律
void AgentCtx::rebuild_mission_() {
    const json mission = vcagent::mission_message(vcagent::collect_tasks(messages_));
    for (std::size_t k = 1; k < messages_.size(); ++k)
        if (vcagent::is_mission(messages_[k])) {
            messages_[k] = mission;
            return;
        }
    const std::size_t at = (!messages_.empty() && messages_[0].is_object() &&
                            messages_[0].value("role", "") == "system") ? 1 : 0;
    messages_.insert(messages_.begin() + static_cast<long>(at), mission);
}

}  // namespace vc_agent
