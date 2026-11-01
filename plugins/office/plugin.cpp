// plugin/office —— 办公文档抽取插件（只读能力域）。
//
// 发布统一 ToolTable（契约 vc_tool.hpp；svc 名 ithaca.tool.office），唯一工具：
//   read —— docx/pptx/xlsx 原生解析（ZIP/DEFLATE/XML 自解）+ pdf/doc/ppt/xls 经
//           系统 IFilter；抽取正文走与 workspace 文本 read 相同的分页管线
//          （read_paged 共享头），出参形状逐字段一致（agent 压缩摘要分支与
//           前端抽取徽标都依赖它）。
// 另发布 PreviewTable（shared/vcoffice/iface.hpp；svc 名 ithaca.preview.office，
// webui 的 /api/fs/preview 消费）：办公文档 → PDF 的版式级预览转换（COM 自动化，
// 见 preview.hpp）——不在 agent 工具面，预览是 UI 能力不是模型能力。
//
// 工具名必须是 read（全名 office.read）：agent 的压缩分派按工具名末段取摘要
// 分支，末段是 read 才命中 read 形态（path/bytes/truncated + recall 提示）。
// jail 根与 workspace 同源同语义（env ITHACA_WORKSPACE_DIR > 当前目录），
// 订阅 vc.workspace_set 热切跟随——两插件对同一路径参数必须解析到同一文件，
// 边界各管各的（重复实现而非共享实例：插件间零运行期耦合）。
// 线程纪律：表调用来自 agent 宿主线程与 webui 分流闭包，tool_execute 以全工具
// 串行锁（tool_mu_）串行化。预览转换（preview.hpp）是唯一的自有线程例外——
// COM 自动化秒级重活不能占调用线程，双线程（转换/看门狗）成对启停于
// activate/deactivate，回调不触插件状态。
#include "vcoffice/iface.hpp"

#include "vc_events.hpp"
#include "vc_tool.hpp"

#include "vcplugin/tool_table.hpp"  // vcplugin：工具表出参契约/发布三连（四插件单源）

#include "docs.hpp"
#include "preview.hpp"
#include <vckit/path_jail.hpp>
#include "vcws/read_paged.hpp"

#include <nostos/abi/plugin.hpp>
#include <vckit/win.hpp>

