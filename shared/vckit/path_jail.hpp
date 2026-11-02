#pragma once
// vckit —— 路径 jail：不可信来源给定路径的统一解析器 + 「在根判定器」。
//
// 语义（墙→闸）：jail 是解析器与「在根判定器」，不是硬边界——根内路径走完整
// 安全闸放行；根外的绝对形态（盘符/根斜杠/UNC）与 .. 上溢折叠结果解析为自身
// 并标 outside = true，交调用方处置（把关责任在权限模式层：suggest/auto-edit
// 走审批提案，full-auto 放行）。安全闸只对「在根」路径全量生效，三道闸缺一不可：
//   1) 文本卫生（规范化前对原始输入段）：拒绝控制字符、Windows 保留设备名、
//      段尾的点与空格（canonical 会静默剥离——请求 "a." 实际落盘 "a"，名实
//      不符，检查必须前置否则被吞段放空）、盘符相对形态（"C:foo"）；空段折叠，
//      "." 段丢弃；".." 段交规范化折叠后做在根判定（上溢逃出根 ⇒ 标 outside；
//      可达性是策略问题归权限模式，故不在词法期一刀切拒；卫生是 Windows
//      正确性问题，归本原语）。
//   2) 规范化分类：GetFullPathNameW 折叠后按根前缀唯一分类——在根内剥前缀转
//      相对；根外解析为自身（full = 规范化绝对路径，norm = '/' 分隔的绝对形态，
//      模型可见可复用：回传同一路径再次解析到同一文件）。
//   3) 物理复核（仅在根路径）：逐组件查重解析属性——词法规范化管不住 junction /
//      symlink（GetFullPathNameW 不解析重解析点，根内 junction 即静默导出根外，
//      会让 auto-edit 免批的工作区内写落到根外）。根外路径跳过本闸：系统
//      junction（"Documents and Settings" 类）合法存在，且根外的边界是审批不是
//      围栏——本闸防的是「免批写被静默导出」，不是可达性。
// 失败一律经 err 出参点名原因，由调用方回灌给模型自我纠正。
//
// NOTE: 安全原语单源——这是唯一实现，需要同一语义时 include 本头，不复制算法
//（第二份独立实现必漂移：手抄结构体/判据与真实布局脱节的事故形态，office 的
// STAT_CHUNK 镜像即前车之鉴）。

#include <cctype>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
// win.hpp 先行：由它引入 <windows.h>（LEAN_AND_MEAN/NOMINMAX 只在那一处定义）。
#include "vckit/win.hpp"
#endif

namespace vckit {

#if defined(_WIN32)

class PathJail {
public:
    explicit PathJail(std::wstring root_wide) { root_ = canonical(std::move(root_wide)); }

    // UTF-8 根构造（消费方边界处 env/config 的根都是 UTF-8，宽字符转换收进本
    // 边界单源）。转换失败 ⇒ 空根 ⇒ resolve 恒拒（安全原语的失败方向是关闭不是
    // 放行）；env 来源的根调用方自校验非空（现消费方均如此）。
    explicit PathJail(std::string root_utf8)
        : PathJail(win::utf8_to_wide(root_utf8)) {}

    const std::wstring& root() const noexcept { return root_; }

    // resolve 的聚合出参：与多参出参同语义，新消费方优先用它（少出参样板）。
    // err 非空 ⇒ 失败。full = 规范化全路径（调用方的存储键）；norm = 折叠后的
    // 相对路径（'/' 分隔；根外时为绝对形态）；outside = 是否落在根外；
    // err = 失败原因（回灌给模型）。
    struct Resolved {
        std::wstring full;
        std::string norm;
        bool outside = false;
        std::string err;
    };
    bool resolve(const std::string& rel_utf8, Resolved& out) const {
        out.err.clear();
        out.outside = false;
        return resolve(rel_utf8, out.full, out.norm, out.err, &out.outside);
    }

