// plugin/llm_mock · 回放线程与产物回流：逐行 delta/推理事件/done 组包经 poster
// 回宿主线程 emit（回放线程绝不直接碰宿主 API）；内置剧本文本与行切分工具。
// 登记与回收 -> src/request.cpp。

#include "../include/llm_mock_plugin.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace vc_llm_mock {

// ---- 回放线程（rid 标签制：请求态随线程走，插件成员里没有"当前请求"）-----------

void MockPlugin::replay_(int rid, std::uint64_t slot, const std::atomic<bool>& stopping) {
    // 模拟深度思考增量（推理显示链路可测；真实推理来自 deepseek-reasoner）
    {
        const std::string think = "（模拟深度思考）正在分析请求并组织回答…";
        nostos::kit::poster_submit(poster_, [this, rid, slot, text = think] {
            const vc_api::LlmReasoning e{rid, text.c_str()};
            host_.emit_slot(vc_api::llm_reasoning, slot, &e);
        });
    }
    if (turns_mode_) {
        if (turn_index_ >= turns_.size()) {  // 剧本耗尽
            post_done_(rid, slot,
                       json{{"role", "assistant"}, {"content", "__MOCK_EXHAUSTED__"}},
                       json{{"prompt_tokens", 1}, {"completion_tokens", 1}, {"total_tokens", 2}});
            finish_request_(rid);
            return;
        }
        const std::size_t idx = turn_index_++;
        json turn = turns_[idx];
        // 剧本条目按定义是 assistant 回合——role 缺失时注入（下游按 role 过滤
        // 消息，缺 role 会让折叠/配对等消费方静默跳过）
        if (!turn.contains("role")) turn["role"] = "assistant";
        const std::string content = turn.value("content", std::string{});
        if (stopping.load(std::memory_order_relaxed)) { finish_request_(rid); return; }
        for (std::size_t i = 0; i < content.size(); ++i) {
            const std::size_t nl = content.find('\n', i);
            const std::size_t end = nl == std::string::npos ? content.size() : nl + 1;
            post_delta_(rid, slot, content.substr(i, end - i));
            i = end - 1;
            if (stopping.load(std::memory_order_relaxed)) { finish_request_(rid); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }
        if (!stopping.load(std::memory_order_relaxed)) {
            // 用量：剧本可携带 "usage"；缺省按轮次合成（确定性——回归 golden 依赖）
            const std::size_t one = idx + 1;
            json usage = turn.contains("usage")
                             ? turn["usage"]
                             : json{{"prompt_tokens", 100 * static_cast<int>(one)},
                                    {"completion_tokens", 10 * static_cast<int>(one)},
                                    {"total_tokens", 110 * static_cast<int>(one)}};
            post_done_(rid, slot, turn, usage);
        }
        finish_request_(rid);
        return;
    }

    // 内置文本剧本（冒烟形态）
    for (const std::string& line : script_) {
        if (stopping.load(std::memory_order_relaxed)) { finish_request_(rid); return; }
        post_delta_(rid, slot, line);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
    }
    if (!stopping.load(std::memory_order_relaxed))
        post_done_(rid, slot, json{{"role", "assistant"}, {"content", builtin_text_()}},
                   json{{"prompt_tokens", 40},
                        {"completion_tokens", 10},
                        {"total_tokens", 50}});
    finish_request_(rid);
}

std::string MockPlugin::builtin_text_() const {
    std::string all;
    for (const auto& l : script_) all += l;
    return all;
}

// ---- 事件回流（kit::poster_submit；载荷所有权/失败回收都在助手内）--------------

// 产物回流：slot=0 广播（主会话）；slot!=0 单播（子会话私有，不进广播通道）。
void MockPlugin::post_delta_(int rid, std::uint64_t slot, std::string text) {
    nostos::kit::poster_submit(poster_, [this, rid, slot, text = std::move(text)] {
        const vc_api::LlmDelta e{rid, text.c_str()};
        host_.emit_slot(vc_api::llm_delta, slot, &e);
    });
}

void MockPlugin::post_done_(int rid, std::uint64_t slot, const json& assistant, const json& usage) {
    const std::string assistant_json = assistant.dump();
    const std::string usage_json = usage.dump();
    nostos::kit::poster_submit(poster_, [this, rid, slot, s = assistant_json, u = usage_json] {
        const vc_api::LlmDone e{rid, s.c_str(), u.c_str()};
        host_.emit_slot(vc_api::llm_done, slot, &e);
    });
}

}  // namespace vc_llm_mock