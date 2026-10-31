// plugin/office —— 办公文档抽取插件（只读能力域）。
//
// 发布统一 ToolTable（契约 vc_tool.hpp；svc 名 ithaca.tool.office），唯一工具：
//   read —— docx/pptx/xlsx 原生解析（ZIP/DEFLATE/XML 自解）+ pdf/doc/ppt/xls 经
//           系统 IFilter；抽取正文走与 workspace 文本 read 相同的分页管线
//          （read_paged 共享头），出参形状逐字段一致（agent 压缩摘要分支与
//           前端抽取徽标都依赖它）。
//
// 工具名必须是 read（全名 office.read）：agent 的压缩分派按工具名末段取摘要
// 分支，末段是 read 才命中 read 形态（path/bytes/truncated + recall 提示）。
// jail 根与 workspace 同源同语义（env ITHACA_WORKSPACE_DIR > 当前目录），
// 订阅 vc.workspace_set 热切跟随——两插件对同一路径参数必须解析到同一文件，
// 边界各管各的（重复实现而非共享实例：插件间零运行期耦合）。
// 线程纪律：无自有线程、无 poster 需求；表调用来自 agent 宿主线程与 webui 分流
// 闭包，tool_execute 以全工具串行锁（tool_mu_）串行化。
#include "vcoffice/iface.hpp"

#include "vc_events.hpp"
#include "vc_tool.hpp"

#include "vcplugin/tool_table.hpp"  // vcplugin：工具表出参契约/发布三连（四插件单源）

#include "docs.hpp"
#include <nostos/kit/path_jail.hpp>
#include "vcws/read_paged.hpp"

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/win.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using nlohmann::json;

class OfficePlugin {
public:
    static constexpr std::string_view name = "vc.office";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 1, 0);

    nostos_status activate(const nostos::abi::Host& host) {
        host_ = host;
        const char* env = std::getenv("ITHACA_WORKSPACE_DIR");
        jail_ = std::make_unique<nostos::kit::PathJail>(
            env != nullptr && *env != '\0' ? env : ".");
        // 发布失败 = 激活失败（对齐 workspace/runner 与 ctx_basic 的裁决：表不在
        // 却报就绪的带病继续没有消费方受益）
        if (vcplugin::publish_tool_table(host_, table_, this, vcoffice::kToolSvc,
                                         vcoffice::kToolSvcName, &OfficePlugin::list_thunk,
                                         &OfficePlugin::execute_thunk,
                                         "ithaca.office") != NOSTOS_OK)
            return NOSTOS_ERR_FAILED;
        // 热接入提示（发现的主通道是 agent 的配置清单；这是给面板/日志看的）
        vcplugin::notify_tool_register(host_, vcoffice::kToolSvcName);
        host_.log(2, "ithaca.office: 就绪，jail 根 = " +
                        nostos::kit::win::wide_to_utf8(jail_->root().c_str()));

        subs_.push_back(host_.on(vc_api::workspace_set, [this](const void* p) {
            const auto* e = static_cast<const vc_api::WorkspaceSet*>(p);
            if (e->root == nullptr || *e->root == '\0') return;
            std::error_code ec;
            fs::create_directories(fs::path(e->root), ec);
            jail_ = std::make_unique<nostos::kit::PathJail>(e->root);
            host_.log(2, "ithaca.office: 工作区已热切为 " + std::string(e->root));
        }));
        return NOSTOS_OK;
    }

    void deactivate() noexcept {
        subs_.clear();
        host_.log(2, "ithaca.office: 已停用");
    }

