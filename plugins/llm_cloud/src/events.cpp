// plugin/llm_cloud · 产物回流：delta/reasoning/done/error 组包后经 poster 回宿主
// 线程 emit（worker 线程绝不直接碰宿主 API）。登记 → src/request.cpp；请求线程
// → src/stream.cpp。

#include "../include/llm_cloud_plugin.hpp"

namespace vc_llm_cloud {

// 产物回流：slot=0 广播（主会话，与迁移前一致）；slot!=0 单播（子会话私有，
// 不进广播通道——子代理的流式/收口事件只在归属槽可见）。
void CloudPlugin::post_delta_(int rid, std::uint64_t slot, std::string text) {
    nostos::kit::poster_submit(poster_, [this, rid, slot, text = std::move(text)] {
        const vc_api::LlmDelta e{rid, text.c_str()};
        host_.emit_slot(vc_api::llm_delta, slot, &e);
    });
}

void CloudPlugin::post_reasoning_(int rid, std::uint64_t slot, std::string text) {
    nostos::kit::poster_submit(poster_, [this, rid, slot, text = std::move(text)] {
        const vc_api::LlmReasoning e{rid, text.c_str()};
        host_.emit_slot(vc_api::llm_reasoning, slot, &e);
    });
}

void CloudPlugin::post_done_(int rid, std::uint64_t slot, const StreamState& st) {
    // 完整 assistant 消息：content + 按序拼装的 tool_calls（契约）
    json assistant = {{"role", "assistant"}, {"content", st.content}};
    if (!st.tc.empty()) {
        json tcs = json::array();
        for (auto& [idx, tc] : st.tc)
            tcs.push_back(json{{"id", tc.id},
                               {"type", "function"},
                               {"function", {{"name", tc.name}, {"arguments", tc.args}}}});
        assistant["tool_calls"] = tcs;
    }
    const std::string assistant_json = assistant.dump();
    const std::string usage_json = st.usage.is_object() ? st.usage.dump() : std::string{};
    nostos::kit::poster_submit(poster_, [this, rid, slot, s = assistant_json,
                                         u = usage_json] {
        const vc_api::LlmDone e{rid, s.c_str(), u.c_str()};
        host_.emit_slot(vc_api::llm_done, slot, &e);
    });
}

void CloudPlugin::post_error_(int rid, std::uint64_t slot, int code, std::string msg) {
    nostos::kit::poster_submit(poster_, [this, rid, slot, code, msg = std::move(msg)] {
        const vc_api::LlmError e{rid, code, msg.c_str()};
        host_.emit_slot(vc_api::llm_error, slot, &e);
    });
}

}  // namespace vc_llm_cloud