    // 校验并解析：成功 ⇒ out_full（规范化全路径，调用方的存储键）+ out_norm
    // （折叠后的相对路径，'/' 分隔；根外为绝对形态）+ 可选 outside 标记（调用方
    // 关心在根判定时传入）；失败 ⇒ false + err 点名原因。outside 缺省 null 保持
    // 只关心「解析成功与否」的既有调用点零改动。
    bool resolve(const std::string& rel_utf8, std::wstring& out_full, std::string& out_norm,
                 std::string& err, bool* outside = nullptr) const {
        if (outside != nullptr) *outside = false;
        if (rel_utf8.empty()) { err = "路径为空"; return false; }
        if (rel_utf8.size() > 512) { err = "路径过长（>512 字节）"; return false; }
        for (const char c : rel_utf8)
            if (static_cast<unsigned char>(c) < 0x20) { err = "路径含控制字符"; return false; }

        // 绝对形态入口：盘符完整路径（X:\…）、根斜杠（/…、\…）、UNC（\\srv\…）。
        // 盘符相对形态（"C:foo"，冒号后无分隔符）语义含混（相对该盘符当前目录），
        // 明确不支持。绝对与相对统一汇入「规范化 → 在根判定」单点分类。
        const bool drive = rel_utf8.size() >= 2 && rel_utf8[1] == ':';
        const bool rooted = rel_utf8[0] == '/' || rel_utf8[0] == '\\';
        if (drive && (rel_utf8.size() == 2 ||
                      (rel_utf8[2] != '/' && rel_utf8[2] != '\\'))) {
            err = "盘符相对路径不支持（用工作区内相对路径）";
            return false;
        }
        // 第一道闸（段卫生，在规范化**前**）：GetFullPathNameW 会静默剥离段尾的
        // 点与空格（"a." 规范化成 "a"）——后置检查会被吞段放空，名实不符的写法
        // 无声落到别的文件上（踩过）。保留设备名同批；"." / ".." 是合法折叠指令
        // 不在此列，空段跳过
        if (!input_hygienic(rel_utf8, err)) return false;

        std::wstring cand;
        if (drive || rooted) {
            cand = canonical(win::utf8_to_wide(rel_utf8));
        } else {
            // 相对输入：canonical(root + 输入) 一并折叠 "." / ".." / 重复分隔符。
            // ".." 不在词法期拒——上溢逃出根的行为由在根判定统一分类（词法拒绝
            // 挡不住「模型想出去」，只会逼它换写法重试；分类 + 审批才是把关点）
            cand = canonical(root_ + L"\\" + win::utf8_to_wide(rel_utf8));
        }

        if (!under_root(cand)) {
            // 根外：解析为自身，标 outside——把关上移权限模式层（suggest/auto-edit
            // 审批、full-auto 放行），jail 只报分类事实
            out_full = cand;
            out_norm = win::wide_to_utf8(slashify(cand).c_str());
            if (outside != nullptr) *outside = true;
            return true;
        }

        // 在根：剥根前缀得折叠后相对形态（under_root 保证前缀对齐且紧随分隔符；
        // 规范化产物分隔符恒为 '\'）。空后缀 = 根本身（"."、"．//" 类纯点形态）
        std::wstring rel_w;
        if (cand.size() > root_.size()) rel_w = cand.substr(root_.size() + 1);

        // norm：'/' 分隔的折叠后相对路径（快照/展示/模型锚点的稳定形态——永不含
        // ".." 与空段；根本身为空串）
        const std::string norm =
            rel_w.empty() ? std::string{} : win::wide_to_utf8(slashify(rel_w).c_str());

        // 第三道闸（重解析点，仅在根路径）：GetFullPathNameW 是纯词法操作，不解析
        // junction / symlink——根内任何 junction 都能把"根内词法路径"映射到根外
        // 物理位置（pnpm 的 node_modules、模型自建的 mklink /J），前两道闸全部
        // 放行。逐组件查 reparse 属性：任一存在的组件带重解析点 ⇒ 拒绝。等价于
        // 真实解析（GetFinalPathNameByHandle）但无需句柄、对不存在末端（新建
        // 文件）同样成立；短名（PROGRA~1）、大小写、尾随空白等词法-物理差异不出
        // 目录边界，无需另防。TOCTOU 残窗（校验后组件被换成 junction）依赖调用方
        // 把校验与落盘收敛在同一串行段内——本闸不承诺跨时间的安全。
        {
            std::wstring probe = root_;
            std::size_t pos = 0;
            while (pos < rel_w.size()) {
                const auto slash = rel_w.find(L'\\', pos);
                const std::wstring seg = rel_w.substr(
                    pos, slash == std::wstring::npos ? std::wstring::npos : slash - pos);
                probe += L"\\" + seg;
                const DWORD attr = ::GetFileAttributesW(probe.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    err = "路径含符号链接/junction（重解析点拒绝）: " +
                          win::wide_to_utf8(seg.c_str());
                    return false;
                }
                if (slash == std::wstring::npos) break;
                pos = slash + 1;
            }
        }
        out_full = cand;
        out_norm = norm;
        return true;
    }

private:
    static std::wstring canonical(const std::wstring& path) {
        // 第一次调用（长度 0）返回"需要的容量，含结尾 null"——缓冲必须按它分配，
        // 否则末字符被吞（踩过: fs::exists 对解析结果全挂）。
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
        if (b == a) return true;  // 根本身（列根目录的空相对路径）
        return b.size() > a.size() && b.compare(0, a.size(), a) == 0 &&
               (b[a.size()] == L'\\' || b[a.size()] == L'/');
    }

