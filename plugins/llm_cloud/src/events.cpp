// plugin/llm_cloud · 产物回流：delta/reasoning/done/error 组包后经 poster 回宿主
// 线程 emit（worker 线程绝不直接碰宿主 API）。登记 → src/request.cpp；请求线程
// → src/stream.cpp。

#include "llm_cloud_plugin.hpp"

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
    // 思维链回传（实验性，env 门控默认关）：Ark 实测接受 assistant.reasoning_content
    // 回传（多轮工具链形态 200 + 正确续写），换跨轮思维连续性；代价 = 明文思维链
    // 计入下轮输入 token（上下文膨胀 ~40%，Z Code 同款取舍）。agent 侧 messages_
    // 原样入库逐字回发，本插件之外零配合成本。
    static const bool reasoning_replay = [] {
        const char* v = std::getenv("ITHACA_LLM_REASONING_REPLAY");
        return v != nullptr && *v != '\0';
    }();
    if (reasoning_replay && !st.reasoning.empty())
        assistant["reasoning_content"] = st.reasoning;
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
