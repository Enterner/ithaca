// plugin/llm_cloud · 入口与配置面：activate/deactivate、表装配、set_model/info、
// provider 热切（vc.llm.set_active 订阅）、env 装载、NOSTOS_PLUGIN。
// 请求登记 → src/request.cpp；请求线程/SSE → src/stream.cpp；产物回流 → src/events.cpp。

#include "llm_cloud_plugin.hpp"

#include <nostos/kit/win.hpp>  // utf8_to_wide（env 与热切的宽字符转换）

#include <cstdlib>
#include <set>

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
    // provider 热切：机制收编 kit::active_gate（竞争样板单源）——env 门首发 +
    // vc.llm.set_active 热切，载荷落地（凭据/端点/子档案）在 classify 的 mine 分支
    if (host.acquire_poster(&poster_) != NOSTOS_OK) {
        host.log(4, "ithaca.llm.cloud: 领取 poster 失败（宿主 ABI 过旧或未实现）");
        return NOSTOS_ERR_FAILED;
    }
    auto armed = gate_.arm(
        host, vc_api::llm_set_active,
        [this](const void* p) {
            const auto* e = static_cast<const vc_api::LlmSetActive*>(p);
            if (e->provider == nullptr || std::string_view(e->provider) != "cloud")
                return nostos::kit::active_gate::prefer::other;
            apply_profile_(e->key != nullptr ? std::string(e->key) : "",
                           e->base_url != nullptr ? std::string(e->base_url) : "",
                           e->sub_model != nullptr ? std::string(e->sub_model) : "",
                           e->sub_key != nullptr ? std::string(e->sub_key) : "",
                           e->sub_base_url != nullptr ? std::string(e->sub_base_url) : "");
            return nostos::kit::active_gate::prefer::mine;
        },
        [this] {
            if (host_.publish(vc_api::llm, &table_) != NOSTOS_OK) {
                host_.log(4, "ithaca.llm.cloud: 发布 vc.llm.v1 失败（同名服务已存在？）");
                return false;
            }
            return true;
        },
        [this] { host_.unpublish(vc_api::llm); });
    if (!armed.ok) return NOSTOS_ERR_FAILED;
    subs_.push_back(std::move(armed.sub));
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

// set_active 载荷落地（发布/撤发布机制在 gate_）。
// 子档案：三元组齐全才生效（缺一 = 配置未解析出可派遣的子模型，清空回落主档案）；
// sub_model 为空 = 主模型未配置子模型（默认形态），静默清空。
void CloudPlugin::apply_profile_(const std::string& key, const std::string& base_url,
                                const std::string& sub_model, const std::string& sub_key,
                                const std::string& sub_base_url) {
    {
        std::lock_guard<std::mutex> lock(model_mu_);
        if (!key.empty()) {
            key_ = key;
            key_wide_ = nostos::kit::win::utf8_to_wide(key);
        }
        if (!base_url.empty()) {
            base_utf8_ = base_url;
            base_wide_ = nostos::kit::win::utf8_to_wide(base_url);
        }
        const bool sub_ok =
            !sub_model.empty() && !sub_key.empty() && !sub_base_url.empty();
        if (sub_ok) {
            sub_model_ = sub_model;
            sub_key_ = sub_key;
            sub_key_wide_ = nostos::kit::win::utf8_to_wide(sub_key);
            sub_base_utf8_ = sub_base_url;
            sub_base_wide_ = nostos::kit::win::utf8_to_wide(sub_base_url);
        } else {
            sub_model_.clear();
            sub_key_.clear();
            sub_key_wide_.clear();
            sub_base_utf8_.clear();
            sub_base_wide_.clear();
        }
    }
    if (!sub_model.empty() && sub_key.empty())
        host_.log(3, "ithaca.llm.cloud: 子模型档案不完整（缺 Key）——子 agent 回落主模型");
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
    // 全程持 model_mu_：model_/base_utf8_ 由 set_active/set_model 在 webui 线程
    // 热写，info 的调用方又在 agent 线程（turn 内探窗）——取值、拼包、写
    // info_buf_ 三步必须同锁完成，否则并发 info 交错写共享出参缓冲
    std::lock_guard<std::mutex> lock(p->model_mu_);
    json info{{"provider", "cloud"}, {"model", p->model_}, {"base", p->base_utf8_}};
    // 模型窗口查表（已知 DeepSeek 模型 = 契约级默认窗口，与 webui 建档同值）。
    // 未知模型不报——消费方回落兜底阈值：虚报窗口（报大）会让压缩阈值越过真实
    // 窗口回到爆窗，宁缺勿滥。max_output_tokens 暂不报——各家输出上限口径未
    // 核实，待模型表补全。
    static const std::set<std::string> kKnownModels = {
        "deepseek-chat", "deepseek-reasoner", "deepseek-v4-pro", "deepseek-flash",
    };
    if (kKnownModels.count(p->model_) != 0)
        info["context_window"] = vc_api::kDefaultContextWindow;
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
        base_utf8_ = vc_api::kDefaultCloudBaseUrl;
    }
    base_wide_ = nostos::kit::win::utf8_to_wide(base_utf8_);
    if (const char* v = std::getenv("ITHACA_MODEL"); v != nullptr && *v != '\0') model_ = v;
    else model_ = "deepseek-flash";
    key_wide_ = nostos::kit::win::utf8_to_wide(key_);
}

}  // namespace vc_llm_cloud

NOSTOS_PLUGIN(vc_llm_cloud::CloudPlugin)
