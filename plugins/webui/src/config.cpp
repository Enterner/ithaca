// 配置（plugin/webui 的供应商两层配置切片）。
// 共享文件存凭据（providers/Key），实例差异项（当前供应商/模型/权限模式）落
// 本实例覆盖层；load/merge/persist 与 /api/config 读写端点同在本文件。

#include "webui_plugin.hpp"

#include "vc_events.hpp"  // vc_api::normalize_permission_mode：权限模式归一（读/写路径）

#include <algorithm>  // std::find：供应商/模型名去重（大小写不敏感）

#include <cctype>  // tolower：名称归一小写

namespace vc_webui {

// ---- 配置 -------------------------------------------------------------------

void WebuiPlugin::load_config_() {
    std::error_code ec;
    const fs::path f = config_path_();
    if (!fs::exists(f, ec)) return;
    std::ifstream in(f, std::ios::binary);
    json d = json::parse(in, nullptr, false);
    if (!d.is_object()) return;
    config_ = d;
    merge_local_overlay_();
}

// ---- 配置两层化（多实例：凭据共享 + 实例覆盖层）---------------------------
// 共享文件（ITHACA_WEBUI_CONFIG，未设 = home/config.json 即单实例原行为）存
// providers/Key 等凭据；实例差异项（当前供应商/模型/权限模式）落 home 的
// config-local.json 覆盖层。唯一写者是设置面板 UI（用户驱动、低频），运行中
// 的实例只读共享文件——结构上无写竞争（多宿主方案 §3.1）。
fs::path WebuiPlugin::config_path_() const {
    if (const char* v = std::getenv("ITHACA_WEBUI_CONFIG"); v != nullptr && *v != '\0')
        return fs::path(v);
    return home_ / "config.json";
}
bool WebuiPlugin::config_shared() const noexcept {
    const char* v = std::getenv("ITHACA_WEBUI_CONFIG");
    return v != nullptr && *v != '\0';
}
void WebuiPlugin::merge_local_overlay_() {
    if (!config_shared()) return;
    std::error_code ec;
    const fs::path f = home_ / "config-local.json";
    if (!fs::exists(f, ec)) return;
    std::ifstream in(f, std::ios::binary);
    json d = json::parse(in, nullptr, false);
    if (!d.is_object()) return;
    for (const char* k : {"current_provider", "model", "provider", "permission_mode"})
        if (d.contains(k)) config_[k] = d[k];
}
bool WebuiPlugin::persist_config_() {
    // 单实例模式 = 原 home/config.json 全量行为不变。共享模式下：providers/
    // Key 等凭据写共享文件，但**实例差异项（当前供应商/模型/权限模式）不进
    // 共享文件**——方案 §3.1「实例对差异项只读」，否则 A 实例切模型会把 B
    // 实例的重启默认值污染掉；差异项只落本实例覆盖层
    const fs::path f = config_path_();
    std::ofstream out(f, std::ios::binary | std::ios::trunc);
    if (!out) {
        // 写失败绝不能静默：否则 key 每次重启都丢，用户只看到"key=未设置"
        host_.log(4, "ithaca.webui: 配置写入失败（" + f.string() +
                         "）——数据目录不可写，用启动器 -DataHome 换位置");
        return false;
    }
    if (config_shared()) {
        json shared = config_;
        for (const char* k : {"current_provider", "model", "provider", "permission_mode"})
            shared.erase(k);
        out << shared.dump();
        json local = json::object();
        for (const char* k : {"current_provider", "model", "provider", "permission_mode"})
            if (config_.contains(k)) local[k] = config_[k];
        std::ofstream lo(home_ / "config-local.json", std::ios::binary | std::ios::trunc);
        if (lo) lo << local.dump();
    } else {
        out << config_.dump();
    }
    return true;
}

json WebuiPlugin::config_public_() {
    // 供应商两层配置：providers 按模式单侧下发（mock=测试内置合成项；cloud=
    // 用户配置项）——面板/模型下拉只显示当前模式一项（产品拍板）；api_key
    // 掩码（不出前端，只给 has_key 供"已设置/未设置"显示）。
    const bool mock = llm_mode_ == "mock";
    json providers = json::array();
    std::string current;
    for (const auto& p : config_.value("providers", json::array())) {
        if (!p.is_object()) continue;
        const std::string type = p.value("type", "cloud");
        if (mock ? type != "mock" : type != "cloud") continue;
        json q = p;
        q.erase("api_key");
        q["has_key"] = !p.value("api_key", "").empty();
        providers.push_back(q);
    }
    if (mock) {   // mock 内置合成：不依赖 config（测试模式不持久化供应商数据）
        if (providers.empty())
            providers.push_back(json{{"name", "mock"},
                                     {"type", "mock"},
                                     {"base_url", ""},
                                     {"enabled", true},
                                     {"has_key", false},
                                     {"models", json::array({json{{"id", "mock-model"},
                                                                  {"context", 128000},
                                                                  {"enabled", true}}})}});
        current = "mock";
    } else if (!providers.empty()) {
        current = config_.value("current_provider", "");
        bool known = false;
        for (const auto& p : providers)
            if (p.value("name", "") == current) { known = true; break; }
        if (!known) current = providers[0].value("name", "");   // 兜底：钳在模式内
    }
    return {{"provider", mock ? std::string("mock")
                              : config_.value("provider", "cloud")},
            {"model", config_.value("model", "deepseek-flash")},
            {"workspace", config_.value("workspace", "")},
            {"port", config_.value("port", 8790)},
            {"permission_mode",
             vc_api::normalize_permission_mode(config_.value("permission_mode", "auto-edit"))},
            {"providers", providers},
            {"current_provider", current},
            {"mcp", config_.value("mcp", json::object())},
            {"plugins", config_.value("plugins", json::object())}};   // 各插件 schema 值域
}

/// POST /api/config → 供应商两层配置校验 + 落盘 + 当前供应商热切。key 只写
/// 不读（响应经 has_key 掩码；请求留空 = 按名保留旧值）。
http::Response WebuiPlugin::api_config_save_(const std::string& body) {
    json in = parse_body_(body);
    // 供应商两层配置：{"providers":[{name,type,base_url,api_key,enabled,
    // models:[{id,context,enabled}...]}], "current_provider":"名称"}——
    // 校验：供应商名非空/≤64/去重（大小写不敏感）、type 只许 mock|cloud、
    // 模型 id 同规 + context ≥1024、至多 32 供应商 × 32 模型。
    // api_key 留空/缺失 = 按名保留旧值（key 不出前端，前端回传必为空）。
    // mock = 测试模式：供应商内置合成、不持久化——面板的供应商/模型修改忽略
    // （mcp/权限模式等其余字段照常保存）；cloud 模式替换式合并（只动 cloud 项，
    // 保留其他类型项——磁盘数据跨模式保全）
    if (in.contains("providers") && in["providers"].is_array() && llm_mode_ != "mock") {
        const json existing = config_.value("providers", json::array());
        const auto old_key_of = [&existing](const std::string& name) -> std::string {
            for (const auto& p : existing)
                if (p.is_object() && p.value("name", "") == name)
                    return p.value("api_key", "");
            return "";
        };
        json cleaned = json::array();
        std::vector<std::string> seen;
        bool bad = false;
        for (const auto& p : in["providers"]) {
            if (!p.is_object() || cleaned.size() >= 32) { bad = true; break; }
            const std::string name = p.contains("name") && p["name"].is_string()
                                         ? p["name"].get<std::string>() : "";
            const std::string type = p.contains("type") && p["type"].is_string()
                                         ? p["type"].get<std::string>() : "cloud";
            if (name.empty() || name.size() > 64 || type != "cloud") {
                bad = true; break;   // 面板单侧显示（产品拍板）：mock 内置不接受外部写入
            }
            std::string low = name;
            for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (std::find(seen.begin(), seen.end(), low) != seen.end()) { bad = true; break; }
            seen.push_back(low);
            json e = {{"name", name}, {"type", type}};
            if (p.contains("builtin") && p["builtin"].is_boolean())
                e["builtin"] = p["builtin"];
            if (type == "cloud") {
                e["base_url"] = p.contains("base_url") && p["base_url"].is_string()
                                    ? p["base_url"].get<std::string>() : "";
                std::string key = p.contains("api_key") && p["api_key"].is_string()
                                      ? p["api_key"].get<std::string>() : "";
                if (key.empty()) key = old_key_of(name);   // 留空 = 按名保留旧 key
                e["api_key"] = key;
            }
            e["enabled"] = p.value("enabled", false);
            json models = json::array();
            std::vector<std::string> mseen;
            if (p.contains("models") && p["models"].is_array())
                for (const auto& m : p["models"]) {
                    if (!m.is_object() || models.size() >= 32) { bad = true; break; }
                    const std::string mid = m.contains("id") && m["id"].is_string()
                                                ? m["id"].get<std::string>() : "";
                    const int ctx = m.contains("context") && m["context"].is_number_integer()
                                        ? m["context"].get<int>() : 0;
                    std::string mlow = mid;
                    for (char& c : mlow)
                        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    if (mid.empty() || mid.size() > 64 || ctx < 1024 ||
                        std::find(mseen.begin(), mseen.end(), mlow) != mseen.end()) {
                        bad = true; break;
                    }
                    mseen.push_back(mlow);
                    models.push_back(json{{"id", mid}, {"context", ctx},
                                          {"enabled", m.value("enabled", true)}});
                }
            if (bad) break;
            e["models"] = models;
            cleaned.push_back(e);
        }
        if (bad)
            return json_resp({{"ok", false},
                              {"error", "供应商配置非法（名称/模型 ID 重复或为空、"
                                        "上下文 ≥1024、超 32 个等）"}});
        // current_provider：前端指定或保留；必须命中列表，否则回落第一个
        std::string cur = config_.value("current_provider", "");
        if (in.contains("current_provider") && in["current_provider"].is_string())
            cur = in["current_provider"].get<std::string>();
        bool cur_ok = false;
        for (const auto& p : cleaned)
            if (p.value("name", "") == cur) { cur_ok = true; break; }
        if (!cur_ok && !cleaned.empty()) cur = cleaned.front().value("name", "");
        {
            json merged = json::array();
            for (const auto& p : config_.value("providers", json::array()))
                if (p.is_object() && p.value("type", "cloud") != "cloud")
                    merged.push_back(p);   // 非 cloud 类型项保全（磁盘跨模式数据不丢）
            // NOTE: 不可 move——合并块之后的 provider/model 同步循环还要遍历
            // cleaned，move 会把元素搬成 null 令该循环静默失效 + 响应路径抛
            // type_error（500，实锤）
            for (const auto& c : cleaned) merged.push_back(c);
            config_["providers"] = std::move(merged);
        }
        config_["current_provider"] = cur;
        // config.provider/model 同步为当前供应商（重启 env 兼容 + 旧面板读法）
        std::string cur_key, cur_burl;
        for (const auto& p : cleaned) {
            if (p.value("name", "") != cur) continue;
            config_["provider"] = p.value("type", "cloud");
            cur_key = p.value("api_key", "");
            cur_burl = p.value("base_url", "");
            // 当前模型不在该供应商启用列表 ⇒ 切到第一个启用模型
            bool m_ok = false;
            for (const auto& m : p.value("models", json::array()))
                if (m.value("enabled", false) &&
                    m.value("id", "") == config_.value("model", "")) { m_ok = true; break; }
            if (!m_ok)
                for (const auto& m : p.value("models", json::array()))
                    if (m.value("enabled", false)) { config_["model"] = m["id"]; break; }
            break;
        }
        // cloud 无 key ⇒ 必然 FAILED(5)，提前拦截并点名
        if (config_.value("provider", "mock") == "cloud" && cur_key.empty())
            return json_resp({{"ok", false},
                              {"error", "当前供应商（" + cur + "）未填写 API Key——"
                                        "请在供应商详情中输入后重试"}});
    }
    if (in.contains("key") && in["key"].is_string() && !in["key"].get<std::string>().empty())
        config_["api_key"] = in["key"];
    for (const char* k : {"provider", "model", "base_url", "workspace"})
        if (in.contains(k) && in[k].is_string()) config_[k] = in[k];
    // MCP server 清单（设置面板 MCP 工具页）：整对象透传，保存后通知 mcp 插件热重载
    bool mcp_changed = false;
    if (in.contains("mcp") && in["mcp"].is_object()) {
        config_["mcp"] = in["mcp"];
        mcp_changed = true;
    }
    // 插件 UI 贡献的值域（方案 A）：plugins.<名> 整节透传（各插件 schema 的表单
    // 值，插件启动/reload 时读取）。
    bool plugins_changed = false;
    if (in.contains("plugins") && in["plugins"].is_object()) {
        config_["plugins"] = in["plugins"];
        plugins_changed = true;
    }
    // NOTE: McpReload 此前只有 mcp_changed 置位却从未发射（死机制，实锤：设置面
    // 板保存 server 清单后热重载从未生效）——接活：mcp/plugins 任一变化即发，
    // mcp 插件收到后重读 config（超时类值随 reload 生效）。
    if (mcp_changed || plugins_changed) {
        nostos::kit::poster_submit(poster_, [this] {
            const vc_api::McpReload r{};
            host_.emit(vc_api::mcp_reload, &r);   // poster 排水后宿主线程发射
        });
    }
    if (!persist_config_()) {
        const std::string cfg_path = config_path_().string();
        return json_resp({{"ok", false},
                          {"error", "数据目录不可写：" + cfg_path +
                                        " —— 重启宿主时加 -DataHome 参数换个位置"}});
    }
    // 当前使用供应商热切：广播 vc.llm.set_active（provider 名=type、key、
    // base_url——尾部追加）——即时生效，无需重启。
    // NOTE: mock 模式恒发 mock（同 activate 对齐——config 的 provider 可能是
    // 另一模式遗留值，跨模式热切会把本模式插件休眠掉）
    const std::string provider = llm_mode_ == "mock"
                                     ? std::string("mock")
                                     : config_.value("provider", "mock");
    const std::string key = config_.value("api_key", "");
    const std::string cur = config_.value("current_provider", "");
    std::string cur_key = key, cur_burl;
    for (const auto& p : config_.value("providers", json::array()))
        if (p.is_object() && p.value("name", "") == cur) {
            cur_key = p.value("api_key", key);
            cur_burl = p.value("base_url", "");
            break;
        }
    nostos::kit::poster_submit(poster_, [this, provider, cur_key, cur_burl] {
        const vc_api::LlmSetActive la{provider.c_str(), cur_key.c_str(),
                                      cur_burl.empty() ? nullptr : cur_burl.c_str()};
        host_.emit(vc_api::llm_set_active, &la);
        // 工作区归会话管：配置保存不热切工作区
    });
    host_.log(2, "ithaca.webui: 配置已保存（供应商「" + config_.value("current_provider", "") +
                     "」已热切）");
    return json_resp({{"ok", true}, {"note", "已保存；当前供应商即时生效"}});
}

/// GET /api/ui/registry → 已注册的插件 UI 贡献清单（vc.ui.register 的内存累积）。
/// 前端设置面板据此动态生成各插件的设置节；schema 是"界面长什么样"，值域另见
/// /api/config 的 plugins 节。
http::Response WebuiPlugin::api_ui_registry_() {
    json arr = json::array();
    for (const auto& [name, ui] : ui_registry_)
        arr.push_back(json{{"plugin", name},
                           {"label", ui.value("label", name)},
                           {"schema", ui.value("schema", json::array())}});
    return json_resp({{"ok", true}, {"plugins", std::move(arr)}});
}

}  // namespace vc_webui
