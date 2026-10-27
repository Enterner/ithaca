// plugin/llm_cloud · 请求线程：WinHTTP 全套 + SSE 逐行拼装 → post_*。全部请求态
// （拼装/取消旗/句柄/凭据快照）随线程走；端点解析/错误人话/句柄 RAII 等网络辅助
// 收在本 TU 匿名 ns。登记 → src/request.cpp；回流 → src/events.cpp。

#include "llm_cloud_plugin.hpp"
#include "../sse.hpp"  // vcllm::SseParser 等纯逻辑件（tests/unit 直接引用，留插件根）

#include <chrono>  // 请求耗时日志
#include <cctype>

namespace vc_llm_cloud {
namespace {

// ---- WinHTTP 句柄的小件 ------------------------------------------------------

struct HandleRAII {
    HINTERNET h = nullptr;
    ~HandleRAII() {
        if (h != nullptr) WinHttpCloseHandle(h);
    }
};

struct Endpoint {
    std::wstring host;
    INTERNET_PORT port = 443;
    std::wstring path;
    bool secure = true;
};

bool parse_endpoint(const std::wstring& base, Endpoint& out, std::string& err) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = 1;
    uc.dwHostNameLength = 1;
    uc.dwUrlPathLength = 1;
    if (!WinHttpCrackUrl(base.c_str(), static_cast<DWORD>(base.size()), 0, &uc)) {
        err = "API 地址无法解析（检查 ITHACA_API_BASE）";
        return false;
    }
    out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out.port = uc.nPort;
    out.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    out.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (out.path.empty() || out.path.back() == L'/') out.path += L"chat/completions";
    else out.path += L"/chat/completions";
    return true;
}

std::string last_error_text() {
    const DWORD e = GetLastError();
    switch (e) {
        case ERROR_WINHTTP_TIMEOUT: return "超时";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "域名解析失败";
        case ERROR_WINHTTP_CANNOT_CONNECT: return "无法连接（断网，或本地代理未启动）";
        case ERROR_WINHTTP_CONNECTION_ERROR: return "与服务器的连接被重置";
        default: return "WinHTTP 错误 " + std::to_string(e);
    }
}

}  // namespace

// ---- 请求线程 ----------------------------------------------------------------

void CloudPlugin::run_request_(int rid, json messages, json tools, std::string model,
                               std::string key_utf8, std::wstring key_wide,
                               std::string base_utf8, std::wstring base_wide,
                               std::uint64_t slot,
                               std::atomic<bool>& stopping, std::atomic<HINTERNET>& active) {
    (void)key_utf8;
    (void)base_utf8;  // 契约面保留 utf8 形态（日志/诊断用）；网络面只用宽字符
    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsed_ms = [t0] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
            .count();
    };
    host_.log(1, "ithaca.llm.cloud: 请求开始 rid=" + std::to_string(rid) +
                     " slot=" + std::to_string(slot) + " model=" + model +
                     " messages=" + std::to_string(messages.size()));
    StreamState st;
    int http_code = 0;
    std::string err;
    if (do_request_(rid, messages, tools, model, key_wide, base_wide, slot, st,
                    stopping, active, http_code, err)) {
        post_done_(rid, slot, st);
        // value() 对非对象（provider 未回 usage ⇒ st.usage 保持 null）抛
        // type_error.302——worker 线程无兜底 ⇒ terminate ⇒ abort（glm 流式
        // 末块 usage=null 实测命中；守卫必须先于取值）
        const long long cache =
            st.usage.is_object() ? st.usage.value("cache_tokens", 0LL) : 0LL;
        const long long prompt =
            st.usage.is_object() ? st.usage.value("prompt_tokens", 0LL) : 0LL;
        host_.log(1, "ithaca.llm.cloud: 请求完成 rid=" + std::to_string(rid) + " 耗时 " +
                         std::to_string(elapsed_ms()) + "ms（tool_calls " +
                         std::to_string(st.tc.size()) + " 条" +
                         (prompt > 0 ? "，prompt " + std::to_string(prompt) : "") +
                         (cache > 0 ? "，cache " + std::to_string(cache) : "") + "）");
    } else if (stopping.load()) {
        // 取消静默（mock 同款约定）：取消不是错误，收口事件由 agent 统一发——
        // 这里再发会造成双重错误卡
        host_.log(1, "ithaca.llm.cloud: 请求已取消 rid=" + std::to_string(rid) + " 耗时 " +
                         std::to_string(elapsed_ms()) + "ms");
    } else {
        post_error_(rid, slot, http_code, err);
        host_.log(3, "ithaca.llm.cloud: 请求失败 rid=" + std::to_string(rid) + " 耗时 " +
                         std::to_string(elapsed_ms()) + "ms http=" +
                         std::to_string(http_code) + " " + err);
    }
    // 回收：经 poster 回宿主线程 join + 擦表——worker 自 erase 会析构
    // joinable thread ⇒ terminate；宿主线程 join 已近完成的线程合法且短暂
    nostos::kit::poster_submit(poster_, [this, rid] {
        std::lock_guard<std::mutex> lock(requests_mu_);
        const auto it = requests_.find(rid);
        if (it != requests_.end()) {
            if (it->second.th.joinable()) it->second.th.join();
            requests_.erase(it);
        }
    });
}

