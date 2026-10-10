// plugin/llm_mock —— 确定性假 provider（vc.llm.v1 的第二份实现）。
//
// 两种剧本形态：
//   1) 内置文本剧本冒烟）：4 行 delta + done（assistant_json = 纯 content）。
//   2) turn 剧本起）：env ITHACA_MOCK_TURNS 指向 JSON 数组文件，每个元素是
//      一条完整的 assistant 消息（可含 tool_calls）；chat() 依序弹出，content 按
//      行流式成 delta，done 携带完整 assistant_json；剧本耗尽 ⇒ "剧本已尽"。
//      agent 的离线确定性回归就靠它（方案 §8 M3）。
//
// 片间隔：ITHACA_MOCK_DELAY_MS（默认 120ms）。取消 = 停止当前 turn（不发 done）。
//
// 线程纪律：chat() 在宿主线程被调（有桥），只做"起线程"；回放线程只经
// kit::poster_submit 把事件投回宿主线程，绝不直接碰宿主 API。
#include "vc_events.hpp"
#include "vc_llm.hpp"

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/poster.hpp>
#include <nostos/kit/win.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

namespace {

using nlohmann::json;

class MockPlugin {
public:
    static constexpr std::string_view name = "ithaca.llm.mock";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 2, 0);

    nostos_status activate(const nostos::abi::Host& host) {
        host_ = host;
        load_script_();

        nostos::abi::init_table(table_);
        table_.self = this;
        table_.chat = &MockPlugin::chat_thunk;
        table_.cancel = &MockPlugin::cancel_thunk;
        table_.info = &MockPlugin::info_thunk;
        table_.chat_ex = &MockPlugin::chat_ex_thunk;  // M3：agent 经 chat_ex 带 tools 提交
        table_.set_model = &MockPlugin::set_model_thunk;  // 热切（mock 仅记录，回放不变）
        table_.set_model = &MockPlugin::set_model_thunk;  // 热切（mock 仅记录，回放不变）
        // 发布门槛（产品拍板：mock 仅测试模式）——只在 ITHACA_LLM_ACTIVE=mock 时
        // 发布；缺省（cloud）休眠。启动器同时按模式互斥装载两 llm 插件，本判定
        // 只防旧启动路径的双装载竞争（同名服务热切竞态，实锤过）
        const char* llm_act = std::getenv("ITHACA_LLM_ACTIVE");
        published_ = (llm_act != nullptr && std::string_view(llm_act) == "mock");
        if (!published_)
            host.log(2, "ithaca.llm.mock: 非测试模式，休眠（不发布）");
        if (published_) {
            if (host.publish(vc_api::llm, &table_) != NOSTOS_OK) {
                host.log(4, "ithaca.llm.mock: 发布 vc.llm.v1 失败（同名服务已存在？）");
                return NOSTOS_ERR_FAILED;
            }
        } else {
            host.log(2, "ithaca.llm.mock: 休眠（当前 provider 非 mock，可经 webui 热切）");
        }
        if (host.acquire_poster(&poster_) != NOSTOS_OK) {
            host.log(4, "ithaca.llm.mock: 领取 poster 失败（宿主 ABI 过旧或未实现）");
            return NOSTOS_ERR_FAILED;
        }
        subs_.push_back(host_.on(vc_api::llm_set_active, [this](const void* p) {
            const auto* e = static_cast<const vc_api::LlmSetActive*>(p);
            on_set_active_(e->provider != nullptr ? std::string(e->provider) : "");
        }));
        host.log(2, "ithaca.llm.mock: 就绪（" +
                        std::string{turns_mode_ ? "turn 剧本 " + std::to_string(turns_.size()) + " 条"
                                                : "内置文本剧本 " + std::to_string(script_.size()) + " 片"} +
                        "，间隔 " + std::to_string(delay_ms_) + "ms）");
        return NOSTOS_OK;
    }

    void deactivate() noexcept {
        stopping_.store(true);
        if (worker_.joinable()) worker_.join();
        host_.log(2, "ithaca.llm.mock: 已停用");
    }

private:
    // ---- 表槽位 ----------------------------------------------------------------

    static nostos_status chat_thunk(void* self, int request_id, const char* messages_json) noexcept {
        return static_cast<MockPlugin*>(self)->chat_(request_id, messages_json);
    }
    static nostos_status chat_ex_thunk(void* self, int request_id, const char* body_json) noexcept {
        // mock 不消费 tools——等价于普通 chat（agent 的 tools 定义只是透传声明）。
        return static_cast<MockPlugin*>(self)->chat_(request_id, body_json);
    }
    static nostos_status cancel_thunk(void* self, int request_id) noexcept {
        (void)request_id;
        static_cast<MockPlugin*>(self)->stopping_.store(true);
        return NOSTOS_OK;
    }
    static nostos_status set_model_thunk(void* self, const char* model) noexcept {
        auto* p = static_cast<MockPlugin*>(self);
        p->model_name_ = (model != nullptr ? model : "");
        p->host_.log(2, "ithaca.llm.mock: 模型热切为 " + p->model_name_ + "（回放不变）");
        return NOSTOS_OK;
    }

    // provider 热切（vc.llm.set_active）：匹配 ⇒ 发布自己；否则撤发布
    void on_set_active_(const std::string& provider) {
        const bool want = (provider == "mock");
        if (want && !published_) {
            if (host_.publish(vc_api::llm, &table_) == NOSTOS_OK) {
                published_ = true;
                host_.log(2, "ithaca.llm.mock: 已激活（热切）");
            }
        } else if (!want && published_) {
            host_.unpublish(vc_api::llm);
            published_ = false;
            host_.log(2, "ithaca.llm.mock: 已休眠（provider 热切）");
        }
    }

