// 附件管线（plugin/webui 的附件切片）。
// /api/upload：上传落盘工作区 .ithaca/uploads/（base64 解码 + 撞名避让，JSON 兼容
// 面——e2e 与小文件）；/api/upload/raw：原始字节直传（大文件通路，上限 1GB——
// base64-JSON 对 1GB 级文件要做 4/3 膨胀 + JSON 序列化副本，内存不可行）；
// attachments_payload_：相对路径数组 → 多模态载荷（图片读字节内联 data_url，
// 非图片仅元数据）；/api/attachment：附件图片字节（前端预览的数据源）。
// 纯逻辑件（base64/mime 折叠/文件名净化）在插件根 attachments.hpp。

#include "webui_plugin.hpp"
#include "../attachments.hpp"  // 本插件根纯逻辑件：base64/mime/文件名净化（单测白盒可达）

#include <sstream>  // std::ostringstream：附件文件整读

namespace vc_webui {

/// 两条上传路由共用的落盘段（defer 闭包体内、宿主线程执行）：工作区就绪检查 +
/// uploads 落盘 + 撞名避让 + {ok,path} 回发。字节在调用前备妥（JSON 路由解码、
/// raw 路由直收）。
void WebuiPlugin::upload_store_(std::uint64_t conn, std::string name, std::string bytes) {
    if (host_workspace_.empty())
        return respond_err_(conn, "没有工作区——请先发送首条消息创建会话再上传附件");
    std::error_code ec;
    const fs::path dir = fs::path(host_workspace_) / ".ithaca" / "uploads";
    fs::create_directories(dir, ec);
    // 撞名加序号（在扩展名前插 " (n)"——下载的文件名习惯，不破坏类型）
    fs::path target = dir / name;
    const std::wstring stem = fs::path(name).stem().wstring();
    const std::wstring ext = fs::path(name).extension().wstring();
    for (int n = 1; fs::exists(target, ec) && n < 1000; ++n)
        target = dir / (stem + L" (" + std::to_wstring(n) + L")" + ext);
    std::ofstream out(target, std::ios::binary);
    if (!out)
        return respond_err_(conn, "附件写盘失败：" + target.string());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) return respond_err_(conn, "附件写盘失败（写中途错误）：" + target.string());
    // 相对路径回前端（/ 分隔——模型与 read 工具同口径）
    std::string rel = ".ithaca/uploads/" + target.filename().string();
    server_->respond(conn, json_resp({{"ok", true}, {"path", rel}}));
}

/// POST /api/upload {name, data(base64)} → 附件落盘到工作区 .ithaca/uploads/，
/// 返回 {ok, path}。直接写盘不经 apply 提案：上传是用户自己的文件进场，不是对
/// 既有内容的修改。select 线程只解析，写盘在宿主线程闭包。
http::Response WebuiPlugin::api_upload_(const http::Request& req) {
    try {
        json in = parse_body_(req.body);
        std::string name = sanitize_upload_name(in.value("name", ""));
        const std::string data = in.value("data", "");
        if (name.empty())
            return json_resp({{"ok", false}, {"error", "附件名为空"}});
        if (name.size() > 120)
            return json_resp({{"ok", false}, {"error", "附件名过长（>120 字节）"}});
        if (data.size() > 28 * 1024 * 1024)   // base64 形态的上限（解码 ≈21MB）
            return json_resp({{"ok", false}, {"error", "附件过大（>20MB）"}});
        return defer_(req, [this, name = std::move(name),
                            data = std::move(data)](std::uint64_t conn) mutable {
            std::string bytes;
            if (!base64_decode(data, bytes))
                return respond_err_(conn, "附件数据不是有效的 base64");
            if (bytes.size() > 20ull * 1024 * 1024)
                return respond_err_(conn, "附件过大（>20MB）");
            upload_store_(conn, std::move(name), std::move(bytes));
        });
    } catch (const std::exception& e) {
        return json_resp({{"ok", false}, {"error", e.what()}});
    }
}