    static std::wstring lower(std::wstring s) {
        for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
        return s;
    }

    // 分隔符统一为 '/'——norm 是给模型/展示看的形态，'/' 跨工具口径一致。
    static std::wstring slashify(std::wstring p) {
        for (wchar_t& c : p)
            if (c == L'\\') c = L'/';
        return p;
    }

    // 段级卫生（Windows 正确性防线，非策略边界）：保留设备名、段尾点/空格。
    // 对在根与根外统一执行，且必须在规范化前对原始输入做（canonical 吞段尾
    // 点/空格，见 resolve 内注释）。
    static bool input_hygienic(const std::string& rel_utf8, std::string& err) {
        std::string cur;
        for (const char c : rel_utf8) {
            if (c == '/' || c == '\\') {
                if (!seg_ok(cur, err)) return false;
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        return seg_ok(cur, err);
    }

    // 单段判定：空段与折叠指令（"." / ".."）放行，名字段查段尾点/空格与保留
    // 设备名。UTF-8 多字节序列不含 0x2F/0x5C，按字节切段安全。
    static bool seg_ok(const std::string& seg, std::string& err) {
        if (seg.empty() || seg == "." || seg == "..") return true;
        if (seg.back() == '.' || seg.back() == ' ') {
            err = "段以 '.' 或空格结尾（Windows 会静默剥离）: " + seg;
            return false;
        }
        if (is_reserved_device(seg)) {
            err = "Windows 保留设备名: " + seg;
            return false;
        }
        return true;
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

#else

// 非 Windows：安全闸无语义可依时宁可显式拒绝，不做假放行。
class PathJail {
public:
    struct Resolved {
        std::wstring full;
        std::string norm;
        bool outside = false;
        std::string err;
    };
    explicit PathJail(std::wstring) {}
    explicit PathJail(std::string) {}
    const std::wstring& root() const noexcept {
        static const std::wstring empty;
        return empty;
    }
    bool resolve(const std::string&, Resolved& out) const {
        out.err = "路径 jail 仅 Windows 支持";
        return false;
    }
    bool resolve(const std::string&, std::wstring&, std::string&, std::string& err,
                 bool* = nullptr) const {
        err = "路径 jail 仅 Windows 支持";
        return false;
    }
};

#endif

}  // namespace vckit
