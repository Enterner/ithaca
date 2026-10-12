// plugin/llm_cloud —— OpenAI 兼容流式 provider（方案 §5.2）。
//
// 职责地图（拆分自单文件，格式对齐 mcp）：
//   src/plugin.cpp   入口与配置面：activate/deactivate、表装配、set_model/info、
//                    provider 热切（vc.llm.set_active 订阅）、env 装载、NOSTOS_PLUGIN
//   src/request.cpp  请求登记面：chat/chat_ex/cancel 的 thunk 与实现、start_ 的
//                    注册表登记（凭据快照/重入拒绝/并发上限）
//   src/stream.cpp   请求线程：WinHTTP 全套 + SSE 逐行拼装 → post_*；端点解析/
//                    错误人话/句柄 RAII 等网络辅助收在本 TU 匿名 ns
//   src/events.cpp   产物回流：delta/reasoning/done/error 经 poster 回宿主线程 emit
//   sse.hpp          SSE 行解析与 payload 提取的纯逻辑件（tests/unit/test_sse.cpp
//                    直接引用——留插件根，勿迁）
//
// 发布 vc.llm.v1（与 llm_mock 同名**可竞争**：同时加载两个 provider 时，后到者激活
// 失败并点名——宿主只知道契约，不知道也不关心是谁在供货）。
//
// 配置（env 层；key 永不落盘、永不进日志）：
//   ITHACA_API_KEY    必需，chat 时校验
//   ITHACA_API_BASE   默认 https://api.deepseek.com（OpenAI 兼容端均可）
//   ITHACA_MODEL      默认 deepseek-flash
//
// 线程纪律：chat() 起请求线程（标签制：多请求可并发在途，各自独立线程/连接/拼装态，
// docs/llm并发请求-方案.md）；线程内只做 WinHTTP 读写 + kit::poster_submit——一切
// 宿主 API（emit/log）都在 poster 任务（宿主线程带桥）里发生。cancel：按 rid 置停止
// 位 + 跨线程关请求句柄（WinHTTP 的阻塞读写会立即带错误返回），请求线程随后以
// 「已取消」的 error 事件收尾——没有悬空请求，没有悬空线程。
#pragma once

#include "vc_events.hpp"
#include "vc_llm.hpp"

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/poster.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

namespace vc_llm_cloud {

using nlohmann::json;

class CloudPlugin {
public:
    static constexpr std::string_view name = "ithaca.llm.cloud";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 1, 0);

    nostos_status activate(const nostos::abi::Host& host);
    void deactivate() noexcept;

private:
    // ---- 请求级类型（标签制，docs/llm并发请求-方案.md；先于使用点声明）----------

    struct TcAcc {
        std::string id;
        std::string name;
        std::string args;
    };
    struct StreamState {  // 每请求一份的流式拼装态（随 worker 走，不再进插件成员）
        std::string content;
        std::map<int, TcAcc> tc;
        std::map<std::string, std::string> name_back;  // 净化名 → 原始点分名（请求期有效）
        json usage;  // 末块的结构化用量（done 时回传给 agent 统计）
    };
    struct RequestWorker {
        std::thread th;
        std::atomic<bool> stopping{false};
        std::atomic<HINTERNET> active{nullptr};  // cancel 精确打断用
    };
    static constexpr std::size_t kMaxConcurrent = 8;  // 防失控上限（agent 本就单飞）

    // ---- 表槽位（thunk；登记面 → src/request.cpp，配置面 → src/plugin.cpp）------

    static nostos_status chat_thunk(void* self, int request_id, const char* messages_json) noexcept;
    static nostos_status chat_ex_thunk(void* self, int request_id, const char* body_json) noexcept;
    static nostos_status cancel_thunk(void* self, int request_id) noexcept;
    static nostos_status set_model_thunk(void* self, const char* model) noexcept;
    static nostos_status info_thunk(void* self, const char** out_json) noexcept;

    // ---- 入口与配置面（src/plugin.cpp）------------------------------------------

    void load_env_();
    // provider 热切（vc.llm.set_active）：匹配 ⇒ 发布自己；否则撤发布。
    // base_url 非空 ⇒ 同步热切请求目标（多供应商下每个供应商各自的端点）。
    void on_set_active_(const std::string& provider, const std::string& key,
                        const std::string& base_url);
    nostos_status set_model_(const char* model);

    // ---- 请求登记面（src/request.cpp）-------------------------------------------

    nostos_status chat_(int request_id, const char* messages_json);
    nostos_status chat_ex_(int request_id, const char* body_json);
    nostos_status start_(int request_id, json messages, json tools);

    // ---- 请求线程（src/stream.cpp）：只做 WinHTTP 与 poster_submit，绝不直接碰
    //      宿主 API。全部请求态（拼装/取消旗/句柄/凭据快照）随线程走——插件成员
    //      里没有"当前请求"。 ----------------------------------------------------

    void run_request_(int rid, json messages, json tools, std::string model,
                      std::string key_utf8, std::wstring key_wide,
                      std::string base_utf8, std::wstring base_wide,
                      std::atomic<bool>& stopping, std::atomic<HINTERNET>& active);
    bool do_request_(int rid, const json& messages, const json& tools,
                     const std::string& model, const std::wstring& key_wide,
                     const std::wstring& base_wide, StreamState& st,
                     const std::atomic<bool>& stopping, std::atomic<HINTERNET>& active,
                     int& http_code, std::string& err);
    static std::string read_all_(HINTERNET req, std::size_t cap);
    // 工具名净化（API 约束）与还原映射：sanitized → 原始点分名
    static std::string sanitize_fn_name_(const std::string& name);

    // ---- 产物回流（src/events.cpp）----------------------------------------------

    void post_delta_(int rid, std::string text);
    void post_reasoning_(int rid, std::string text);
    void post_done_(int rid, const StreamState& st);
    void post_error_(int rid, int code, std::string msg);

    // ---- 状态 --------------------------------------------------------------------

    nostos::abi::Host host_{};
    nostos_poster poster_{};
    vc_api::LlmTable table_{};
    std::string info_buf_;
    std::string out_buf_;  // （预留）同步出参缓冲

    std::string key_;  // 仅内存；不进日志
    std::wstring key_wide_;
    std::string base_utf8_;
    std::wstring base_wide_;
    std::string model_;  // model_ 的写（宿主线程 set_model）与读（worker 组包快照）互斥

    std::map<int, RequestWorker> requests_;  // request_id → 在途请求
    std::mutex requests_mu_;

    std::vector<nostos::abi::Host::Subscription> subs_;  // provider hot-switch subscription
    std::mutex model_mu_;
    bool published_ = false;  // 是否已发布 vc.llm.v1（provider 热切）
};

}  // namespace vc_llm_cloud
