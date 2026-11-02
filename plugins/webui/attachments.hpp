#pragma once
// 附件管线的纯逻辑件（插件根私有头：单测白盒可达；数据面/端点在 src/attachments.cpp）。
// base64 编解码（标准字母表 + padding）、图片扩展名 mime 折叠、上传文件名净化
// ——上传端点与多模态载荷共用的字节口径，跨端点复制必然漂移。
#include "vcws/text.hpp"  // vcws::ascii_lower：图片扩展名折叠（共享件）

#include <cstddef>
#include <string>

namespace vc_webui {

// 原始二进制上传体上限（/api/upload/raw）：1GB。前端 MAX_UPLOAD（InputBar.vue）
// 与此镜像——改一处必须同步另一处
inline constexpr std::size_t kUploadRawMax = 1024ull * 1024 * 1024;

// base64 编码（标准字母表 + padding）：多模态附件把上传的图片字节内联为 data_url
inline std::string base64_encode(const std::string& in) {
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
inline std::string image_mime_of(const std::string& ext_in) {
    const std::string e = vcws::ascii_lower(ext_in);
    if (e == ".png") return "image/png";
    if (e == ".jpg" || e == ".jpeg") return "image/jpeg";
    if (e == ".webp") return "image/webp";
    if (e == ".gif") return "image/gif";
    return {};
}

// base64 解码（标准字母表 + padding；含空白容忍——浏览器 FileReader 出的是干净
// 串，这里按宽容口径处理）。失败返回 false。
inline bool base64_decode(const std::string& in, std::string& out) {
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

// 上传文件名净化：只取文件名组件（防路径穿越），去 Windows 非法字符与控制符，
// 剥尾部点/空格（Windows 会吞掉尾点尾空格——落盘名与展示名分叉的来源）。
// 返回可为空串（空 = 端点侧拒绝）；长度护栏（120B）留在端点（错误文案在那层）。
inline std::string sanitize_upload_name(std::string name) {
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    std::string clean;
    for (const unsigned char c : name)
        if (c >= 0x20 && c != '<' && c != '>' && c != ':' && c != '"' &&
            c != '|' && c != '?' && c != '*')
            clean.push_back(static_cast<char>(c));
    while (!clean.empty() && (clean.back() == '.' || clean.back() == ' ')) clean.pop_back();
    return clean;
}

}  // namespace vc_webui
