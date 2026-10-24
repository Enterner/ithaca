#pragma once
// ithaca —— 路径 jail：把模型给出的路径钉死在工作区根内。
//
// 两道闸，缺一不可：
//   1) 文本校验：拒绝 ".." 段、Windows 保留设备名、段尾的点与空格（Windows 会
//      静默剥离——请求 "a." 实际落盘 "a"，名实不符）、控制字符；空段折叠，"." 段
//      丢弃。绝对路径（盘符 / 根斜杠）不再一刀切拒绝——在根内的转成相对路径
//      放行（模型常把工具输出里的绝对形态原样送回来，拒掉只会逼它无谓重试），
//      根外按越界拒绝，边界本身不松。
//   2) 规范化复核：GetFullPathNameW 之后必须仍落在根内——文本校验管不住
//      junction / 符号链接这类重解析点。
// 失败一律经 err 出参点名原因，由调用方回灌给模型自我纠正。
//
// 与内核 video_stream 插件的 stream.hpp 路径安全同族——第二份独立实现
// （内核收编门槛达成）。

#include <nostos/kit/win.hpp>

#include <cwctype>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vcws {

class Jail {
public:
    explicit Jail(std::wstring root_wide) { root_ = canonical(std::move(root_wide)); }

    const std::wstring& root() const noexcept { return root_; }

    // 校验并解析：成功 ⇒ out_full（规范化全路径，Store 的键）+ out_norm（折叠后的
    // 相对路径，'/' 分隔）；失败 ⇒ false + err 点名原因。
    bool resolve(const std::string& rel_utf8, std::wstring& out_full, std::string& out_norm,
                 std::string& err) const {
        if (rel_utf8.empty()) { err = "路径为空"; return false; }
        if (rel_utf8.size() > 512) { err = "路径过长（>512 字节）"; return false; }
        for (const char c : rel_utf8)
            if (static_cast<unsigned char>(c) < 0x20) { err = "路径含控制字符"; return false; }

        // 绝对路径入口：盘符完整路径（X:\…）与根斜杠（/…、\…）。规范化后必须
        // 落在根内——在根内剥根前缀转相对路径走同一条分段校验；根外一律越界
        // 拒绝。盘符相对形态（"C:foo"，冒号后无分隔符）语义含混（相对该盘符
        // 当前目录），明确不支持；裸 "/" 规范化后是当前盘根，必然根外。
        const bool drive = rel_utf8.size() >= 2 && rel_utf8[1] == ':';
        const bool rooted = rel_utf8[0] == '/' || rel_utf8[0] == '\\';
        std::string rel = rel_utf8;
        if (drive || rooted) {
            if (drive && (rel_utf8.size() == 2 ||
                          (rel_utf8[2] != '/' && rel_utf8[2] != '\\'))) {
                err = "盘符相对路径不支持（用工作区内相对路径）";
                return false;
            }
            const std::wstring full = canonical(nostos::kit::win::utf8_to_wide(rel_utf8));
            if (!under_root(full)) {
                err = "绝对路径越出工作区根（jail 边界）";
                return false;
            }
            // under_root 保证前缀长度对齐且紧随分隔符——只留后缀，大小写差异
            //（盘符大小写等）随前缀一起剥掉
            rel = nostos::kit::win::wide_to_utf8(full.substr(root_.size()).c_str());
        }

        // 分段折叠（分隔符是 ASCII；UTF-8 多字节序列不含 0x2F/0x5C，按字节切安全）
        std::vector<std::string> parts;
        {
            std::string cur;
            for (const char c : rel) {
                if (c == '/' || c == '\\') {
                    parts.push_back(std::move(cur));
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            parts.push_back(std::move(cur));
        }
        out_norm.clear();
        for (std::string& seg : parts) {
            if (seg.empty() || seg == ".") continue;
            if (seg == "..") { err = "包含 '..'（越界尝试）"; return false; }
            if (seg.back() == '.' || seg.back() == ' ') {
                err = "段以 '.' 或空格结尾（Windows 会静默剥离）: " + seg;
                return false;
            }
            if (is_reserved_device(seg)) { err = "Windows 保留设备名: " + seg; return false; }
            if (!out_norm.empty()) out_norm.push_back('/');
            out_norm += seg;
        }
        if (out_norm.empty()) {
            // 纯点形态（"."、"./"、".//"——折叠后为空）语义 = 根本身：直接返回根，
            // 不再报"路径为空"（踩过：子代理列根目录送 "." 被拒，只好换路重试）
            out_full = root_;
            return true;
        }

        const std::wstring canon = canonical(root_ + L"\\" + nostos::kit::win::utf8_to_wide(out_norm));
        if (!under_root(canon)) { err = "规范化后越出工作区根"; return false; }
        out_full = canon;
        return true;
    }

private:
    static std::wstring canonical(const std::wstring& path) {
        // 第一次调用（长度 0）返回"需要的容量，含结尾 null"——缓冲必须按它分配，
        // 否则末字符被吞（踩过：fs::exists 对解析结果全挂）。
        const DWORD need = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (need == 0) return path;
        std::wstring out(need, L'\0');
        const DWORD copied = ::GetFullPathNameW(path.c_str(), need, out.data(), nullptr);
        if (copied == 0) return path;
        out.resize(copied);
        while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
        return out;
    }

    bool under_root(const std::wstring& full) const {
        const std::wstring a = lower(root_);
        const std::wstring b = lower(full);
        if (b == a) return true;  // 根本身（list 的空相对路径）
        return b.size() > a.size() && b.compare(0, a.size(), a) == 0 &&
               (b[a.size()] == L'\\' || b[a.size()] == L'/');
    }

    static std::wstring lower(std::wstring s) {
        for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
        return s;
    }

    // 仅裸名（con.txt 合法；con 不合法），大小写无关。
    static bool is_reserved_device(const std::string& seg) {
        std::string up;
        for (const char c : seg)
            up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (up == "CON" || up == "PRN" || up == "AUX" || up == "NUL") return true;
        if (up.size() == 4 && up.compare(0, 3, "COM") == 0 && up[3] >= '1' && up[3] <= '9')
            return true;
        if (up.size() == 4 && up.compare(0, 3, "LPT") == 0 && up[3] >= '1' && up[3] <= '9')
            return true;
        return false;
    }

    std::wstring root_;
};

}  // namespace vcws
