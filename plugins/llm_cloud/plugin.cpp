// plugin/llm_cloud —— OpenAI 兼容流式 provider（方案 §5.2）。
//
// 发布 vc.llm.v1（与 llm_mock 同名**可竞争**：同时加载两个 provider 时，后到者激活
// 失败并点名——宿主只知道契约，不知道也不关心是谁在供货）。
//
// 配置（env 层；key 永不落盘、永不进日志）：
//   ITHACA_API_KEY    必需，chat 时校验
//   ITHACA_API_BASE   默认 https://api.deepseek.com（OpenAI 兼容端均可）
//   ITHACA_MODEL      默认 deepseek-flash
//
// 线程纪律：chat() 起一个请求线程（一次一条在途）；线程内只做 WinHTTP 读写 +
// kit::poster_submit——一切宿主 API（emit/log）都在 poster 任务（宿主线程带桥）里发生。
// cancel：置停止位 + 跨线程关闭请求句柄（WinHTTP 的阻塞读写会立即带错误返回），
// 请求线程随后以「已取消」的 error 事件收尾——没有悬空请求，没有悬空线程。
#include "vc_events.hpp"
#include "vc_llm.hpp"
#include "sse.hpp"

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/poster.hpp>
#include <nostos/kit/win.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace {

using nlohmann::json;

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
    if (out.path.empty()) out.path = L"";
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

class CloudPlugin {
public:
    static constexpr std::string_view name = "ithaca.llm.cloud";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 1, 0);

    nostos_status activate(const nostos::abi::Host& host) {
        host_ = host;
        load_env_();

        nostos::abi::init_table(table_);
        table_.self = this;
        table_.chat = &CloudPlugin::chat_thunk;
        table_.cancel = &CloudPlugin::cancel_thunk;
        table_.info = &CloudPlugin::info_thunk;
        table_.chat_ex = &CloudPlugin::chat_ex_thunk;
        table_.set_model = &CloudPlugin::set_model_thunk;
        table_.set_model = &CloudPlugin::set_model_thunk;
        // provider 热切：按 ITHACA_LLM_ACTIVE 决定是否立即发布；
        // 缺省（env 未设）= 发布（单 provider 启动如 vc.ps1 保持原行为）
        const char* llm_act = std::getenv("ITHACA_LLM_ACTIVE");
        published_ = (llm_act == nullptr || *llm_act == '\0' ||
                      std::string_view(llm_act) == "cloud");
        if (published_) {
            if (host.publish(vc_api::llm, &table_) != NOSTOS_OK) {
                host.log(4, "ithaca.llm.cloud: 发布 vc.llm.v1 失败（同名服务已存在？）");
                return NOSTOS_ERR_FAILED;
            }
        } else {
            host.log(2, "ithaca.llm.cloud: 休眠（当前 provider 非 cloud，可经 webui 热切）");
        }
        if (host.acquire_poster(&poster_) != NOSTOS_OK) {
            host.log(4, "ithaca.llm.cloud: 领取 poster 失败（宿主 ABI 过旧或未实现）");
            return NOSTOS_ERR_FAILED;
        }
        // provider 热切订阅
        subs_.push_back(host_.on(vc_api::llm_set_active, [this](const void* p) {
            const auto* e = static_cast<const vc_api::LlmSetActive*>(p);
            on_set_active_(e->provider != nullptr ? std::string(e->provider) : "",
                           e->key != nullptr ? std::string(e->key) : "",
                           e->base_url != nullptr ? std::string(e->base_url) : "");
        }));
        host.log(2, "ithaca.llm.cloud: 就绪 base=" + base_utf8_ + " model=" + model_ +
                        " key=" + (key_.empty() ? "未设置" : "已设置（不落盘）"));
        return NOSTOS_OK;
    }

    void deactivate() noexcept {
        subs_.clear();
        stopping_.store(true);
        if (HINTERNET h = active_request_.exchange(nullptr)) WinHttpCloseHandle(h);  // 阻塞读写立即带错返回
        if (worker_.joinable()) worker_.join();
        host_.log(2, "ithaca.llm.cloud: 已停用");
    }