#include <algorithm>
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
        jail_ = std::make_unique<vckit::PathJail>(
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
        // 预览表（webui /api/fs/preview 的数据源；不在 agent 工具面）：线程先起
        // 后发布——发布失败即停线程，不留无表消费方却跑着转换线程的悬空态
        preview_.start([this](const std::string& line) { host_.log(2, line); });
        nostos::abi::init_table(preview_table_);
        preview_table_.self = this;
        preview_table_.convert = &OfficePlugin::convert_thunk;
        if (host_.publish(vcoffice::kPreviewSvc, &preview_table_) != NOSTOS_OK) {
            preview_.stop();
            host_.log(4, "ithaca.office: 发布 ithaca.preview.office 失败（同名服务已存在？）");
            return NOSTOS_ERR_FAILED;
        }
        vcplugin::notify_tool_register(host_, vcoffice::kPreviewSvcName);
        host_.log(2, "ithaca.office: 就绪，jail 根 = " +
                        vckit::win::wide_to_utf8(jail_->root().c_str()));

        subs_.push_back(host_.on(vc_api::workspace_set, [this](const void* p) {
            const auto* e = static_cast<const vc_api::WorkspaceSet*>(p);
            if (e->root == nullptr || *e->root == '\0') return;
            // 同值热切幂等（backfill 补发形态，与 workspace 同款）：重建边界非零
            // 成本且就绪日志会刷屏
            const std::wstring next = vckit::win::utf8_to_wide(e->root);
            if (jail_ != nullptr && jail_->root() == next) return;
            std::error_code ec;
            fs::create_directories(fs::path(e->root), ec);
            jail_ = std::make_unique<vckit::PathJail>(e->root);
            host_.log(2, "ithaca.office: 工作区已热切为 " + std::string(e->root));
        }));
        return NOSTOS_OK;
    }

    void deactivate() noexcept {
        // 预览引擎先收口：在途 job 杀进程解锁、余量取消回调、双 join——此后不再
        // 有任何回调（消费方的 per-request 状态不受 deactivate 顺序影响）
        preview_.stop();
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

    static nostos_status convert_thunk(void* self, const char* req_json,
                                       vcoffice::PreviewDoneFn on_done, void* ud,
                                       const char** out_json) noexcept {
        return static_cast<OfficePlugin*>(self)->preview_convert(req_json, on_done, ud,
                                                                 out_json);
    }

    // 预览转换入口（调用线程 = webui 宿主排水闭包，天然串行）：jail 校验与
    // 白名单在提交前做完（快速失败不进队列），重活全在 preview_ 的工作线程。
    // NOSTOS_OK 只代表"调用成功"；提交失败以 ok:false JSON 返回且不会有回调。
    // 整体 try/catch：本函数经 noexcept thunk 入场，畸形请求（如 path 非字符串
    // 触发 json type_error）若裸抛 = terminate 整个宿主——tool_execute 同款纪律。
    nostos_status preview_convert(const char* req_json, vcoffice::PreviewDoneFn on_done,
                                  void* ud, const char** out) {
        try {
            std::lock_guard<std::mutex> lock(preview_mu_);
            json args = json::parse(req_json != nullptr ? req_json : "{}", nullptr,
                                    /*allow_exceptions=*/false);
            if (args.is_discarded() || !args.is_object()) args = json::object();
            vckit::PathJail::Resolved r;
            if (!jail_->resolve(args.value("path", ""), r))
                return finish_preview(fail("路径被拒: " + r.err), out);
            const std::string ext_lower =
                vcws::ascii_lower(fs::path(r.norm).extension().string());
            // 版式预览白名单：docx/doc/pptx/ppt/pdf（.pdf 直通）。xlsx 不进——表格
            // 的"打印版式"对预览无增益，维持抽取文本（与前端/agent 的口径一致）
            static constexpr const char* kPreviewExts[] = {".docx", ".doc", ".pptx", ".ppt",
                                                           ".pdf"};
            const bool supported = std::any_of(std::begin(kPreviewExts),
                                               std::end(kPreviewExts),
                                               [&](const char* e) { return e == ext_lower; });
            if (!supported)
                return finish_preview(
                    fail("该格式不支持版式预览（docx/doc/pptx/ppt/pdf 之外维持抽取文本）: " +
                         r.norm),
                    out);
            std::error_code ec;
            if (!fs::is_regular_file(r.full, ec))
                return finish_preview(fail("文件不存在: " + r.norm), out);
            const auto sz = fs::file_size(r.full, ec);
            // 与 read 同一护栏：整文件面（读入/转换）不分页，超限在入口点名
            if (!ec && sz > vcoffice::docs::kArchiveCap)
                return finish_preview(
                    fail("文档过大（" + std::to_string(sz / (1024 * 1024)) + "MB > " +
                         std::to_string(vcoffice::docs::kArchiveCap / (1024 * 1024)) +
                         "MB 上限）: " + r.norm),
                    out);
            vcoffice::Preview::Job job;
            job.root = vckit::win::wide_to_utf8(jail_->root().c_str());
            job.norm = r.norm;
            job.full = r.full;
            job.ext = ext_lower;
            job.done = on_done;
            job.ud = ud;
            std::string resp;
            preview_.submit(std::move(job), resp);   // 两侧同体：未入队=错误即终态无回调；
            return finish_preview(std::move(resp), out);   // 已入队=pending，结果走回调
        } catch (const std::exception& e) {
            return finish_preview(fail(std::string("内部错误: ") + e.what()), out);
        } catch (...) {
            return finish_preview(fail("内部错误（未知异常）"), out);
        }
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
        // 全工具串行锁（tool_mu_）：本表两个调用线程——agent 宿主线程与 webui
        // 分流闭包，出参缓冲 out_buf_ 共享 ⇒ 必须串行
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

    // 预览表出参（与 finish 同款调用期有效契约；独立缓冲——两表出参生命周期
    // 不相交）。双重载与 finish_result 的 json/string 对齐——NOTE: 只留 string
    // 形参时，fail() 的 json 实参会走 json 的隐式转换运算符（运行时 get<string>
    // 对对象值抛 type_error——探针实测崩在宿主 abort），重载决议在这里必须分明
    nostos_status finish_preview(json result, const char** out) {
        return vcplugin::finish_result(preview_out_buf_, std::move(result), out);
    }
    nostos_status finish_preview(std::string resp, const char** out) {
        return vcplugin::finish_result(preview_out_buf_, std::move(resp), out);
    }

    static json fail(const std::string& error) { return json{{"ok", false}, {"error", error}}; }

    json read_tool(const json& args) {
        vckit::PathJail::Resolved r;
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
            return fail("文档过大（" + std::to_string(sz / (1024 * 1024)) + "MB > " +
                        std::to_string(vcoffice::docs::kArchiveCap / (1024 * 1024)) +
                        "MB 上限）: " + r.norm);
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
    std::unique_ptr<vckit::PathJail> jail_;
    std::vector<nostos::abi::Host::Subscription> subs_;  // 工作区热切订阅
    vc_api::ToolTable table_{};
    std::mutex tool_mu_;
    std::string out_buf_;  // execute 出参的存活缓冲（调用期有效纪律）
    // ---- 预览转换（preview.hpp；唯一自有线程面）--------------------------------
    vcoffice::PreviewTable preview_table_{};
    vcoffice::Preview preview_;
    std::mutex preview_mu_;    // convert 入口串行（webui 排水已串行，防御性保底）
    std::string preview_out_buf_;  // convert 即时出参的存活缓冲（调用期有效纪律）
};

}  // namespace

NOSTOS_PLUGIN(OfficePlugin)
