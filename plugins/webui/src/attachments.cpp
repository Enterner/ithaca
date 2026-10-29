// 附件管线（plugin/webui 的附件切片）。
// /api/upload：上传落盘工作区 .ithaca/uploads/（base64 解码 + 撞名避让）；
// attachments_payload_：相对路径数组 → 多模态载荷（图片读字节内联 data_url，
// 非图片仅路径注记）；base64 编解码与图片 mime 折叠为本文件私有纯函数。

#include "webui_plugin.hpp"

#include "vcws/text.hpp"  // vcws::ascii_lower：图片扩展名折叠（共享件）

#include <sstream>  // std::ostringstream：附件文件整读

namespace vc_webui {

// base64 编码（标准字母表 + padding）：多模态附件把上传的图片字节内联为 data_url
static std::string base64_encode(const std::string& in) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const unsigned n = static_cast<unsigned>(static_cast<unsigned char>(in[i])) << 16 |
                           (i + 1 < in.size() ? static_cast<unsigned>(static_cast<unsigned char>(in[i + 1])) << 8 : 0u) |
                           (i + 2 < in.size() ? static_cast<unsigned>(static_cast<unsigned char>(in[i + 2])) : 0u);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += i + 1 < in.size() ? kAlphabet[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kAlphabet[n & 63] : '=';
    }
    return out;
}

// 图片扩展名 → data_url 的 mime（非图片返回空：不走多模态，仅路径注记）
static std::string image_mime_of(const std::string& ext_in) {
    const std::string e = vcws::ascii_lower(ext_in);
    if (e == ".png") return "image/png";
    if (e == ".jpg" || e == ".jpeg") return "image/jpeg";
    if (e == ".webp") return "image/webp";
    if (e == ".gif") return "image/gif";
    return {};
}

// base64 解码（标准字母表 + padding；含空白容忍——浏览器 FileReader 出的是干净
// 串，这里按宽容口径处理）。失败返回 false。
static bool base64_decode(const std::string& in, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    out.reserve(in.size() / 4 * 3);
    unsigned buf = 0;
    int bits = 0;
    for (const char c : in) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) {
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
            return false;
        }
        buf = (buf << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return true;
}

/// POST /api/upload {name, data(base64)} → 附件落盘到工作区 .ithaca/uploads/，
/// 返回 {ok, path}。直接写盘不经 apply 提案：上传是用户自己的文件进场，不是对
/// 既有内容的修改。select 线程只解析，写盘在宿主线程闭包。
http::Response WebuiPlugin::api_upload_(const http::Request& req) {
    try {
        json in = parse_body_(req.body);
        std::string name = in.value("name", "");
        const std::string data = in.value("data", "");
        // 文件名净化：只取文件名组件（防路径穿越），去 Windows 非法字符与控制符
        {
            const std::size_t slash = name.find_last_of("/\\");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            std::string clean;
            for (const unsigned char c : name)
                if (c >= 0x20 && c != '<' && c != '>' && c != ':' && c != '"' &&
                    c != '|' && c != '?' && c != '*')
                    clean.push_back(static_cast<char>(c));
            while (!clean.empty() && (clean.back() == '.' || clean.back() == ' ')) clean.pop_back();
            name = clean;
        }
        if (name.empty())
            return json_resp({{"ok", false}, {"error", "附件名为空"}});
        if (name.size() > 120)
            return json_resp({{"ok", false}, {"error", "附件名过长（>120 字节）"}});
        if (data.size() > 28 * 1024 * 1024)   // base64 形态的上限（解码 ≈21MB）
            return json_resp({{"ok", false}, {"error", "附件过大（>20MB）"}});
        return defer_(req, [this, name = std::move(name),
                            data = std::move(data)](std::uint64_t conn) mutable {
            if (host_workspace_.empty())
                return respond_err_(conn, "没有工作区——请先发送首条消息创建会话再上传附件");
            std::string bytes;
            if (!base64_decode(data, bytes))
                return respond_err_(conn, "附件数据不是有效的 base64");
            if (bytes.size() > 20ull * 1024 * 1024)
                return respond_err_(conn, "附件过大（>20MB）");
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
        });
    } catch (const std::exception& e) {
        return json_resp({{"ok", false}, {"error", e.what()}});
    }
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

}  // namespace vc_webui
