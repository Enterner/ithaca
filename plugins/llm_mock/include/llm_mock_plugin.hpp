// plugin/llm_mock —— 确定性假 provider（vc.llm.v1 的第二份实现）。
//
// 职责地图（格式对齐 mcp/llm_cloud）：
//   src/plugin.cpp   入口与配置面：activate（表装配/发布门槛/set_active 订阅）、
//                    deactivate（全停全收）、set_model/info、剧本装载、NOSTOS_PLUGIN
//   src/request.cpp  请求登记面：chat/chat_ex/cancel 的 thunk、注册表登记与回收
//   src/replay.cpp   回放线程与产物回流：逐行 delta/推理事件/done 组包经 poster emit；
//                    内置剧本文本与行切分工具
//
// 两种剧本形态：
//   1) 内置文本剧本（冒烟）：4 行 delta + done（assistant_json = 纯 content）。
//   2) turn 剧本（M3 起）：env ITHACA_MOCK_TURNS 指向 JSON 数组文件，每个元素是
//      一条完整的 assistant 消息（可含 tool_calls）；chat() 依序弹出，content 按
//      行流式成 delta，done 携带完整 assistant_json；剧本耗尽 ⇒ "剧本已尽"。
//      agent 的离线确定性回归就靠它。
//
// 片间隔：ITHACA_MOCK_DELAY_MS（默认 120ms）。取消 = 停止当前 turn（不发 done）。
//
// 线程纪律：chat() 在宿主线程被调（有桥），只做"起线程"（标签制：请求态随线程
// 走，多会话槽可并发在途）；回放线程只经
// kit::poster_submit 把事件投回宿主线程，绝不直接碰宿主 API。
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
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vc_llm_mock {

using nlohmann::json;

class MockPlugin {
public:
    static constexpr std::string_view name = "ithaca.llm.mock";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 2, 0);

    nostos_status activate(const nostos::abi::Host& host);
    void deactivate() noexcept;

private:
    // ---- 并发请求注册表（标签制）------------------------

    struct MockWorker {
        std::thread th;
        std::atomic<bool> stopping{false};
    };

    // ---- 表槽位（thunk；登记面 -> src/request.cpp，配置面 -> src/plugin.cpp）----

    static nostos_status chat_thunk(void* self, int request_id, const char* messages_json) noexcept;
    static nostos_status chat_ex_thunk(void* self, int request_id, const char* body_json) noexcept;
    static nostos_status cancel_thunk(void* self, int request_id) noexcept;
    static nostos_status set_model_thunk(void* self, const char* model) noexcept;
    static nostos_status info_thunk(void* self, const char** out_json) noexcept;

    // ---- 入口与配置面（src/plugin.cpp）------------------------------------------

    // provider 热切（vc.llm.set_active）：匹配 => 发布自己；否则撤发布
    void on_set_active_(const std::string& provider);
    void load_script_();
    bool delay_env_();

    // ---- 请求登记面（src/request.cpp）-------------------------------------------

    nostos_status chat_(int request_id, const char* messages_or_body);
    // worker 收尾回收：经 poster 回宿主线程 join + 擦表（自 erase 会析构
    // joinable thread => terminate；与 llm_cloud 同款纪律）
    void finish_request_(int rid);

    // ---- 回放线程与产物回流（src/replay.cpp）------------------------------------

    void replay_(int rid, const std::atomic<bool>& stopping);
    static std::vector<std::string> split_lines(const std::string& s);
    std::string builtin_text_() const;
    void post_delta_(int rid, std::string text);
    void post_done_(int rid, const json& assistant, const json& usage);

    // ---- 状态 --------------------------------------------------------------------

    nostos::abi::Host host_{};
    nostos_poster poster_{};
    vc_api::LlmTable table_{};
    bool published_ = false;  // 是否已发布 vc.llm.v1（provider 热切）
    std::vector<nostos::abi::Host::Subscription> subs_;

    std::map<int, MockWorker> requests_;  // request_id -> 在途请求（回放互不归属）
    std::mutex requests_mu_;

    std::string model_name_ = "mock-1";
    std::atomic<std::size_t> turn_index_{0};  // 剧本游标（并发下原子推进）
    int delay_ms_ = 120;
    bool turns_mode_ = false;
    std::vector<json> turns_;
    std::vector<std::string> script_;
};

}  // namespace vc_llm_mock