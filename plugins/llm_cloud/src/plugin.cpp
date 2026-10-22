// plugin/llm_cloud · 入口与配置面：activate/deactivate、表装配、set_model/info、
// provider 热切（vc.llm.set_active 订阅）、env 装载、NOSTOS_PLUGIN。
// 请求登记 → src/request.cpp；请求线程/SSE → src/stream.cpp；产物回流 → src/events.cpp。

#include "../include/llm_cloud_plugin.hpp"

#include <nostos/kit/win.hpp>  // utf8_to_wide（env 与热切的宽字符转换）

#include <cstdlib>

namespace vc_llm_cloud {

nostos_status CloudPlugin::activate(const nostos::abi::Host& host) {
    host_ = host;
    load_env_();

    nostos::abi::init_table(table_);
    table_.self = this;
    table_.chat = &CloudPlugin::chat_thunk;
    table_.cancel = &CloudPlugin::cancel_thunk;
    table_.info = &CloudPlugin::info_thunk;
    table_.chat_ex = &CloudPlugin::chat_ex_thunk;
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

void CloudPlugin::deactivate() noexcept {
    subs_.clear();
    // 全停 + 全收（runner 同款"无悬空"纪律）：置取消旗 + 关句柄打断阻塞读写，
    // 逐个 join 后清表。注册表回收任务对已清的 rid 是 no-op（find 不到）。
    {
        std::lock_guard<std::mutex> lock(requests_mu_);
        for (auto& [rid, w] : requests_) {
            w.stopping.store(true);
            if (HINTERNET h = w.active.exchange(nullptr)) WinHttpCloseHandle(h);
        }
        for (auto& [rid, w] : requests_)
            if (w.th.joinable()) w.th.join();
        requests_.clear();
    }
    host_.log(2, "ithaca.llm.cloud: 已停用");
}

// provider 热切（vc.llm.set_active）：匹配 ⇒ 发布自己；否则撤发布。
void CloudPlugin::on_set_active_(const std::string& provider, const std::string& key,
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

nostos_status CloudPlugin::set_model_(const char* model) {
    if (model == nullptr || *model == '\0') return NOSTOS_ERR_INVALID_ARG;
    { std::lock_guard<std::mutex> lock(model_mu_); model_ = model; }
    host_.log(2, "ithaca.llm.cloud: 模型热切为 " + std::string(model));
    return NOSTOS_OK;
}

nostos_status CloudPlugin::set_model_thunk(void* self, const char* model) noexcept {
    return static_cast<CloudPlugin*>(self)->set_model_(model);
}

nostos_status CloudPlugin::info_thunk(void* self, const char** out_json) noexcept {
    auto* p = static_cast<CloudPlugin*>(self);
    // model_/base_utf8_ 由 set_active/set_model 在 webui 线程热写——info 可能从
    // 宿主线程并发读，取值必须锁内拷快照
    std::string model, base;
    { std::lock_guard<std::mutex> lock(p->model_mu_); model = p->model_; base = p->base_utf8_; }
    json info{{"provider", "cloud"}, {"model", model}, {"base", base}};
    // 模型窗口查表（已知 DeepSeek 模型 128K）。未知模型不报——消费方回落兜底阈值：
    // 虚报窗口（报大）会让压缩阈值越过真实窗口回到爆窗，宁缺勿滥。
    // max_output_tokens 暂不报——各家输出上限口径未核实，待模型表补全。
    static const std::map<std::string, long long> kContextWindows = {
        {"deepseek-chat", 128000},
        {"deepseek-reasoner", 128000},
        {"deepseek-v4-pro", 128000},
        {"deepseek-flash", 128000},
    };
    if (const auto it = kContextWindows.find(model); it != kContextWindows.end())
        info["context_window"] = it->second;
    p->info_buf_ = info.dump();
    *out_json = p->info_buf_.c_str();
    return NOSTOS_OK;
}

void CloudPlugin::load_env_() {
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

}  // namespace vc_llm_cloud

NOSTOS_PLUGIN(vc_llm_cloud::CloudPlugin)
