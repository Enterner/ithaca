// 配置（plugin/webui 的供应商两层配置切片）。
// 共享文件存凭据（providers/Key），实例差异项（当前供应商/模型/权限模式）落
// 本实例覆盖层；load/merge/persist 与 /api/config 读写端点、供应商模型清单
// 拉取（设置面的自动填充动作）同在本文件。

#include "webui_plugin.hpp"

#include "vc_events.hpp"  // vc_api::normalize_permission_mode：权限模式归一（读/写路径）

#include "vcws/text.hpp"  // vcws::ascii_lower：名称去重的大小写折叠（共享件）

#include <nostos/kit/win_http.hpp>  // 一发 HTTP 请求小件 + ERROR_WINHTTP_*（模型清单拉取）

#include <algorithm>  // std::find：供应商/模型名去重（大小写不敏感）

namespace vc_webui {

// 实例差异项键清单（共享/覆盖两层配置的分界）——merge 与 persist 三处共用，
// 单一出处：新增覆盖键漏改任一处即静默失效
static const char* const kOverlayKeys[] = {"current_provider", "model", "provider",
                                           "permission_mode"};

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
// 的实例只读共享文件——结构上无写竞争。
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
    for (const char* k : kOverlayKeys)
        if (d.contains(k)) config_[k] = d[k];
}
bool WebuiPlugin::persist_config_() {
    // 单实例模式 = 原 home/config.json 全量行为不变。共享模式下：providers/
    // Key 等凭据写共享文件，但**实例差异项（当前供应商/模型/权限模式）不进
    // 共享文件**（实例对差异项只读）——否则 A 实例切模型会把 B
    // 实例的重启默认值污染掉；差异项只落本实例覆盖层
    const fs::path f = config_path_();
    // temp + rename 原子替换：trunc 直写中途崩溃 ⇒ 配置截断，load 失败静默丢
    // providers/key（load_config_ 解析失败即 return）
    const fs::path tmp = f.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            // 写失败绝不能静默：否则 key 每次重启都丢，用户只看到"key=未设置"
            host_.log(4, "ithaca.webui: 配置写入失败（" + f.string() +
                             "）——数据目录不可写，用启动器 -DataHome 换位置");
            return false;
        }
        if (config_shared()) {
            json shared = config_;
            for (const char* k : kOverlayKeys)
                shared.erase(k);
            out << shared.dump();
        } else {
            out << config_.dump();
        }
        out.flush();
    }
    std::error_code ec;
    fs::rename(tmp, f, ec);
    if (ec) {
        fs::remove(tmp, ec);
        host_.log(4, "ithaca.webui: 配置替换失败（" + f.string() + "）: " + ec.message());
        return false;
    }
    if (config_shared()) {
        json local = json::object();
        for (const char* k : kOverlayKeys)
            if (config_.contains(k)) local[k] = config_[k];
        const fs::path lf = home_ / "config-local.json";
        const fs::path ltmp = lf.string() + ".tmp";
        {
            std::ofstream lo(ltmp, std::ios::binary | std::ios::trunc);
            if (lo) {
                lo << local.dump();
                lo.flush();
            }
        }
        std::error_code lec;
        fs::rename(ltmp, lf, lec);
        if (lec) fs::remove(ltmp, lec);
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
                                 {"context", vc_api::kDefaultContextWindow},
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

// 供应商数组校验与清洗（api_config_save_ 第一段提纯）：名称非空/≤64/去重（大小
// 写不敏感）、type 只许 cloud（mock 内置不接受外部写入）、模型 id 同规 + context
// ≥1024、至多 32 供应商 × 32 模型；api_key 留空 = 按名保留旧值（key 不出前端，
// 前端回传必为空）；sub_model 引用存在性后置校验（目标供应商须在本次清单内且
// 模型启用——悬空引用运行期静默回落主模型，保存时拦截让用户当场看到）。
// 失败返回 false 且 err 点名。
static bool validate_providers(const json& in, const json& existing, json& cleaned,
                               std::string& err) {
    const auto old_key_of = [&existing](const std::string& name) -> std::string {
        for (const auto& p : existing)
            if (p.is_object() && p.value("name", "") == name)
                return p.value("api_key", "");
        return "";
    };
    std::vector<std::string> seen;
    for (const auto& p : in) {
        if (!p.is_object() || cleaned.size() >= 32) {
            err = "供应商配置非法（名称/模型 ID 重复或为空、上下文 ≥1024、超 32 个等）";
            return false;
        }
        const std::string name = p.contains("name") && p["name"].is_string()
                                     ? p["name"].get<std::string>() : "";
        const std::string type = p.contains("type") && p["type"].is_string()
                                     ? p["type"].get<std::string>() : "cloud";
        if (name.empty() || name.size() > 64 || type != "cloud") {
            err = "供应商配置非法（名称/模型 ID 重复或为空、上下文 ≥1024、超 32 个等）";
            return false;
        }
        const std::string low = vcws::ascii_lower(name);
        if (std::find(seen.begin(), seen.end(), low) != seen.end()) {
            err = "供应商配置非法（名称/模型 ID 重复或为空、上下文 ≥1024、超 32 个等）";
            return false;
        }
        seen.push_back(low);
        json e = {{"name", name}, {"type", type}};
        if (p.contains("builtin") && p["builtin"].is_boolean())
            e["builtin"] = p["builtin"];
        e["base_url"] = p.contains("base_url") && p["base_url"].is_string()
                            ? p["base_url"].get<std::string>() : "";
        std::string key = p.contains("api_key") && p["api_key"].is_string()
                              ? p["api_key"].get<std::string>() : "";
        if (key.empty()) key = old_key_of(name);   // 留空 = 按名保留旧 key
        e["api_key"] = key;
        e["enabled"] = p.value("enabled", false);
        json models = json::array();
        std::vector<std::string> mseen;
        if (p.contains("models") && p["models"].is_array())
            for (const auto& m : p["models"]) {
                if (!m.is_object() || models.size() >= 32) {
                    err = "供应商配置非法（名称/模型 ID 重复或为空、上下文 ≥1024、超 32 个等）";
                    return false;
                }
                const std::string mid = m.contains("id") && m["id"].is_string()
                                            ? m["id"].get<std::string>() : "";
                const int ctx = m.contains("context") && m["context"].is_number_integer()
                                    ? m["context"].get<int>() : 0;
                const std::string mlow = vcws::ascii_lower(mid);
                if (mid.empty() || mid.size() > 64 || ctx < 1024 ||
                    std::find(mseen.begin(), mseen.end(), mlow) != mseen.end()) {
                    err = "供应商配置非法（名称/模型 ID 重复或为空、上下文 ≥1024、超 32 个等）";
                    return false;
                }
                mseen.push_back(mlow);
                // 子 agent 模型（可选）：结构化引用 {"provider","model"}——结构化
                // 而非 "供应商/模型" 拼串，供应商名可含 "/" 不歧义。缺省 = 自己。
                json mm = json{{"id", mid}, {"context", ctx},
                               {"enabled", m.value("enabled", true)}};
                if (m.contains("sub_model") && m["sub_model"].is_object()) {
                    const std::string sp = m["sub_model"].value("provider", "");
                    const std::string sm = m["sub_model"].value("model", "");
                    if (!sp.empty() && !sm.empty())
                        mm["sub_model"] = json{{"provider", sp}, {"model", sm}};
                }
                models.push_back(std::move(mm));
            }
        e["models"] = models;
        cleaned.push_back(e);
    }
    for (const auto& p : cleaned)
        for (const auto& m : p.value("models", json::array())) {
            if (!m.is_object() || !m.contains("sub_model")) continue;
            const json& sm = m["sub_model"];
            bool ok = false;
            for (const auto& q : cleaned) {
                if (!q.is_object() || q.value("name", "") != sm.value("provider", ""))
                    continue;
                for (const auto& qm : q.value("models", json::array()))
                    if (qm.is_object() && qm.value("id", "") == sm.value("model", "")) {
                        ok = qm.value("enabled", true);
                        break;
                    }
                break;
            }
            if (!ok) {
                err = "子 agent 模型引用无效：" + sm.value("provider", "") + " / " +
                      sm.value("model", "") + "（供应商不存在或模型未启用）";
                return false;
            }
        }
    return true;
}

// 清洗结果落位（api_config_save_ 第二段提纯）：current_provider 收敛——前端指定
// 或保留，必须命中清单**且供应商启用（带启用模型）**，停用的供应商继续当当前项
// 会让输入栏仍走它的凭据、停用语义失真（踩过）；不命中 ⇒ 回落第一个可用的。
// provider/model 同步为当前供应商（重启 env 兼容 + 旧面板读法），当前模型不在
// 启用列表 ⇒ 切到第一个启用模型。返回当前供应商的 api_key（cloud 无 key 拦截用）。
static std::string settle_current_provider(json& config, const json& cleaned,
                                           const json& in) {
    const auto usable = [](const json& p) {
        if (!p.is_object() || !p.value("enabled", false)) return false;
        for (const auto& m : p.value("models", json::array()))
            if (m.is_object() && m.value("enabled", false)) return true;
        return false;
    };
    std::string cur = config.value("current_provider", "");
    if (in.contains("current_provider") && in["current_provider"].is_string())
        cur = in["current_provider"].get<std::string>();
    bool cur_ok = false;
    for (const auto& p : cleaned)
        if (p.value("name", "") == cur) { cur_ok = usable(p); break; }
    if (!cur_ok)
        for (const auto& p : cleaned)
            if (usable(p)) { cur = p.value("name", ""); cur_ok = true; break; }
    if (!cur_ok && !cleaned.empty()) cur = cleaned.front().value("name", "");
    config["current_provider"] = cur;
    std::string cur_key;
    for (const auto& p : cleaned) {
        if (p.value("name", "") != cur) continue;
        config["provider"] = p.value("type", "cloud");
        cur_key = p.value("api_key", "");
        bool m_ok = false;
        for (const auto& m : p.value("models", json::array()))
            if (m.value("enabled", false) &&
                m.value("id", "") == config.value("model", "")) { m_ok = true; break; }
        if (!m_ok)
            for (const auto& m : p.value("models", json::array()))
                if (m.value("enabled", false)) { config["model"] = m["id"]; break; }
        break;
    }
    return cur_key;
}

/// POST /api/config → 供应商两层配置校验 + 落盘 + 当前供应商热切。key 只写
/// 不读（响应经 has_key 掩码）。select 线程只解析，校验/合并/落盘/热切全在
/// 宿主线程闭包后补发响应（校验失败同样走补发 200/400）。mock = 测试模式：
/// 供应商/模型修改忽略（mcp/权限模式等其余字段照常保存）。
http::Response WebuiPlugin::api_config_save_(const http::Request& req) {
    json in = parse_body_(req.body);
    return defer_(req, [this, in = std::move(in)](std::uint64_t conn) {
    if (in.contains("providers") && in["providers"].is_array() && llm_mode_ != "mock") {
        json cleaned = json::array();
        std::string err;
        if (!validate_providers(in["providers"],
                                config_.value("providers", json::array()), cleaned, err))
            return respond_err_(conn, err);
        // 替换式合并：cloud 项换成清洗结果，非 cloud 类型项保全（磁盘跨模式数据
        // 不丢）。NOTE: cleaned 只按引用遍历不可 move——落位前 settle 还要遍历它
        json merged = json::array();
        for (const auto& p : config_.value("providers", json::array()))
            if (p.is_object() && p.value("type", "cloud") != "cloud")
                merged.push_back(p);
        for (const auto& c : cleaned) merged.push_back(c);
        config_["providers"] = std::move(merged);
        const std::string cur_key = settle_current_provider(config_, cleaned, in);
        // cloud 无 key ⇒ 必然 FAILED(5)，提前拦截并点名
        if (config_.value("provider", "mock") == "cloud" && cur_key.empty())
            return respond_err_(conn, "当前供应商（" +
                                          config_.value("current_provider", "") +
                                          "）未填写 API Key——请在供应商详情中输入后重试");
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
    // 插件 UI 贡献的值域：plugins.<名> 整节透传（各插件 schema 的表单值）
    bool plugins_changed = false;
    if (in.contains("plugins") && in["plugins"].is_object()) {
        config_["plugins"] = in["plugins"];
        plugins_changed = true;
    }
    // NOTE: mcp/plugins 任一变化即发 McpReload——发射必须排在 persist_config_()
    // 之后：mcp 插件 reload 读磁盘文件，poster 排水抢在落盘前会读到旧清单
    //（踩过：只置位不发射热重载从未生效；抢跑则原样重启旧 server 组）。
    if (mcp_changed || plugins_changed) {
        mcp_reload_pending_ = true;
    }
    if (!persist_config_())
        return respond_err_(conn, "数据目录不可写：" + config_path_().string() +
                                      " —— 重启宿主时加 -DataHome 参数换个位置");
    if (mcp_reload_pending_) {
        mcp_reload_pending_ = false;
        nostos::kit::poster_submit(poster_, [this] {
            const vc_api::McpReload r{};
            host_.emit(vc_api::mcp_reload, &r);   // poster 排水后宿主线程发射
        });
    }
    // 当前使用供应商热切：广播 vc.llm.set_active——即时生效，无需重启。
    // NOTE: mock 模式恒发 mock（同 activate 对齐——跨模式热切会把本模式插件休眠掉）
    post_llm_set_active_();
    host_.log(2, "ithaca.webui: 配置已保存（供应商「" + config_.value("current_provider", "") +
                     "」已热切）");
    server_->respond(conn, json_resp({{"ok", true}, {"note", "已保存；当前供应商即时生效"}}));
    });
}

/// GET /api/ui/registry → 已注册的插件 UI 贡献清单（vc.ui.register 的内存累积）。
/// 前端设置面板据此动态生成各插件的设置节；schema 是"界面长什么样"，值域另见
/// /api/config 的 plugins 节。数据源：路由 defer 包装（宿主线程闭包内调用）。
json WebuiPlugin::api_ui_registry_() {
    json arr = json::array();
    for (const auto& [name, ui] : ui_registry_)
        arr.push_back(json{{"plugin", name},
                           {"label", ui.value("label", name)},
                           {"schema", ui.value("schema", json::array())}});
    return {{"ok", true}, {"plugins", std::move(arr)}};
}

// 组装并投递 vc.llm.set_active（context_window = 当前启用模型的窗口——agent
// 动态压缩阈值的源头；mock 模式不发——跨模式热切会把本模式插件休眠掉，踩过）。
// activate 对齐、配置保存热切、模型热切补发共用本出口（单点维护）。
void WebuiPlugin::post_llm_set_active_() {
    if (llm_mode_ == "mock") return;
    const std::string provider = config_.value("provider", "cloud");
    const std::string key = config_.value("api_key", "");
    const std::string cur = config_.value("current_provider", "");
    std::string cur_key = key, cur_burl;
    int window = 0;
    // 子 agent 模型档案：当前主模型的 sub_model 引用 → 目标供应商凭据，随本事件
    // 尾部字段下发。解析不出（未配置/悬空引用/目标无 Key）= 留空——provider 侧
    // 语义是子请求回落主模型，默认形态即无子档案，不报错。
    std::string sub_model, sub_key, sub_base;
    for (const auto& p : config_.value("providers", json::array()))
        if (p.is_object() && p.value("name", "") == cur) {
            cur_key = p.value("api_key", key);
            cur_burl = p.value("base_url", "");
            for (const auto& m : p.value("models", json::array()))
                if (m.is_object() && m.value("id", "") == config_.value("model", "") &&
                    m.value("enabled", true)) {
                    window = m.value("context", 0);
                    if (m.contains("sub_model") && m["sub_model"].is_object()) {
                        const std::string sp = m["sub_model"].value("provider", "");
                        const std::string sm = m["sub_model"].value("model", "");
                        for (const auto& q : config_.value("providers", json::array()))
                            if (q.is_object() && q.value("name", "") == sp) {
                                const std::string qkey = q.value("api_key", "");
                                if (!sm.empty() && !qkey.empty()) {
                                    sub_model = sm;
                                    sub_key = qkey;
                                    sub_base = q.value("base_url", "");
                                }
                                break;
                            }
                    }
                    break;
                }
            break;
        }
    nostos::kit::poster_submit(poster_, [this, provider, cur_key, cur_burl, window,
                                         sub_model, sub_key, sub_base] {
        const vc_api::LlmSetActive e{
            provider.c_str(), cur_key.c_str(),
            cur_burl.empty() ? nullptr : cur_burl.c_str(), window,
            sub_model.empty() ? nullptr : sub_model.c_str(),
            sub_key.empty() ? nullptr : sub_key.c_str(),
            sub_base.empty() ? nullptr : sub_base.c_str()};
        host_.emit(vc_api::llm_set_active, &e);
        // 工作区归会话管：配置保存不热切工作区
    });
}

// ---- 供应商模型清单拉取（OpenAI 兼容 GET /models）---------------------------

/// POST /api/provider/models {name, base_url, api_key?} → 请求平台的 OpenAI 兼容
/// GET /models，模型 ID 清单经 SSE provider_models 事件回设置面（自动填充模型
/// 表）。key 定位在宿主线程闭包（留空 = 按名取已存——前端掩码拿不到明文），
/// 网络在 detach 线程（外网往返不能占宿主排水）；瞬态事件不落 transcript。
http::Response WebuiPlugin::api_provider_models_(const http::Request& req) {
    json in = parse_body_(req.body);
    const std::string name = in.value("name", "");
    std::string base = in.value("base_url", "");
    std::string key = in.value("api_key", "");
    if (base.empty()) return json_resp({{"ok", false}, {"error", "Base URL 为空"}});
    return defer_(req, [this, name, base = std::move(base),
                        key = std::move(key)](std::uint64_t conn) mutable {
        if (key.empty())
            for (const auto& p : config_.value("providers", json::array()))
                if (p.is_object() && p.value("name", "") == name) {
                    key = p.value("api_key", "");
                    break;
                }
        if (key.empty())
            return server_->respond(conn, json_resp(
                {{"ok", false},
                 {"error", "API Key 为空——先粘贴 key，或该供应商已保存过 key"}}));
        // 停机纪律（见 webui_plugin.hpp 文件头）：detach 线对插件零触碰——spawn
        // 前在宿主线程快照 server_ 值捕获，广播直调快照；deactivate 的 reset 不
        // 与本线程竞态（快照保活，stop 后 broadcast 为安全 no-op）
        std::shared_ptr<http::Server> srv = server_;
        std::thread([srv, base = std::move(base), key = std::move(key)] {
        // base 尾斜杠归一后拼 /models；llm_cloud 同款四段解析口径
        std::string url = base;
        while (!url.empty() && url.back() == '/') url.pop_back();
        url += "/models";
        const std::wstring wurl = win::utf8_to_wide(url);
        const std::wstring wkey = win::utf8_to_wide(key);
        json result = {{"ok", false}, {"error", std::string("请求未发起")}};
        const win::HttpReply reply = win::http_fetch(wurl, {
            .headers = {L"Authorization: Bearer " + wkey},
            // 短超时：清单拉取是设置面动作，用户在等，失败要快回
            .timeouts = {5000, 5000, 10000, 30000},
        });
        if (!reply.sent || reply.status == 0) {
            result["error"] = win::winhttp_error_text(reply.gle);   // 失败码人话（kit 单源）
        } else {
            const json doc = json::parse(reply.body, nullptr,
                                         /*allow_exceptions=*/false);
            if (reply.status == 200 && doc.is_object() && doc.contains("data") &&
                doc["data"].is_array()) {
                json ids = json::array();
                for (const auto& m : doc["data"])
                    if (m.is_object() && m.contains("id") && m["id"].is_string())
                        ids.push_back(m["id"].get<std::string>());
                result = {{"ok", true}, {"models", std::move(ids)}};
            } else {
                // 错误消息人话化：平台 OpenAI 形态 error.message 优先，
                // 原文兜底（截 200 字节防超长错误体刷屏）
                std::string msg;
                if (doc.is_object()) {
                    if (doc.contains("error") && doc["error"].is_object())
                        msg = doc["error"].value("message", "");
                    else if (doc.contains("message") && doc["message"].is_string())
                        msg = doc["message"].get<std::string>();
                }
                if (msg.empty()) msg = vcws::trim_utf8(reply.body, 200);   // 按字节切会劈开 UTF-8 ⇒ dump 抛 316
                result = {{"ok", false},
                          {"error", "HTTP " + std::to_string(reply.status) +
                                        (msg.empty() ? "" : "：" + msg)}};
            }
        }
        json evt = {{"type", "provider_models"}};
        evt.merge_patch(result);
        if (srv) srv->broadcast(evt.dump());   // kit 层线程安全；已停实例 = no-op
    }).detach();
        server_->respond(conn, json_resp({{"ok", true}, {"pending", true}}));
    });
}

}  // namespace vc_webui