private:
    // ---- ToolTable 槽位（noexcept thunk → 成员函数）----------------------------

    static nostos_status list_thunk(void* self, const char** out_json) noexcept {
        return static_cast<OfficePlugin*>(self)->tool_list(out_json);
    }
    static nostos_status execute_thunk(void* self, const char* tool, const char* args_json,
                                       int turn_id, const char** out_json) noexcept {
        return static_cast<OfficePlugin*>(self)->tool_execute(tool, args_json, turn_id,
                                                              out_json);
    }

    nostos_status tool_list(const char** out) {
        json schema_path = {{"type", "string"}, {"description", "工作区内相对路径"}};
        // NOTE: properties 必须显式建 json::object——花括号初始化会把 {"k",v} 对
        // 变成数组对，OpenAI 的 JSON Schema 不认（workspace 同款教训）
        auto object_with = [](std::initializer_list<json::value_type> props,
                              json required) {
            json properties = json::object();
            for (const auto& p : props) {
                if (p.is_array() && p.size() == 2 && p[0].is_string())
                    properties[p[0].get<std::string>()] = p[1];
            }
            return json{{"type", "object"}, {"properties", std::move(properties)},
                        {"required", std::move(required)}};
        };
        json tools = json::array(
            {json{{"name", "read"},
                  {"description",
                   "读取办公文档，返回抽取后的纯文本（docx/pptx/xlsx 原生抽取，保留"
                   "分页/表格行结构；pdf/doc/ppt/xls 走系统 IFilter，本机没装对应"
                   "过滤器时点名拒绝）。返回带行号（行首 = 真实行号 + 制表符），"
                   "带 total_lines 与分页参数（offset/limit，同文本 read 口径）"},
                  {"schema", object_with(
                                 {{"path", schema_path},
                                  {"offset", {{"type", "integer"},
                                              {"description", "起始行号（1 起，默认 1）"}}},
                                  {"limit", {{"type", "integer"},
                                             {"description", "本次读取行数（默认/上限 2000）"}}}},
                                 json::array({"path"}))}}});
        return finish(json{{"ok", true}, {"tools", tools}}, out);
    }

    nostos_status tool_execute(const char* tool, const char* args_json, int turn_id,
                               const char** out) {
        // 全工具串行锁：与 workspace 同款双线程纪律（agent 宿主线程 + webui 分流闭包）
        std::lock_guard<std::mutex> lock(tool_mu_);
        json args = json::parse(args_json != nullptr ? args_json : "{}", nullptr,
                                /*allow_exceptions=*/false);
        if (args.is_discarded() || !args.is_object()) args = json::object();
        try {
            const std::string t = tool != nullptr ? tool : "";
            if (t == "read") return finish(read_tool(args), out);
            return finish(fail("未知工具: \"" + t + "\""), out);
        } catch (const std::exception& e) {
            return finish(fail(std::string("内部错误: ") + e.what()), out);
        }
    }

    // 约定与 workspace 一致：NOSTOS_OK 只代表"调用成功"；工具级失败以 ok:false +
    // error 的 JSON 返回（给模型的正常回灌通路）。
    nostos_status finish(json result, const char** out) {
        return vcplugin::finish_result(out_buf_, result, out);
    }

    static json fail(const std::string& error) { return json{{"ok", false}, {"error", error}}; }

    json read_tool(const json& args) {
        nostos::kit::PathJail::Resolved r;
        if (!jail_->resolve(args.value("path", ""), r))
            return fail("路径被拒: " + r.err);
        const std::string ext_lower = vcws::ascii_lower(fs::path(r.norm).extension().string());
        // 本插件只收文档格式：文本文件归 workspace.read（域边界清晰，误指路径
        // 时错误直接教模型换对工具）。扩展名清单是共享件（read_paged.hpp）
        if (!vcws::is_document_ext(ext_lower))
            return fail("office.read 只读办公文档（docx/pptx/xlsx/pdf/doc/ppt/"
                        "xls），文本文件用 workspace.read: " + r.norm);
        std::error_code ec;
        const auto sz = fs::file_size(r.full, ec);
        // 查询失败放行：不存在/权限失败由下方 ifstream 打开失败如实上报（错误
        // 口径单一）；只挡"确实大"的文件——整读 slurp 无分页，护栏必须在读之前
        if (!ec && sz > vcoffice::docs::kArchiveCap)
            return fail("文档过大（" + std::to_string(sz / (1024 * 1024)) +
                        "MB > 64MB 上限）: " + r.norm);
        std::string bytes;
        {
            // 二进制整读（抽取器自己解容器，不走 workspace 的文本 Store）
            std::ifstream in(r.full, std::ios::binary);
            if (!in) return fail("文件不存在或不可读: " + r.norm);
            bytes.assign(std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>());
        }
        vcoffice::docs::DocText d = vcoffice::docs::extract(
            reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
            ext_lower, r.full);
        if (!d.ok) return fail(d.error);
        return vcws::read_paged(std::move(d.text), r.norm, args, d.truncated, d.kind);
    }

    // ---- 状态 ---------------------------------------------------------------------

    nostos::abi::Host host_{};
    std::unique_ptr<nostos::kit::PathJail> jail_;
    std::vector<nostos::abi::Host::Subscription> subs_;  // 工作区热切订阅
    vc_api::ToolTable table_{};
    std::mutex tool_mu_;
    std::string out_buf_;  // execute 出参的存活缓冲（调用期有效纪律）
};

}  // namespace

NOSTOS_PLUGIN(OfficePlugin)
