// plugin/llm_cloud · 请求登记面：chat/chat_ex/cancel 的 thunk 与实现、start_ 的
// 注册表登记（凭据快照/重入拒绝/并发上限）。请求线程 → src/stream.cpp。

#include "../include/llm_cloud_plugin.hpp"

namespace vc_llm_cloud {

nostos_status CloudPlugin::chat_thunk(void* self, int request_id, const char* messages_json) noexcept {
    return static_cast<CloudPlugin*>(self)->chat_(request_id, messages_json);
}

nostos_status CloudPlugin::chat_ex_thunk(void* self, int request_id, const char* body_json) noexcept {
    return static_cast<CloudPlugin*>(self)->chat_ex_(request_id, body_json);
}

nostos_status CloudPlugin::cancel_thunk(void* self, int request_id) noexcept {
    auto* p = static_cast<CloudPlugin*>(self);
    // 标签制：按 rid 精确停；未知/已结束 rid = 正常竞态（取消不是错误）
    std::lock_guard<std::mutex> lock(p->requests_mu_);
    const auto it = p->requests_.find(request_id);
    if (it == p->requests_.end()) return NOSTOS_OK;
    it->second.stopping.store(true);
    if (HINTERNET h = it->second.active.exchange(nullptr)) WinHttpCloseHandle(h);
    return NOSTOS_OK;
}

nostos_status CloudPlugin::chat_(int request_id, const char* messages_json) {
    json messages = json::parse(messages_json != nullptr ? messages_json : "", nullptr,
                                /*allow_exceptions=*/false);
    if (!messages.is_array()) {
        host_.log(4, "ithaca.llm.cloud: messages_json 必须是数组");
        return NOSTOS_ERR_INVALID_ARG;
    }
    return start_(request_id, std::move(messages), json(), 0);
}

nostos_status CloudPlugin::chat_ex_(int request_id, const char* body_json) {
    json doc = json::parse(body_json != nullptr ? body_json : "", nullptr,
                           /*allow_exceptions=*/false);
    if (!doc.is_object()) {
        host_.log(4, "ithaca.llm.cloud: body_json 必须是对象（messages/tools）");
        return NOSTOS_ERR_INVALID_ARG;
    }
    json messages = doc.value("messages", json::array());
    json tools = doc.contains("tools") ? doc["tools"] : json();
    // slot 路由键：agent 子任务 = 子会话槽 turn_no（非 0）。非 0 ⇒ 凭据走子档案
    // 且事件按槽单播；0 = 主会话请求，广播回发（与迁移前行为一致）。
    const std::uint64_t slot = doc.value("slot", static_cast<std::uint64_t>(0));
    return start_(request_id, std::move(messages), std::move(tools), slot);
}

nostos_status CloudPlugin::start_(int request_id, json messages, json tools,
                                  std::uint64_t slot) {
    // 凭据/端点/模型快照进 worker 闭包：provider 热切只影响后续请求。快照在锁内
    // 一次取齐——并发在途与热切并存时，worker 手里的凭据/端点/模型是自洽的一份，
    // 不存在跨线程读竞态。子会话请求（slot!=0）且子档案齐全 ⇒ 按子档案发起（跨
    // provider 派遣的唯一通路）；档案缺失回落主档案——"子用主模型"是默认形态。
    std::string model, key_utf8, base_utf8;
    std::wstring key_wide, base_wide;
    bool sub_used = false;
    {
        std::lock_guard<std::mutex> lock(model_mu_);
        if (slot != 0 && !sub_model_.empty() && !sub_key_.empty() &&
            !sub_base_utf8_.empty()) {
            model = sub_model_;
            key_utf8 = sub_key_;
            key_wide = sub_key_wide_;
            base_utf8 = sub_base_utf8_;
            base_wide = sub_base_wide_;
            sub_used = true;
        } else {
            model = model_;
            key_utf8 = key_;
            key_wide = key_wide_;
            base_utf8 = base_utf8_;
            base_wide = base_wide_;
        }
    }
    if (slot != 0 && !sub_used)
        host_.log(1, "ithaca.llm.cloud: 子会话请求无子档案，回落主模型 " + model);
    if (key_utf8.empty()) {
        host_.log(4, "ithaca.llm.cloud: ITHACA_API_KEY 未设置——无法发起云端请求");
        return NOSTOS_ERR_FAILED;
    }
    {
        std::lock_guard<std::mutex> lock(requests_mu_);
        if (requests_.count(request_id) != 0) {
            host_.log(4, "ithaca.llm.cloud: request_id " + std::to_string(request_id) +
                             " 已在途（调用方 bug，拒绝重入）");
            return NOSTOS_ERR_FAILED;
        }
        if (requests_.size() >= kMaxConcurrent) {
            host_.log(3, "ithaca.llm.cloud: 并发请求已达上限 " +
                             std::to_string(kMaxConcurrent) + "，拒绝新请求");
            return NOSTOS_ERR_FAILED;
        }
        RequestWorker& w = requests_[request_id];
        w.th = std::thread([this, request_id, messages = std::move(messages),
                            tools = std::move(tools), model, key_utf8, key_wide,
                            base_utf8, base_wide, slot, workerp = &w]() mutable {
            run_request_(request_id, std::move(messages), std::move(tools), model,
                         key_utf8, key_wide, base_utf8, base_wide, slot,
                         workerp->stopping, workerp->active);
        });
    }
    return NOSTOS_OK;
}

}  // namespace vc_llm_cloud