private:
    // ---- 表槽位 ----------------------------------------------------------------

    static nostos_status chat_thunk(void* self, int request_id, const char* messages_json) noexcept {
        return static_cast<CloudPlugin*>(self)->chat_(request_id, messages_json);
    }
    static nostos_status chat_ex_thunk(void* self, int request_id, const char* body_json) noexcept {
        return static_cast<CloudPlugin*>(self)->chat_ex_(request_id, body_json);
    }
    static nostos_status cancel_thunk(void* self, int request_id) noexcept {
        auto* p = static_cast<CloudPlugin*>(self);
        (void)request_id;  // M2 单请求在途：cancel 即停当前请求
        p->stopping_.store(true);
        if (HINTERNET h = p->active_request_.exchange(nullptr)) WinHttpCloseHandle(h);
        return NOSTOS_OK;
    }
    static nostos_status set_model_thunk(void* self, const char* model) noexcept {
        return static_cast<CloudPlugin*>(self)->set_model_(model);
    }
    // provider 热切（vc.llm.set_active）：匹配 ⇒ 发布自己；否则撤发布。
    // base_url 非空 ⇒ 同步热切请求目标（多供应商下每个供应商各自的端点）；
    // base_wide_ 的读写跨线程模式与 key_wide_ 一致（既有取舍，切换时无在途请求为常态）。
    void on_set_active_(const std::string& provider, const std::string& key,
                        const std::string& base_url) {
        const bool want = (provider == "cloud");
        if (want) {
            if (!key.empty()) {
                std::lock_guard<std::mutex> lock(model_mu_);
                key_ = key;
                key_wide_ = nostos::kit::win::utf8_to_wide(key);
            }
            if (!base_url.empty()) {
                std::lock_guard<std::mutex> lock(model_mu_);
                base_utf8_ = base_url;
                base_wide_ = nostos::kit::win::utf8_to_wide(base_url);
            }
            if (!published_) {
                if (host_.publish(vc_api::llm, &table_) == NOSTOS_OK) {
                    published_ = true;
                    host_.log(2, "ithaca.llm.cloud: 已激活（热切）");
                }
            }
        } else if (published_) {
            host_.unpublish(vc_api::llm);
            published_ = false;
            host_.log(2, "ithaca.llm.cloud: 已休眠（provider 热切）");
        }
    }
    nostos_status set_model_(const char* model) {
        if (model == nullptr || *model == '\0') return NOSTOS_ERR_INVALID_ARG;
        { std::lock_guard<std::mutex> lock(model_mu_); model_ = model; }
        host_.log(2, "ithaca.llm.cloud: 模型热切为 " + std::string(model));
        return NOSTOS_OK;
    }
    static nostos_status info_thunk(void* self, const char** out_json) noexcept {
        auto* p = static_cast<CloudPlugin*>(self);
        p->info_buf_ = json{{"provider", "cloud"},
                            {"model", p->model_},
                            {"base", p->base_utf8_}}.dump();
        *out_json = p->info_buf_.c_str();
        return NOSTOS_OK;
    }

    nostos_status chat_(int request_id, const char* messages_json) {
        json messages = json::parse(messages_json != nullptr ? messages_json : "", nullptr,
                                    /*allow_exceptions=*/false);
        if (!messages.is_array()) {
            host_.log(4, "ithaca.llm.cloud: messages_json 必须是数组");
            return NOSTOS_ERR_INVALID_ARG;
        }
        return start_(request_id, std::move(messages), json());
    }

    nostos_status chat_ex_(int request_id, const char* body_json) {
        json doc = json::parse(body_json != nullptr ? body_json : "", nullptr,
                               /*allow_exceptions=*/false);
        if (!doc.is_object()) {
            host_.log(4, "ithaca.llm.cloud: body_json 必须是对象（messages/tools）");
            return NOSTOS_ERR_INVALID_ARG;
        }
        json messages = doc.value("messages", json::array());
        json tools = doc.contains("tools") ? doc["tools"] : json();
        return start_(request_id, std::move(messages), std::move(tools));
    }

    nostos_status start_(int request_id, json messages, json tools) {
        if (worker_.joinable()) worker_.join();  // 上一条已跑完：收尸后放行
        stopping_.store(false);  // 新请求显式复位
        if (key_.empty()) {
            host_.log(4, "ithaca.llm.cloud: ITHACA_API_KEY 未设置——无法发起云端请求");
            return NOSTOS_ERR_FAILED;
        }
        request_id_ = request_id;
        content_acc_.clear();
        tc_acc_.clear();
        name_back_.clear();
        usage_json_ = json();
        worker_ = std::thread([this, messages = std::move(messages), tools = std::move(tools)] {
            run_request_(messages, tools);
        });
        return NOSTOS_OK;
    }

    // ---- 请求线程：只做 WinHTTP 与 poster_submit，绝不直接碰宿主 API -----------

    void run_request_(const json& messages, const json& tools) {
        int http_code = 0;
        std::string err;
        if (do_request_(messages, tools, http_code, err)) {
            post_done_();
        } else if (stopping_.load()) {
            // 取消静默（mock 同款约定）：取消不是错误，收口事件由 agent 统一发——
            // 这里再发会造成双重错误卡
        } else {
            post_error_(http_code, err);
        }
    }

    // 工具名净化（API 约束）与还原映射：sanitized → 原始点分名
    static std::string sanitize_fn_name_(const std::string& name) {
        std::string out;
        for (const char c : name)
            out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
                              ? c
                              : '_');
        return out;
    }

    bool do_request_(const json& messages, const json& tools, int& http_code, std::string& err) {
        Endpoint ep;
        if (!parse_endpoint(base_wide_, ep, err)) return false;
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
        WinHttpSetTimeouts(req.h, /*resolve*/ 10000, /*connect*/ 20000, /*send*/ 30000,
                           /*receive*/ 120000);

        std::string model;  // set_model 可在宿主线程热切——取快照避免竞态
        { std::lock_guard<std::mutex> lock(model_mu_); model = model_; }
        json body = {{"model", model}, {"messages", messages}, {"stream", true}};
        if (tools.is_array() && !tools.empty()) {
            // DeepSeek/OpenAI 的 function.name 只允许 [a-zA-Z0-9_-]；内部工具名带点
            // （workspace.read）⇒ 请求侧净化、响应侧经 name_back_ 还原（踩过：HTTP 400）
            json safe_tools = json::array();
            for (const auto& t : tools) {
                json tool = t;
                if (tool.contains("function") && tool["function"].is_object() &&
                    tool["function"].contains("name") &&
                    tool["function"]["name"].is_string()) {
                    const std::string orig = tool["function"]["name"].get<std::string>();
                    const std::string safe = sanitize_fn_name_(orig);
                    tool["function"]["name"] = safe;
                    if (safe != orig) name_back_[safe] = orig;
                }
                safe_tools.push_back(tool);
            }
            body["tools"] = safe_tools;
        }
        const std::string body_str = body.dump();
        const std::wstring headers =
            L"Content-Type: application/json\r\nAuthorization: Bearer " + key_wide_;

        active_request_.store(req.h);
        struct RequestCloser {  // cancel 与线程收尾谁先到都只关一次
            std::atomic<HINTERNET>& slot;
            ~RequestCloser() {
                if (HINTERNET h = slot.exchange(nullptr)) WinHttpCloseHandle(h);
            }
        } closer{active_request_};

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
            if (stopping_.load()) { err = "请求已取消"; return false; }
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
                    content_acc_ += delta;
                    post_delta_(delta);
                }
                const std::string reasoning = vcllm::delta_reasoning_of(piece);
                if (!reasoning.empty()) post_reasoning_(reasoning);
                // tool_calls 碎片拼装（OpenAI 流式形态：按 index 分片，arguments 逐段追加）
                if (piece.contains("choices") && piece["choices"].is_array() &&
                    !piece["choices"].empty() && piece["choices"][0].contains("delta") &&
                    piece["choices"][0]["delta"].contains("tool_calls") &&
                    piece["choices"][0]["delta"]["tool_calls"].is_array()) {
                    for (const auto& tc : piece["choices"][0]["delta"]["tool_calls"]) {
                        if (!tc.is_object()) continue;
                        TcAcc& slot = tc_acc_[tc.value("index", 0)];
                        if (tc.contains("id") && tc["id"].is_string())
                            slot.id = tc["id"].get<std::string>();
                        if (tc.contains("function") && tc["function"].is_object()) {
                            const auto& fn = tc["function"];
                            if (fn.contains("name") && fn["name"].is_string()) {
                                const std::string raw = fn["name"].get<std::string>();
                                const auto it = name_back_.find(raw);
                                slot.name = it != name_back_.end() ? it->second : raw;
                            }
                            if (fn.contains("arguments") && fn["arguments"].is_string())
                                slot.args += fn["arguments"].get<std::string>();
                        }
                    }
                }
                const json uobj = vcllm::usage_obj_of(piece);
                if (uobj.is_object()) usage_json_ = uobj;  // 末块带用量，done 时回传
            });
            if (saw_done) break;
        }
        sse.finish([](const std::string&) {});  // 无行尾残余：异常断流的碎片，丢弃
        return true;
    }

    static std::string read_all_(HINTERNET req, std::size_t cap) {
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

    // ---- 产物回流（poster → 宿主线程 emit）--------------------------------------

    void post_delta_(std::string text) {
        nostos::kit::poster_submit(poster_, [this, text = std::move(text)] {
            const vc_api::LlmDelta e{request_id_, text.c_str()};
            host_.emit(vc_api::llm_delta, &e);
        });
    }
    void post_reasoning_(std::string text) {
        nostos::kit::poster_submit(poster_, [this, text = std::move(text)] {
            const vc_api::LlmReasoning e{request_id_, text.c_str()};
            host_.emit(vc_api::llm_reasoning, &e);
        });
    }
    void post_done_() {
        // 完整 assistant 消息：content + 按序拼装的 tool_calls契约）
        json assistant = {{"role", "assistant"}, {"content", content_acc_}};
        if (!tc_acc_.empty()) {
            json tcs = json::array();
            for (auto& [idx, tc] : tc_acc_)
                tcs.push_back(json{{"id", tc.id},
                                   {"type", "function"},
                                   {"function", {{"name", tc.name}, {"arguments", tc.args}}}});
            assistant["tool_calls"] = tcs;
        }
        const std::string assistant_json = assistant.dump();
        const std::string usage_json =
            usage_json_.is_object() ? usage_json_.dump() : std::string{};
        nostos::kit::poster_submit(poster_, [this, s = assistant_json, u = usage_json] {
            const vc_api::LlmDone e{request_id_, s.c_str(), u.c_str()};
            host_.emit(vc_api::llm_done, &e);
        });
    }
    void post_error_(int code, std::string msg) {
        nostos::kit::poster_submit(poster_, [this, code, msg = std::move(msg)] {
            const vc_api::LlmError e{request_id_, code, msg.c_str()};
            host_.emit(vc_api::llm_error, &e);
        });
    }

    void load_env_() {
        if (const char* v = std::getenv("ITHACA_API_KEY")) key_ = v;
        if (const char* v = std::getenv("ITHACA_API_BASE");
            v != nullptr && *v != '\0') {
            base_utf8_ = v;
        } else {
            base_utf8_ = "https://api.deepseek.com";
        }
        base_wide_ = nostos::kit::win::utf8_to_wide(base_utf8_);
        if (const char* v = std::getenv("ITHACA_MODEL"); v != nullptr && *v != '\0') model_ = v;
        else model_ = "deepseek-flash";
        key_wide_ = nostos::kit::win::utf8_to_wide(key_);
    }

    // ---- 状态 ---------------------------------------------------------------------

    nostos::abi::Host host_{};
    nostos_poster poster_{};
    vc_api::LlmTable table_{};
    std::string info_buf_;
    std::string out_buf_;  // （预留）同步出参缓冲

    std::string key_;       // 仅内存；不进日志
    std::wstring key_wide_;
    std::string base_utf8_;
    std::wstring base_wide_;
    std::string model_;

    std::thread worker_{};
    std::atomic<bool> stopping_{false};
    std::atomic<HINTERNET> active_request_{nullptr};
    int request_id_ = 0;

    // 流式拼装（done 时合成完整 assistant 消息）
    struct TcAcc {
        std::string id;
        std::string name;
        std::string args;
    };
    std::string content_acc_;
    std::map<int, TcAcc> tc_acc_;
    std::map<std::string, std::string> name_back_;  // 净化名 → 原始点分名（请求期有效）
    json usage_json_;  // 末块的结构化用量（done 时回传给 agent 统计）
    std::vector<nostos::abi::Host::Subscription> subs_;  // provider hot-switch subscription
    std::mutex model_mu_;
    bool published_ = false;   // 是否已发布 vc.llm.v1（provider 热切）  // model_ 的写（宿主线程 set_model）与读（worker 组包）互斥
};

}  // namespace

NOSTOS_PLUGIN(CloudPlugin)
