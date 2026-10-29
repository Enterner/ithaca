// 工作区代理端点（plugin/webui 的文件树/查看器切片）。
// /api/fs/list（文件树栏目录列举，代理 workspace list）、/api/fs/read（右栏代码
// 查看器，按扩展名分流 workspace/office 的 read）、/api/mcp/status（mcp 插件
// 回发的 server 状态）。全部跑在 select 线程解析 + defer 投宿主线程直调工具表。

#include "webui_plugin.hpp"

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
    const std::string rel = query_val(req.target, "path");
    const std::string depth_s = query_val(req.target, "depth");
    const json args = json{{"path", rel},
                           {"depth", depth_s.empty() ? 1 : std::clamp(std::atoi(depth_s.c_str()), 1, 6)},
                           {"pattern", query_val(req.target, "pattern")},
                           // 树视图要逐文件列名,而 list 工具的默认视图是 aggregate
                           // (每目录一条汇总、文件只计数——模型侧骨架视图),对树
                           // 不可用,故钉死 flat,不吃 query 透传。
                           {"mode", "flat"},
                           {"per_dir", std::atoi(query_val(req.target, "per_dir").c_str())}};
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
/// 标记）全在提供方。副作用：无（纯读）。线程纪律同 api_fs_list_（见上）。
http::Response WebuiPlugin::api_fs_read_(const http::Request& req) {
    const std::string rel = query_val(req.target, "path");
    const std::string ext_lower = vcws::ascii_lower(fs::path(rel).extension().string());
    // 扩展名清单是共享件（workspace/read_paged.hpp），与 office/workspace 同源
    const bool doc = vcws::is_document_ext(ext_lower);
    const json args = json{{"path", rel}};
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
    const char* out_json = nullptr;
    // NOSTOS_OK + {ok:false} = 工具级失败，原样透传给前端
    const nostos_status st = table->execute(table->self, tool, args_json.c_str(), 0, &out_json);
    json r = st == NOSTOS_OK && out_json != nullptr
                 ? json::parse(out_json, nullptr, false)
                 : json{{"ok", false}, {"error", "工具表 execute 失败 (" +
                                              std::to_string(static_cast<int>(st)) + ")"}};
    if (r.is_discarded() || !r.is_object())
        r = json{{"ok", false}, {"error", "工具表返回了无效 JSON"}};
    return r;
}

/// GET /api/mcp/status → mcp 插件回发的 server 状态（home/mcp-status.json，
// 由 mcp 插件在 activate/reload/server 变化时改写）。文件缺失/损坏 ⇒ 空清单。
// 数据源：路由 defer 包装（宿主线程闭包内调用）。
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