    static nostos_status info_thunk(void* self, const char** out_json) noexcept {
        (void)self;
        *out_json = R"({"provider":"mock","model":"mock-1"})";
        return NOSTOS_OK;
    }

    nostos_status chat_(int request_id, const char* /*messages 或 body，mock 不消费*/) {
        if (worker_.joinable()) worker_.join();
        if (stopping_.load()) return NOSTOS_ERR_BAD_STATE;
        request_id_ = request_id;
        worker_ = std::thread([this] { replay_(); });
        return NOSTOS_OK;
    }

    // ---- 回放线程 ---------------------------------------------------------------

    void replay_() {
        // 模拟深度思考增量（推理显示链路可测；真实推理来自 deepseek-reasoner）
        {
            const std::string think = "（模拟深度思考）正在分析请求并组织回答…";
            nostos::kit::poster_submit(poster_, [this, text = think] {
                const vc_api::LlmReasoning e{request_id_, text.c_str()};
                host_.emit(vc_api::llm_reasoning, &e);
            });
        }
        if (turns_mode_) {
            if (turn_index_ >= turns_.size()) {  // 剧本耗尽
                post_done_(json{{"role", "assistant"}, {"content", "__MOCK_EXHAUSTED__"}},
                           json{{"prompt_tokens", 1}, {"completion_tokens", 1}, {"total_tokens", 2}});
                return;
            }
            const std::size_t idx = turn_index_++;
            const json turn = turns_[idx];
            const std::string content = turn.value("content", std::string{});
            if (stopping_.load(std::memory_order_relaxed)) return;
            for (std::size_t i = 0; i < content.size(); ++i) {
                const std::size_t nl = content.find('\n', i);
                const std::size_t end = nl == std::string::npos ? content.size() : nl + 1;
                post_delta_(content.substr(i, end - i));
                i = end - 1;
                if (stopping_.load(std::memory_order_relaxed)) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
            }
            if (!stopping_.load(std::memory_order_relaxed)) {
                // 用量：剧本可携带 "usage"；缺省按轮次合成（确定性——回归 golden 依赖）
                const std::size_t one = idx + 1;
                json usage = turn.contains("usage")
                                 ? turn["usage"]
                                 : json{{"prompt_tokens", 100 * static_cast<int>(one)},
                                        {"completion_tokens", 10 * static_cast<int>(one)},
                                        {"total_tokens", 110 * static_cast<int>(one)}};
                post_done_(turn, usage);
            }
            return;
        }

        // 内置文本剧本形态）
        for (const std::string& line : script_) {
            if (stopping_.load(std::memory_order_relaxed)) return;
            post_delta_(line);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }
        if (!stopping_.load(std::memory_order_relaxed))
            post_done_(json{{"role", "assistant"}, {"content", builtin_text_()}},
                       json{{"prompt_tokens", 40},
                            {"completion_tokens", 10},
                            {"total_tokens", 50}});
    }

    static std::vector<std::string> split_lines(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (const char c : s) {
            if (c == '\n') { out.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) out.push_back(std::move(cur));
        return out;
    }

    std::string builtin_text_() const {
        std::string all;
        for (const auto& l : script_) all += l;
        return all;
    }

    // ---- 事件回流（kit::poster_submit；载荷所有权/失败回收都在助手内）-----------

    void post_delta_(std::string text) {
        nostos::kit::poster_submit(poster_, [this, text = std::move(text)] {
            const vc_api::LlmDelta e{request_id_, text.c_str()};
            host_.emit(vc_api::llm_delta, &e);
        });
    }
    void post_done_(const json& assistant, const json& usage) {
        const std::string assistant_json = assistant.dump();
        const std::string usage_json = usage.dump();
        nostos::kit::poster_submit(poster_, [this, s = assistant_json, u = usage_json] {
            const vc_api::LlmDone e{request_id_, s.c_str(), u.c_str()};
            host_.emit(vc_api::llm_done, &e);
        });
    }

    // ---- 剧本加载 ----------------------------------------------------------------

    void load_script_() {
        script_.assign({
            "mock 第 1 片：连接建立",
            "mock 第 2 片：分析输入",
            "mock 第 3 片：组织回答",
            "mock 第 4 片：回放完成",
        });
        delay_env_();

        if (const char* path = std::getenv("ITHACA_MOCK_TURNS")) {
            std::ifstream in(nostos::kit::win::utf8_to_wide(path));
            json doc = json::parse(in, nullptr, /*allow_exceptions=*/false);
            if (!doc.is_discarded() && doc.is_array() && !doc.empty()) {
                turns_ = doc.get<std::vector<json>>();
                turns_mode_ = true;
            } else {
                host_.log(3, "ithaca.llm.mock: ITHACA_MOCK_TURNS 无法解析为非空数组——退回内置剧本");
            }
        }
    }

    bool delay_env_() {
        if (const char* v = std::getenv("ITHACA_MOCK_DELAY_MS")) {
            const int n = std::atoi(v);
            delay_ms_ = n < 0 ? 0 : (n > 2000 ? 2000 : n);
            return true;
        }
        return false;
    }

    // ---- 状态 ---------------------------------------------------------------------

    nostos::abi::Host host_{};
    nostos_poster poster_{};
    vc_api::LlmTable table_{};
    bool published_ = false;   // 是否已发布 vc.llm.v1（provider 热切）
    std::vector<nostos::abi::Host::Subscription> subs_;
    std::string model_name_ = "mock-1";
    std::thread worker_{};
    std::atomic<bool> stopping_{false};
    int request_id_ = 0;
    int delay_ms_ = 120;
    bool turns_mode_ = false;
    std::size_t turn_index_ = 0;
    std::vector<json> turns_;
    std::vector<std::string> script_;
};

}  // namespace

NOSTOS_PLUGIN(MockPlugin)
