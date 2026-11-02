// hello_mod —— mod SDK 最小示例：发布 ithaca.tool.hello 工具表（hello 工具）。
//
// 用法：sdk\mod\build.cmd example\hello_mod.cpp hello_plugin.dll
//   ⇒ 拷 hello_plugin.dll 进 <宿主 exe>\mods\
//   ⇒ 重启宿主（应用内 设置 → 重启后端）
//   ⇒ agent 工具集即多出 hello（vc.tool.register 通告 → agent 热接入扩名单）
//
// 一个 mod = 一个普通 ithaca 插件 DLL：契约（contract 头）+ 发布三连
// （shared/vcplugin/tool_table.hpp 单源）+ NOSTOS_PLUGIN 入口，与内置插件同构。
#include "vcplugin/tool_table.hpp"

#include <nostos/svc_id.hpp>

#include <string_view>

namespace {

using json = nlohmann::json;

class HelloMod {
public:
    // 插件元数据（nostos 静态断言强制）：名字与版本进宿主装载日志
    static constexpr std::string_view name = "ithaca.hello";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 1, 0);

    nostos_status activate(const nostos::abi::Host& host) {
        host_ = host;
        // 发布三连单源（表装配 → publish → 失败点名）；失败即激活失败显式可见
        if (vcplugin::publish_tool_table(host, table_, this, kSvc, kSvcName,
                                         &HelloMod::list_thunk,
                                         &HelloMod::execute_thunk,
                                         "hello.mod") != NOSTOS_OK)
            return NOSTOS_ERR_FAILED;
        // 热接入通告：agent 订阅 vc.tool.register，收到即把本表加进取表名单
        vcplugin::notify_tool_register(host_, kSvcName);
        host_.log(2, "hello.mod: 就绪（ithaca.tool.hello）");
        return NOSTOS_OK;
    }

    void deactivate() noexcept {}

private:
    static constexpr std::uint64_t kSvc = nostos::svc_id<"ithaca.tool.hello">;
    static constexpr const char* kSvcName = "ithaca.tool.hello";

    // 工具清单（出参仅调用期有效，消费方同步拷贝）：schema 是工具的自述，
    // agent 原文透传 LLM 不解释内容
    static nostos_status list_thunk(void* self, const char** out_json) noexcept {
        auto* p = static_cast<HelloMod*>(self);
        const json tools = json::array({json{
            {"name", "hello"},
            {"description", "返回一句问候（示例 mod，验证发行版自扩展链路）"},
            {"schema", json{{"type", "object"},
                            {"properties", json{{"name", {{"type", "string"}}}}},
                            {"required", json::array({"name"})}}}}});
        p->list_buf_ = json{{"ok", true}, {"tools", tools}}.dump();
        *out_json = p->list_buf_.c_str();
        return NOSTOS_OK;
    }

    static nostos_status execute_thunk(void* self, const char* tool,
                                       const char* args_json, int turn_id,
                                       const char** out_json) noexcept {
        return static_cast<HelloMod*>(self)->execute(tool, args_json, out_json);
    }

    nostos_status execute(const char* tool, const char* args_json, const char** out) {
        json args = json::parse(args_json != nullptr ? args_json : "{}", nullptr, false);
        if (args.is_discarded() || !args.is_object()) args = json::object();
        // 工具级失败 = NOSTOS_OK + ok:false（回灌模型的正常通路）；
        // NOSTOS_ERR_* 只留给基础设施级失败
        if (std::string_view(tool != nullptr ? tool : "") != "hello")
            return vcplugin::finish_result(
                out_buf_, json{{"ok", false}, {"error", "未知工具"}}, out);
        const std::string who = args.value("name", std::string("世界"));
        return vcplugin::finish_result(
            out_buf_, json{{"ok", true},
                           {"text", "你好，" + who + "！（来自 hello mod）"}}, out);
    }

    nostos::abi::Host host_{};
    vc_api::ToolTable table_{};
    std::string list_buf_;   // list 出参缓冲（调用期有效契约的实现侧）
    std::string out_buf_;    // execute 出参缓冲
};

}  // namespace

NOSTOS_PLUGIN(HelloMod)
