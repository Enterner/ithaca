// SSE 解析与 OpenAI 兼容 chunk 提取的单测（离线：字节流边界/错误体/用量）。
#include "ithaca_test_framework.hpp"

#include "sse.hpp"

#include <string>
#include <vector>

VC_TEST("sse: 逐字节喂入——CRLF 行边界与 [DONE] 透传") {
    vcllm::SseParser sse;
    std::vector<std::string> lines;
    const std::string src = "data: {\"a\":1}\r\ndata: [DONE]\r\n\r\n";
    for (const char c : src) sse.feed(&c, 1, [&](const std::string& l) { lines.push_back(l); });
    sse.finish([&](const std::string& l) { lines.push_back(l); });

    REQUIRE(lines.size() == 3);  // 两个 data 行 + 一个空行（事件分隔）
    CHECK(lines[0] == "data: {\"a\":1}");
    CHECK(lines[1] == "data: [DONE]");
    CHECK(lines[2].empty());
}

VC_TEST("sse: 网络分块不对齐行尾——缓冲拼接后仍逐行完整") {
    vcllm::SseParser sse;
    std::vector<std::string> lines;
    const auto feed = [&](const char* s) { sse.feed(s, std::char_traits<char>::length(s), [&](const std::string& l) { lines.push_back(l); }); };
    feed("data: {\"ch");      // 切在 JSON 中间
    feed("oice\":\"你\"}\n"); // 行尾跨块
    feed("data: [DONE]\n");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "data: {\"choice\":\"你\"}");
    CHECK(lines[1] == "data: [DONE]");
}

VC_TEST("sse: finish 冲刷无行尾残余（异常断流兜底）") {
    vcllm::SseParser sse;
    std::vector<std::string> lines;
    sse.feed("data: x", 7, [&](const std::string& l) { lines.push_back(l); });
    CHECK(lines.empty());  // 未遇行尾：不产出
    sse.finish([&](const std::string& l) { lines.push_back(l); });
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "data: x");
}

VC_TEST("delta: content 提取；role/usage/空 choices 全部返回空") {
    using namespace vcllm;
    CHECK(delta_content_of(json::parse(R"({"choices":[{"delta":{"role":"assistant"}}]})")) == "");
    CHECK(delta_content_of(json::parse(R"({"choices":[{"delta":{"content":"你"}}]})")) == "你");
    CHECK(delta_content_of(json::parse(
              R"({"choices":[],"usage":{"prompt_tokens":5,"completion_tokens":7,"total_tokens":12}})")) == "");
    CHECK(delta_content_of(json::parse(R"({"choices":[{"delta":{"reasoning_content":"思考"}}]})")) == "");
    CHECK(delta_content_of(json::array()) == "");

    const auto usage_chunk = json::parse(
        R"({"choices":[],"usage":{"prompt_tokens":5,"completion_tokens":7,"total_tokens":12}})");
    const std::string usage = usage_of(usage_chunk);
    CHECK(usage.find("prompt 5") != std::string::npos);
    CHECK(usage.find("total 12") != std::string::npos);
    CHECK(usage_of(json::parse(R"({"choices":[{"delta":{"content":"x"}}]})")).empty());
}

VC_TEST("delta: 云端错误体——OpenAI 形态优先，非 JSON 原文兜底") {
    using namespace vcllm;
    const std::string msg = error_message_of(
        R"({"error":{"message":"Incorrect API key provided","type":"invalid_request_error"}})");
    CHECK(msg == "Incorrect API key provided");

    CHECK(error_message_of("Not JSON at all") == "Not JSON at all");
    CHECK(error_message_of("").empty());
}