std::string CloudPlugin::sanitize_fn_name_(const std::string& name) {
    std::string out;
    for (const char c : name)
        out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
                          ? c
                          : '_');
    return out;
}

bool CloudPlugin::do_request_(int rid, const json& messages, const json& tools,
                              const std::string& model, const std::wstring& key_wide,
                              const std::wstring& base_wide, std::uint64_t slot,
                              StreamState& st,
                              const std::atomic<bool>& stopping, std::atomic<HINTERNET>& active,
                              int& http_code, std::string& err) {
    Endpoint ep;
    if (!parse_endpoint(base_wide, ep, err)) return false;
    HandleRAII session{WinHttpOpen(L"ithaca/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (session.h == nullptr)  // 老系统不支持自动代理：回退默认代理
        session.h = WinHttpOpen(L"ithaca/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session.h == nullptr) { err = last_error_text(); return false; }
    HandleRAII conn{WinHttpConnect(session.h, ep.host.c_str(), ep.port, 0)};
    if (conn.h == nullptr) { err = "连接失败: " + last_error_text(); return false; }
    HandleRAII req{WinHttpOpenRequest(conn.h, L"POST", ep.path.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      ep.secure ? WINHTTP_FLAG_SECURE : 0)};
    if (req.h == nullptr) { err = last_error_text(); return false; }
    // 首字节窗 300s：thinking 模型读大上下文后服务端可能长时间不吐首字节，
    // 固定短窗会掐死合法长思考轮（踩过：122s 无数据 ⇒ 请求失败，同任务另一轮
    // 顺利通过——纯时序非确定性，不是模型/网络异常）。收到响应头后恢复 120s：
    // 流式期的判断单位是增量间隔，长窗反而拖慢取消与错误发现。
    WinHttpSetTimeouts(req.h, /*resolve*/ 10000, /*connect*/ 20000, /*send*/ 30000,
                       /*receive*/ 300000);

    json body = {{"model", model}, {"messages", messages}, {"stream", true}};
    if (tools.is_array() && !tools.empty()) {
        // DeepSeek/OpenAI 的 function.name 只允许 [a-zA-Z0-9_-]；内部工具名带点
        // （workspace.read）⇒ 请求侧净化、响应侧经 name_back 还原（踩过：HTTP 400）
        json safe_tools = json::array();
        for (const auto& t : tools) {
            json tool = t;
            if (tool.contains("function") && tool["function"].is_object() &&
                tool["function"].contains("name") &&
                tool["function"]["name"].is_string()) {
                const std::string orig = tool["function"]["name"].get<std::string>();
                const std::string safe = sanitize_fn_name_(orig);
                tool["function"]["name"] = safe;
                if (safe != orig) st.name_back[safe] = orig;
            }
            safe_tools.push_back(tool);
        }
        body["tools"] = safe_tools;
    }
    const std::string body_str = body.dump();
    const std::wstring headers =
        L"Content-Type: application/json\r\nAuthorization: Bearer " + key_wide;

    active.store(req.h);
    struct RequestCloser {  // cancel 与线程收尾谁先到都只关一次
        std::atomic<HINTERNET>& slot;
        ~RequestCloser() {
            if (HINTERNET h = slot.exchange(nullptr)) WinHttpCloseHandle(h);
        }
    } closer{active};

    if (!WinHttpSendRequest(req.h, headers.c_str(),
                            static_cast<DWORD>(headers.size()),
                            static_cast<LPVOID>(const_cast<char*>(body_str.data())),
                            static_cast<DWORD>(body_str.size()),
                            static_cast<DWORD>(body_str.size()), 0)) {
        err = "发送请求失败: " + last_error_text();
        return false;
    }
    if (!WinHttpReceiveResponse(req.h, nullptr)) {
        err = "接收响应失败: " + last_error_text();
        return false;
    }
    // 响应头已到 ⇒ 流式期恢复常规接收窗（增量间隔超 120s 才判死）
    WinHttpSetTimeouts(req.h, /*resolve*/ 10000, /*connect*/ 20000, /*send*/ 30000,
                       /*receive*/ 120000);

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    http_code = static_cast<int>(status);
    if (status != 200) {
        const std::string body_text = read_all_(req.h, 64 * 1024);
        err = "HTTP " + std::to_string(status) + ": " + vcllm::error_message_of(body_text);
        return false;
    }

    // ---- 流式读：SSE 逐行 → 增量经 poster 回宿主线程 ------------------------
    vcllm::SseParser sse;
    bool saw_done = false;
    for (;;) {
        if (stopping.load()) { err = "请求已取消"; return false; }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req.h, &available)) {
            err = "读取流失败: " + last_error_text();
            return false;
        }
        if (available == 0) break;  // 服务器关流（未见 [DONE] 也按完成处理——容错）
        std::string chunk(available, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req.h, chunk.data(), available, &got)) {
            err = "读取流失败: " + last_error_text();
            return false;
        }
        chunk.resize(got);
        sse.feed(chunk.data(), got, [&](const std::string& line) {
            if (line.rfind("data:", 0) != 0) return;  // 注释/其他字段忽略
            std::string payload = line.substr(5);
            if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
            if (payload == "[DONE]") { saw_done = true; return; }
            const json piece = json::parse(payload, nullptr, false);
            if (piece.is_discarded()) return;
            const std::string delta = vcllm::delta_content_of(piece);
            if (!delta.empty()) {
                st.content += delta;
                post_delta_(rid, slot, delta);
            }
            const std::string reasoning = vcllm::delta_reasoning_of(piece);
            if (!reasoning.empty()) {
                st.reasoning += reasoning;
                post_reasoning_(rid, slot, reasoning);
            }
            // tool_calls 碎片拼装（OpenAI 流式形态：按 index 分片，arguments 逐段追加）
            if (piece.contains("choices") && piece["choices"].is_array() &&
                !piece["choices"].empty() && piece["choices"][0].contains("delta") &&
                piece["choices"][0]["delta"].contains("tool_calls") &&
                piece["choices"][0]["delta"]["tool_calls"].is_array()) {
                for (const auto& tc : piece["choices"][0]["delta"]["tool_calls"]) {
                    if (!tc.is_object()) continue;
                    TcAcc& slot = st.tc[tc.value("index", 0)];
                    if (tc.contains("id") && tc["id"].is_string())
                        slot.id = tc["id"].get<std::string>();
                    if (tc.contains("function") && tc["function"].is_object()) {
                        const auto& fn = tc["function"];
                        if (fn.contains("name") && fn["name"].is_string()) {
                            const std::string raw = fn["name"].get<std::string>();
                            const auto it = st.name_back.find(raw);
                            slot.name = it != st.name_back.end() ? it->second : raw;
                        }
                        if (fn.contains("arguments") && fn["arguments"].is_string())
                            slot.args += fn["arguments"].get<std::string>();
                    }
                }
            }
            const json uobj = vcllm::usage_obj_of(piece);
            if (uobj.is_object()) st.usage = uobj;  // 末块带用量，done 时回传
        });
        if (saw_done) break;
    }
    sse.flush([](const std::string&) {});  // 无行尾残余：异常断流的碎片，丢弃
    return true;
}

std::string CloudPlugin::read_all_(HINTERNET req, std::size_t cap) {
    std::string out;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req, &available) || available == 0) break;
        std::string chunk(available, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req, chunk.data(), available, &got)) break;
        chunk.resize(got);
        out += chunk;
        if (out.size() >= cap) return out.substr(0, cap);
    }
    return out;
}

}  // namespace vc_llm_cloud
