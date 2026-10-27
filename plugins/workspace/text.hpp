#pragma once
// 文本护栏（收敛点）：UTF-8 校验 / 码点安全截断 / 宽字符小写折叠——本仓
// 唯一实现，runner/webui 经相对路径共用（踩过：各自为政的年代里，runner 的
// sanitize_utf8 放行 overlong、webui 的按字节截断劈码点）。
// TODO: 内核 kit 收编后整体迁移。
#include <cwctype>
#include <string>

namespace vcws {

// 严格 UTF-8 校验：拒绝 overlong（C0/C1 头、E0 后 <0xA0、F0 后 <0x90）、代理区
//（ED 后 ≥0xA0）、>U+10FFFF（F5-FF）——nlohmann dump 对这些一律抛 type_error.316，
// 必须在进 JSON/事件之前拦住（runner sanitize 的教训：宽了就漏）。
inline bool utf8_valid(const std::string& s) {
    std::size_t i = 0;
    const auto b = [&s](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    while (i < s.size()) {
        const unsigned char c = b(i);
        if (c < 0x80) { ++i; continue; }
        std::size_t len = 0;
        if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        else return false;
        if (i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; ++k)
            if ((b(i + k) & 0xC0) != 0x80) return false;
        if (len == 2) { i += 2; continue; }
        const unsigned char c1 = b(i + 1);
        if (len == 3) {
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) return false;
        } else if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F)) return false;
        i += len;
    }
    return true;
}

// 截断到 cap 字节以内，且不劈开多字节码点（回退到码点首字节边界）。
// 前提：s 本身是完整 UTF-8（从合法字符串截断而来）。对「切片恰好 = cap」不校验
// 尾部——调用方自做字节切片（如 exec_recall 的分页窗口）必须自洽边界，否则尾部
// 残留不完整码点会让下游 dump() 抛 type_error.316（异常被 emit catch-all 吞掉
// ⇒ turn 挂死，踩过）。
inline std::string trim_utf8(const std::string& s, std::size_t cap) {
    if (s.size() <= cap) return s;
    std::size_t n = cap;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

// 字节分页窗口：取 s 的 [offset, offset+cap) 并双边界自洽——起点落在续字节上
// 则后移到码点首字节，尾部被 cap 劈开的残码点收回上一边界。前提：s 是完整
// UTF-8（nlohmann dump 产物）。offset 进出都是相对 s 的字节偏移，出参已对齐
// 码点边界（分页续读锚点）。产物可直接进 JSON——trim_utf8 只管「截断到 cap」
// 不管切片尾部，双边界必须由本函数一次做完（踩过：recall 分页尾部残留不完整
// 码点 ⇒ dump 抛 type_error.316，异常被 emit catch-all 吞掉，turn 挂死）。
inline std::string utf8_window(const std::string& s, std::size_t& offset, std::size_t cap) {
    if (offset > s.size()) offset = s.size();
    while (offset < s.size() &&
           (static_cast<unsigned char>(s[offset]) & 0xC0) == 0x80)
        ++offset;
    std::string page = s.substr(offset, cap);
    std::size_t k = 0, last = 0;
    while (k < page.size()) {
        last = k;
        const unsigned char c = static_cast<unsigned char>(page[k]);
        k += c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2
           : (c >= 0xE0 && c <= 0xEF) ? 3 : 4;
    }
    if (k != page.size()) page.resize(last);   // 尾码点被劈 ⇒ 收回到上一边界
    return page;
}

// UTF-8 清洗：非法字节替换为 replacement（子进程/外部字节流进 JSON 前的保守
// 净化——nlohmann dump 对非法序列抛 type_error.316）。口径与 utf8_valid 一致
// （overlong/代理区/>U+10FFFF 一律非法），区别只在处置是替换不是拒绝。
inline std::string utf8_sanitize(const std::string& s, char replacement = '?') {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        if (c < 0x80) len = 1;
        else if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        bool ok = len != 0 && i + len <= s.size();
        for (std::size_t k = 1; ok && k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) ok = false;
        if (ok && len == 3 &&
            ((c == 0xE0 && static_cast<unsigned char>(s[i + 1]) < 0xA0) ||
             (c == 0xED && static_cast<unsigned char>(s[i + 1]) >= 0xA0)))
            ok = false;
        if (ok && len == 4 &&
            ((c == 0xF0 && static_cast<unsigned char>(s[i + 1]) < 0x90) ||
             (c == 0xF4 && static_cast<unsigned char>(s[i + 1]) > 0x8F)))
            ok = false;
        if (!ok) { out.push_back(replacement); ++i; continue; }
        out.append(s, i, len);
        i += len;
    }
    return out;
}

// 宽字符小写折叠（Windows 路径大小写不敏感语义；文件标识键统一用它的产物）。
inline std::wstring wc_lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// ---- 行内高亮的公共跨度（diff 展示配对行用）------------------------------------
// 取 a/b 的公共前缀与公共后缀（按 UTF-8 码点对齐——字节级比较后回退/推进到码点
// 首字节，绝不把多字节字符劈成两半），拆出 (pre, mid_a, mid_b, suf)：
//   a = pre + mid_a + suf，b = pre + mid_b + suf。mid 为"变化子串"（行内高亮段）。
inline void utf8_change_span(const std::string& a, const std::string& b, std::string& pre,
                             std::string& mid_a, std::string& mid_b, std::string& suf) {
    std::size_t p = 0;
    while (p < a.size() && p < b.size() && a[p] == b[p]) ++p;
    while (p > 0 && (static_cast<unsigned char>(a[p]) & 0xC0) == 0x80) --p;
    std::size_t s = 0;
    while (s < a.size() - p && s < b.size() - p &&
           a[a.size() - 1 - s] == b[b.size() - 1 - s])
        ++s;
    // 字节级失配点落在多字节字符中间 ⇒ 后缀收缩到码点首字节（扩展会并入不相等字节）
    while (s > 0 && (static_cast<unsigned char>(a[a.size() - s]) & 0xC0) == 0x80) --s;
    pre = a.substr(0, p);
    mid_a = a.substr(p, a.size() - p - s);
    mid_b = b.substr(p, b.size() - p - s);
    suf = s > 0 ? a.substr(a.size() - s) : std::string();
}

}  // namespace vcws
