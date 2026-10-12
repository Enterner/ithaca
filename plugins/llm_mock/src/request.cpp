// plugin/llm_mock · 请求登记面：chat/chat_ex/cancel 的 thunk、注册表登记与回收。
// 入口/配置 -> src/plugin.cpp；回放与回流 -> src/replay.cpp。

#include "../include/llm_mock_plugin.hpp"

namespace vc_llm_mock {

nostos_status MockPlugin::chat_thunk(void* self, int request_id, const char* messages_json) noexcept {
    return static_cast<MockPlugin*>(self)->chat_(request_id, messages_json);
}

nostos_status MockPlugin::chat_ex_thunk(void* self, int request_id, const char* body_json) noexcept {
    // mock 不消费 tools——等价于普通 chat（agent 的 tools 定义只是透传声明）。
    return static_cast<MockPlugin*>(self)->chat_(request_id, body_json);
}

nostos_status MockPlugin::cancel_thunk(void* self, int request_id) noexcept {
    auto* p = static_cast<MockPlugin*>(self);
    // 标签制：按 rid 精确停；未知/已结束 rid = 正常竞态
    std::lock_guard<std::mutex> lock(p->requests_mu_);
    const auto it = p->requests_.find(request_id);
    if (it != p->requests_.end()) it->second.stopping.store(true);
    return NOSTOS_OK;
}

nostos_status MockPlugin::chat_(int request_id, const char* /*messages 或 body，mock 不消费*/) {
    {
        std::lock_guard<std::mutex> lock(requests_mu_);
        if (requests_.count(request_id) != 0) return NOSTOS_ERR_BAD_STATE;  // 重入拒绝
        auto& w = requests_[request_id];
        w.th = std::thread([this, request_id, workerp = &w] {
            replay_(request_id, workerp->stopping);
        });
    }
    return NOSTOS_OK;
}

// worker 收尾回收：经 poster 回宿主线程 join + 擦表（自 erase 会析构
// joinable thread => terminate；与 llm_cloud 同款纪律）
void MockPlugin::finish_request_(int rid) {
    nostos::kit::poster_submit(poster_, [this, rid] {
        std::lock_guard<std::mutex> lock(requests_mu_);
        const auto it = requests_.find(rid);
        if (it != requests_.end()) {
            if (it->second.th.joinable()) it->second.th.join();
            requests_.erase(it);
        }
    });
}

}  // namespace vc_llm_mock