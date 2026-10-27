// plugin/llm_mock · 入口与配置面：activate（表装配/发布门槛/set_active 订阅）、
// deactivate（全停全收）、set_model/info、剧本装载、NOSTOS_PLUGIN。
// 请求登记 -> src/request.cpp；回放与回流 -> src/replay.cpp。

#include "../include/llm_mock_plugin.hpp"

#include <nostos/kit/win.hpp>  // utf8_to_wide（turn 剧本文件路径）

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace vc_llm_mock {

nostos_status MockPlugin::activate(const nostos::abi::Host& host) {
    host_ = host;
    load_script_();

    nostos::abi::init_table(table_);
    table_.self = this;
    table_.chat = &MockPlugin::chat_thunk;
    table_.cancel = &MockPlugin::cancel_thunk;
    table_.info = &MockPlugin::info_thunk;
    table_.chat_ex = &MockPlugin::chat_ex_thunk;  // M3：agent 经 chat_ex 带 tools 提交
    table_.set_model = &MockPlugin::set_model_thunk;  // 热切（mock 仅记录，回放不变）
    // 发布门槛（产品拍板：mock 仅测试模式）——只在 ITHACA_LLM_ACTIVE=mock 时
    // 发布；缺省（cloud）休眠。启动器同时按模式互斥装载两 llm 插件，本判定
    // 只防旧启动路径的双装载竞争（同名服务热切竞态，踩过）
    const char* llm_act = std::getenv("ITHACA_LLM_ACTIVE");
    published_ = (llm_act != nullptr && std::string_view(llm_act) == "mock");
    if (published_) {
        if (host.publish(vc_api::llm, &table_) != NOSTOS_OK) {
            host.log(4, "ithaca.llm.mock: 发布 vc.llm.v1 失败（同名服务已存在？）");
            return NOSTOS_ERR_FAILED;
        }
    } else {
        host.log(2, "ithaca.llm.mock: 非测试模式，休眠（不发布）");
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

void MockPlugin::deactivate() noexcept {
    // 全停 + 全收（llm_cloud 同款）：置取消旗，逐个 join 后清表；回收任务对
    // 已清的 rid 是 no-op（find 不到）
    {
        std::lock_guard<std::mutex> lock(requests_mu_);
        for (auto& [rid, w] : requests_) w.stopping.store(true);
        for (auto& [rid, w] : requests_)
            if (w.th.joinable()) w.th.join();
        requests_.clear();
    }
    host_.log(2, "ithaca.llm.mock: 已停用");
}

// provider 热切（vc.llm.set_active）：匹配 => 发布自己；否则撤发布
void MockPlugin::on_set_active_(const std::string& provider) {
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

void MockPlugin::load_script_() {
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

bool MockPlugin::delay_env_() {
    if (const char* v = std::getenv("ITHACA_MOCK_DELAY_MS")) {
        const int n = std::atoi(v);
        delay_ms_ = n < 0 ? 0 : (n > 2000 ? 2000 : n);
        return true;
    }
    return false;
}

nostos_status MockPlugin::set_model_thunk(void* self, const char* model) noexcept {
    auto* p = static_cast<MockPlugin*>(self);
    p->model_name_ = (model != nullptr ? model : "");
    p->host_.log(2, "ithaca.llm.mock: 模型热切为 " + p->model_name_ + "（回放不变）");
    return NOSTOS_OK;
}

nostos_status MockPlugin::info_thunk(void* self, const char** out_json) noexcept {
    (void)self;
    // 元数据自述：mock 的窗口/输出上限是合成值——让消费方（agent 压缩阈值、批次四
    // 输出预留）走真实链路，而非各自落入兜底分支。窗口取契约级默认（单一出处，
    // 与 webui 建档/llm_cloud 查表同值）。静态串满足出参「调用期有效」契约。
    static const std::string kInfo = [] {
        nlohmann::json j{{"provider", "mock"},
                         {"model", "mock-1"},
                         {"context_window", vc_api::kDefaultContextWindow},
                         {"max_output_tokens", 8192}};
        return j.dump();
    }();
    *out_json = kInfo.c_str();
    return NOSTOS_OK;
}

}  // namespace vc_llm_mock

NOSTOS_PLUGIN(vc_llm_mock::MockPlugin)