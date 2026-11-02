// 工作区代理端点（plugin/webui 的文件树/查看器切片）。
// /api/fs/list（文件树栏目录列举，代理 workspace list）、/api/fs/read（右栏代码
// 查看器，按扩展名分流 workspace/office 的 read）、/api/mcp/status（mcp 插件
// 回发的 server 状态）。全部跑在 select 线程解析 + defer 投宿主线程直调工具表。

#include "webui_plugin.hpp"

#include <vckit/slot_call.hpp>  // vckit::slot_call_json：工具表调用的出参消费纪律单源
#include "vcws/text.hpp"  // vcws::ascii_lower：扩展名折叠（共享件）
#include "vcws/read_paged.hpp"  // vcws::is_document_ext：/api/fs/read 分流的扩展名清单（共享件）

#include <algorithm>  // std::clamp/std::find：depth 钳制、黑名单过滤
#include <cstdlib>    // std::atoi：query depth 解析

namespace vc_webui {

/// GET /api/fs/list?path=<相对路径,空=根>&depth=<1..6> → 文件树栏的目录列举。
/// 代理 workspace 的 list 工具（jail 越界防护/上限/symlink 跳过全在提供方），
/// 本端点只做：黑名单目录过滤（树视图不需要的噪音，agent 的 list 保持无偏）、
/// query 的 %XX 解码。直调工具表——跨线程安全由 workspace 的全工具串行锁保证
/// （调用在宿主线程闭包内）。
http::Response WebuiPlugin::api_fs_list_(const http::Request& req) {
    const std::string rel = req.query("path");
    const std::string depth_s = req.query("depth");
    const json args = json{{"path", rel},
                           {"depth", depth_s.empty() ? 1 : std::clamp(std::atoi(depth_s.c_str()), 1, 6)},
                           {"pattern", req.query("pattern")},
                           // 树视图要逐文件列名,而 list 工具的默认视图是 aggregate
                           // (每目录一条汇总、文件只计数——模型侧骨架视图),对树
                           // 不可用,故钉死 flat,不吃 query 透传。
                           {"mode", "flat"},
                           {"per_dir", std::atoi(req.query("per_dir").c_str())}};
    return defer_(req, [this, args = std::move(args)](std::uint64_t conn) {
        json r = call_tool_json_(tools_, "list", args.dump());
        // 黑名单：一级目录名精确匹配（树视图噪音；agent 的 list 工具不受影响）
        static const char* kHide[] = {".git",           "node_modules", "build",
                                      "dist",           ".ithaca-data", "__pycache__",
                                      ".vscode",        ".zcode"};
        if (r.value("ok", false) && r.contains("entries") && r["entries"].is_array()) {
            json kept = json::array();
            for (auto& e : r["entries"]) {
                if (!e.is_object()) continue;
                const std::string p = e.value("path", "");
                const std::string seg = p.substr(0, p.find('/'));
                if (std::find(std::begin(kHide), std::end(kHide), seg) == std::end(kHide))
                    kept.push_back(std::move(e));
            }
            r["entries"] = std::move(kept);
        }
        server_->respond(conn, json_resp(std::move(r)));
    });
}

/// GET /api/fs/read?path=<相对路径> → 右栏代码查看器的文件内容。
/// 按扩展名分流：文档格式（docx/pptx/xlsx/pdf/doc/ppt/xls）路由到 office 表
///（抽取面所在域，doc 徽标随出参 doc 字段继续工作）；office 未装载回退
/// workspace——文档被其守卫点名指向 office.read。其余代理 workspace 的 read
/// 并原样透传：jail 越界防护、NUL 拒二进制、UTF-8 校验、256KB 封顶（truncated
/// 标记）全在提供方。副作用：无（纯读）。线程纪律同 api_fs_list_（解析在
/// select 线程、工具直调在宿主线程闭包）。
http::Response WebuiPlugin::api_fs_read_(const http::Request& req) {
    const std::string rel = req.query("path");
    const std::string ext_lower = vcws::ascii_lower(fs::path(rel).extension().string());
    // 扩展名清单是共享件（workspace/read_paged.hpp），与 office/workspace 同源
    const bool doc = vcws::is_document_ext(ext_lower);
    // numbered=false 要原文：read 默认行首带行号（模型引用面），查看器把它当
    // 正文渲染会与 Monaco 自绘 gutter 叠成双列行号
    const json args = json{{"path", rel}, {"numbered", false}};
    return defer_(req, [this, args = std::move(args), doc](std::uint64_t conn) {
        const vc_api::ToolTable* table =
            doc && office_tools_ != nullptr ? office_tools_ : tools_;
        server_->respond(conn, json_resp(call_tool_json_(table, "read", args.dump())));
    });
}

// 工具表直调（/api/fs/* 代理共用）：判空 → execute → NOSTOS_OK 判定 →
// JSON parse → 无效兜底。返回值恒为 object；调用须在宿主线程闭包内（table 的
// 读写纪律见 webui_plugin.hpp）
json WebuiPlugin::call_tool_json_(const vc_api::ToolTable* table, const char* tool,
                                  const std::string& args_json) {
    if (table == nullptr)
        return {{"ok", false}, {"error", "工具表不在"}};
    // NOSTOS_OK + {ok:false} = 工具级失败，原样透传给前端
    const auto r = vckit::slot_call_json<json>(args_json, [&](const char* a, const char** out) {
        return table->execute(table->self, tool, a, 0, out);
    });
    return r.ok ? std::move(r.body)
                : json{{"ok", false},
                       {"error", "工具表 execute 失败 (" + std::to_string(static_cast<int>(r.st)) +
                                     ")"}};
}

// ---- 版式预览（/api/fs/preview）------------------------------------------------
// docx/doc/pptx/ppt/pdf → PDF 字节流的版式级预览出口。数据面：office 的
// PreviewTable.convert 只提交（立即返回），重活在 office 的 STA 转换线程；
// 本端点挂起连接，结果经 on_done 回调在转换线程上补发（http 件 respond 原语支持
// 任意线程，连接已断静默丢弃）。
//
// 回调生命周期（deactivate 顺序不定的悬垂防线）：ud 是堆上的 PreviewWait——
// 只有 server 快照 + 连接号 + inflight 指针，零插件成员；office 保证每 job 恰
// 一次回调（成功/失败/停机取消/杀进程后失败四路同口），回调内自毁。页面关闭
// 后回调照常来、respond 静默丢——不占不漏。
namespace {

// 挂起连接的在途记账：闭包 ++（宿主线程）与回调 --（office 转换线程）跨线程，
// atomic。上限对齐 office 队列容量——挂起连接占客户端槽、文件流占 max_streams，
// 不给"排队等转换"开无界口子。
constexpr int kPreviewMaxInflight = 4;

struct PreviewWait {
    std::shared_ptr<http::Server> srv;   // picker/api_provider_models 同款快照纪律
    std::uint64_t conn = 0;
    std::atomic<int>* inflight = nullptr;
};

// 预览流文件的 content_type：按缓存产物扩展名定（直通族保源扩展名、转换族恒 .pdf）。
const char* preview_mime_(const fs::path& p) {
    std::string e = vcws::ascii_lower(p.extension().string());
    if (e == ".png") return "image/png";
    if (e == ".gif") return "image/gif";
    if (e == ".webp") return "image/webp";
    if (e == ".bmp") return "image/bmp";
    if (e == ".jpg" || e == ".jpeg") return "image/jpeg";
    return "application/pdf";
}

// office 转换线程回调（C ABI trampoline）：result_json 只在回调期有效，同步消费。
// 成功 → 预览文件流（no-cache：URL 无版本参数，浏览器缓存会吃掉改版后的新转换）；
// 失败 → JSON {ok:false,error}（前端回落文本抽取）。两路都 200——失败语义是
// 「预览不可用，请看抽取文本」，不是传输层错误。file_path 流式的内存面 = 内核
// 按 chunk 分块读盘，零整读上界。
void preview_on_done(void* ud, const char* result_json) noexcept {
    std::unique_ptr<PreviewWait> w(static_cast<PreviewWait*>(ud));
    if (!w) return;
    if (w->inflight) w->inflight->fetch_sub(1);
    if (!w->srv) return;
    const json r = json::parse(result_json != nullptr ? result_json : "null", nullptr, false);
    http::Response resp;
    resp.status = 200;
    if (r.is_object() && r.value("ok", false) && r.contains("pdf")) {
        std::error_code ec;
        const fs::path pdf = fs::path(r["pdf"].get<std::string>());
        const auto sz = fs::file_size(pdf, ec);
        if (!ec) {
            resp.content_type = preview_mime_(pdf);
            resp.file_path = pdf.string();
            resp.file_length = sz;
            resp.headers.push_back({"cache-control", "no-cache"});
            w->srv->respond(w->conn, resp);
            return;
        }
        resp.body = json{{"ok", false}, {"error", "预览缓存文件不可读"}}.dump();
    } else {
        resp.body = json{{"ok", false},
                         {"error", r.is_object() ? r.value("error", std::string("预览转换失败"))
                                                 : std::string("预览转换失败")}}
                        .dump();
    }
    resp.content_type = "application/json; charset=utf-8";
    w->srv->respond(w->conn, resp);
}

}  // namespace

/// GET /api/fs/preview?path=<相对路径> → 文档的 PDF 版式预览（office 插件转换，
/// 本端点只挂起-补发）。扩展名白名单在入口即断（xlsx 等维持抽取文本，前端据此
/// 走 /api/fs/read）；转换校验（jail/大小）在 office 侧同款复跑——两侧白名单
/// 必须一致，此处挡的是"明显不支持的格式别占挂起连接"。线程纪律：解析在 select
/// 线程、提交在宿主闭包、补发在 office 转换线程（respond 原语任意线程）。
http::Response WebuiPlugin::api_fs_preview_(const http::Request& req) {
    static constexpr const char* kPreviewExts[] = {
        ".docx", ".doc", ".pptx", ".ppt", ".pdf",
        ".jpg",  ".jpeg", ".png",  ".gif", ".webp", ".bmp"};
    const std::string rel = req.query("path");
    const std::string ext_lower = vcws::ascii_lower(fs::path(rel).extension().string());
    const bool supported =
        std::any_of(std::begin(kPreviewExts), std::end(kPreviewExts),
                    [&](const char* e) { return e == ext_lower; });
    if (!supported)
        return json_resp({{"ok", false},
                          {"error", "该格式不支持版式预览（docx/doc/pptx/ppt/pdf 之外"
                                    "维持抽取文本）"}});
    return defer_(req, [this, rel](std::uint64_t conn) {
        if (preview_ == nullptr) {
            respond_err_(conn, "office 插件未装载，无版式预览能力");
            return;
        }
        if (preview_inflight_.load() >= kPreviewMaxInflight) {
            respond_err_(conn, "预览转换排队已满，请稍后再试");
            return;
        }
        auto* w = new PreviewWait{server_, conn, &preview_inflight_};
        w->inflight->fetch_add(1);
        const std::string args = json{{"path", rel}}.dump();
        const char* out_json = nullptr;
        const nostos_status st =
            preview_->convert(preview_->self, args.c_str(), &preview_on_done, w, &out_json);
        json r = st == NOSTOS_OK && out_json != nullptr
                     ? json::parse(out_json, nullptr, false)
                     : json{{"ok", false}};
        // 即时失败（校验/队满/停机）⇒ 未入队 ⇒ 无回调：状态就地回退并补发错误。
        // 成功入队 ⇒ PreviewWait 所有权移交回调（恰一次，自毁）
        if (r.is_object() && r.value("ok", false)) return;
        w->inflight->fetch_sub(1);
        delete w;
        respond_err_(conn, r.is_object() && r.contains("error")
                               ? r["error"].get<std::string>()
                               : "预览提交失败 (" + std::to_string(static_cast<int>(st)) + ")");
    });
}

/// GET /api/mcp/status → mcp 插件回发的 server 状态（home/mcp-status.json，
/// 由 mcp 插件在 activate/reload/server 变化时改写）。文件缺失/损坏 ⇒ 空清单。
/// 数据源：路由 defer 包装（宿主线程闭包内调用）。
json WebuiPlugin::api_mcp_status_() {
    json fallback = {{"ok", true}, {"servers", json::array()}};
    std::error_code ec;
    const fs::path f = home_ / "mcp-status.json";
    if (!fs::exists(f, ec)) return fallback;
    std::ifstream in(f, std::ios::binary);
    const json st = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!st.is_object() || !st.contains("servers") || !st["servers"].is_array())
        return fallback;
    return st;
}

}  // namespace vc_webui
