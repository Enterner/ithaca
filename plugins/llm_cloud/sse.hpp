#pragma once
// llm_cloud 的纯逻辑层：SSE 行解析 + OpenAI 兼容 chunk 的增量/用量/错误提取。
// 与网络零耦合（字节流喂进来即可）——单元测试因此可以离线覆盖分块边界。
//
// SSE 事实约定（OpenAI/DeepSeek 实测形态）：
//   data: {...}\n\n            事件（本插件逐行处理，多行 data 的合并语义用不上）
//   data: [DONE]\n\n           结束标记
//   以 \n 或 \r\n 结尾；网络分块不保证对齐行尾——解析器内部缓冲拼行。
#include <nostos/nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace vcllm {

using nlohmann::json;

class SseParser {
public:
    // 喂一段网络字节流；每个完整行（不含行尾）回调一次。
    void feed(const char* data, std::size_t n,
              const std::function<void(const std::string&)>& on_line) {
        buf_.append(data, n);
        std::size_t pos = 0;
        for (;;) {
            const std::size_t eol = buf_.find('\n', pos);
            if (eol == std::string::npos) break;
            std::string line = buf_.substr(pos, eol - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            on_line(line);
            pos = eol + 1;
        }
        buf_.erase(0, pos);
    }

    // 流结束：把没有行尾的残余按一行冲刷（服务器异常断流的兜底）。
    void finish(const std::function<void(const std::string&)>& on_line) {
        if (buf_.empty()) return;
        std::string line = std::move(buf_);
        buf_.clear();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        on_line(line);
    }

private:
    std::string buf_;
};

// chat.completion.chunk → 增量文本。role 块 / usage 块 / finish 块都返回空串——
// 调用方跳过空增量即可。（DeepSeek reasoner 的 reasoning_content 增量见 delta_reasoning_of。）
inline std::string delta_content_of(const json& chunk) {
    if (!chunk.is_object()) return {};
    const auto choices = chunk.find("choices");
    if (choices == chunk.end() || !choices->is_array() || choices->empty()) return {};
    const auto& first = (*choices)[0];
    if (!first.is_object()) return {};
    const auto delta = first.find("delta");
    if (delta == first.end() || !delta->is_object()) return {};
    const auto content = delta->find("content");
    if (content == delta->end() || !content->is_string()) return {};
    return content->get<std::string>();
}

// 思考增量（reasoning_content）——实测所有模型均可产生；正文块 ⇒ 空串。
inline std::string delta_reasoning_of(const json& chunk) {
    if (!chunk.is_object()) return {};
    const auto choices = chunk.find("choices");
    if (choices == chunk.end() || !choices->is_array() || choices->empty()) return {};
    const auto& first = (*choices)[0];
    if (!first.is_object()) return {};
    const auto delta = first.find("delta");
    if (delta == first.end() || !delta->is_object()) return {};
    const auto rc = delta->find("reasoning_content");
    if (rc == delta->end() || !rc->is_string()) return {};
    return rc->get<std::string>();
}

// 末块的 usage（DeepSeek 默认在最后一个 chunk 携带）→ 人类可读摘要；无 ⇒ 空串。
inline std::string usage_of(const json& chunk) {
    if (!chunk.is_object()) return {};
    const auto usage = chunk.find("usage");
    if (usage == chunk.end() || !usage->is_object()) return {};
    const auto get = [&usage](const char* key) -> long long {
        const auto it = usage->find(key);
        return it != usage->end() && it->is_number_unsigned() ? static_cast<long long>(it->get<std::uint64_t>()) : 0;
    };
    return "tokens: total " + std::to_string(get("total_tokens")) + " (prompt " +
           std::to_string(get("prompt_tokens")) + " + completion " +
           std::to_string(get("completion_tokens")) + ")";
}

// 末块的 usage → 结构化对象；无 ⇒ null。
inline json usage_obj_of(const json& chunk) {
    if (!chunk.is_object()) return json();
    const auto usage = chunk.find("usage");
    if (usage == chunk.end() || !usage->is_object()) return json();
    json out = json::object();
    for (const char* key : {"prompt_tokens", "completion_tokens", "total_tokens"}) {
        const auto it = usage->find(key);
        if (it != usage->end() && it->is_number_unsigned())
            out[key] = it->get<std::uint64_t>();
    }
    // 缓存命中（可选，归一口径 = prompt 中命中缓存的部分）：DeepSeek
    // prompt_cache_hit_tokens / OpenAI prompt_tokens_details.cached_tokens——
    // 缓存收益度量（待办 12）与输出预留（19）的数据源；无则不报
    long long cache = 0;
    if (const auto it = usage->find("prompt_cache_hit_tokens");
        it != usage->end() && it->is_number_unsigned())
        cache = static_cast<long long>(it->get<std::uint64_t>());
    if (cache == 0) {
        const auto det = usage->find("prompt_tokens_details");
        if (det != usage->end() && det->is_object()) {
            const auto it = det->find("cached_tokens");
            if (it != det->end() && it->is_number_unsigned())
                cache = static_cast<long long>(it->get<std::uint64_t>());
        }
    }
    if (cache > 0) out["cache_tokens"] = cache;
    return out;
}

// 云端错误体 → 人类可读 message。{"error":{"message":...}} 优先（OpenAI/DeepSeek 形态）；
// 解析不出 JSON ⇒ 原文前 300 字节（非 UTF-8 时由调用方兜底）。
inline std::string error_message_of(const std::string& body) {
    const json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_discarded() && doc.is_object()) {
        const auto err = doc.find("error");
        if (err != doc.end() && err->is_object()) {
            const auto msg = err->find("message");
            if (msg != err->end() && msg->is_string()) return msg->get<std::string>();
        }
        if (err != doc.end() && err->is_string()) return err->get<std::string>();
    }
    return body.substr(0, body.size() < 300 ? body.size() : 300);
}

}  // namespace vcllm