/// POST /api/upload/raw?name=<文件名>，body = 原始字节 → 同 /api/upload 落盘。
/// NOTE: defer 捕获 + 解析层 substr 使内存峰值 ≈ 2-3× 体积——本地回环单用户可
/// 接受；分块落盘需动服务层解析，不值得。
http::Response WebuiPlugin::api_upload_raw_(const http::Request& req) {
    std::string name = sanitize_upload_name(req.query("name"));
    if (name.empty())
        return json_resp({{"ok", false}, {"error", "附件名为空"}});
    if (name.size() > 120)
        return json_resp({{"ok", false}, {"error", "附件名过长（>120 字节）"}});
    if (req.body.size() > kUploadRawMax)
        return json_resp({{"ok", false}, {"error", "附件过大（>1GB）"}}, 413);
    return defer_(req, [this, name = std::move(name),
                        body = req.body](std::uint64_t conn) mutable {
        upload_store_(conn, std::move(name), std::move(body));
    });
}

/// 附件相对路径数组 → 多模态载荷（TurnInput.attachments_json 的 JSON 数组）：
/// path 必须落在 .ithaca/uploads/ 内（防穿越——上传端点的落点即白名单），图片读
/// 字节内联为 data_url，非图片/读取失败仅保留元数据供路径注记。调用点在宿主线程
/// 闭包内（/api/input 与 /api/session/new 的 defer）。
json WebuiPlugin::attachments_payload_(const json& rel_paths) {
    json out = json::array();
    if (!rel_paths.is_array()) return out;
    for (const auto& p : rel_paths) {
        if (!p.is_string()) continue;
        const std::string rel = p.get<std::string>();
        json item = {{"path", rel},
                     {"name", rel.substr(rel.find_last_of('/') + 1)},
                     {"mime", ""},
                     {"data_url", ""}};
        if (rel.rfind(".ithaca/uploads/", 0) == 0 && rel.find("..") == std::string::npos) {
            std::error_code ec;
            const fs::path target = fs::path(host_workspace_) / fs::path(rel);
            std::ifstream in(target, std::ios::binary);
            if (in.good()) {
                std::ostringstream buf;
                buf << in.rdbuf();
                const std::string mime = image_mime_of(target.extension().string());
                item["mime"] = mime;
                if (!mime.empty())
                    item["data_url"] = "data:" + mime + ";base64," + base64_encode(buf.str());
            }
        }
        out.push_back(std::move(item));
    }
    return out;
}

/// GET /api/attachment?path=<相对路径> → 附件图片字节（流式文件体，Content-Type
/// 按扩展名）。发送栏缩略图与对话流图片预览的唯一数据源——transcript 只带展示
/// 元数据（data_url 体量大不落转录），预览必须能按路径取回字节。只放行 uploads
/// 白名单内的图片：其余文件继续走 /api/fs/read 的文本护栏，不在此开二进制口子。
/// host_workspace_ 是宿主线程数据面 ⇒ select 线程只解析 query，取根/归界/补发
/// 全在宿主线程闭包。
http::Response WebuiPlugin::api_attachment_(const http::Request& req) {
    const std::string rel = req.query("path");
    return defer_(req, [this, rel](std::uint64_t conn) {
        if (host_workspace_.empty())
            return respond_err_(conn, "附件图片不存在", 404);
        // 归界（file_ 同款教训）：词法白名单外再做规范化复核——weakly_canonical
        // 兜符号链接与穿越，父目录须恰为 uploads 根（upload 落点无子目录，兄弟
        // 目录的同前缀名也一并排除）
        std::error_code ec;
        const fs::path root = fs::weakly_canonical(
            fs::path(host_workspace_) / ".ithaca" / "uploads", ec);
        const fs::path target =
            ec ? fs::path{}
               : fs::weakly_canonical(fs::path(host_workspace_) / fs::path(rel), ec);
        const std::string mime = ec ? std::string()
                                    : image_mime_of(target.extension().string());
        if (ec || target.parent_path() != root || mime.empty() ||
            !fs::is_regular_file(target, ec))
            return respond_err_(conn, "附件图片不存在", 404);
        http::Response r;
        r.file_path = target.string();
        r.file_length = fs::file_size(target, ec);
        if (ec) return respond_err_(conn, "附件图片不可读", 404);
        r.content_type = mime;
        server_->respond(conn, r);
    });
}

}  // namespace vc_webui
