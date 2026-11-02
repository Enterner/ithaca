#pragma once
// read 工具族的共享管线（纯逻辑头）：分页出参 + 文档扩展名清单。
//
// 消费方两路：workspace 的文本 read、office 插件的文档抽取 read（shared/ 共享
// 树，与 text.hpp 同层）——两条路的出参形状必须逐字段一致
//（agent 压缩按工具名末段分派摘要分支、前端徽标读 doc 字段），形状分叉即事故，
// 所以管线只此一份；文档扩展名清单同理（workspace 守卫 / office 受理 / webui
// /api/fs/read 分流三处消费，各抄一份必然漂移）。
#include "nlohmann/json.hpp"

#include "vcws/text.hpp"  // vcws::trim_utf8：码点安全截断的收敛实现，勿再按字节切

#include <algorithm>
#include <string>
#include <utility>

namespace vcws {

using nlohmann::json;

inline constexpr std::size_t kReadCap = 256 * 1024;   // read 的内容字节上限（切片前的整读护栏）
inline constexpr int kReadLinesCap = 2000;            // read 单次返回行数上限（默认值 = 参数上限）
inline constexpr int kReadLineCharCap = 2000;         // read 单行字符上限（超长行按字符数截断行尾）
inline constexpr std::size_t kReadSlurpCap = 64ull * 1024 * 1024;  // 整读护栏：超过拒绝
                                      //（DiskStore 全量 slurp 无分页，护栏必须在读之前）

// 按字符数截断（码点边界）：工具自述的行上限是字符口径——trim_utf8 是字节预算，
// 中文 3 字节/字会提前 3 倍截断，模型按截断文本做 replace 锚点必失败。粗判先行：
// 字节数 ≤ cap 时字符数必 ≤ cap，零开销通过。内容已过 UTF-8 校验，多字节序列完整。
inline std::pair<std::string, bool> trim_utf8_chars(const std::string& s,
                                                    std::size_t max_chars) {
    if (s.size() <= max_chars) return {s, false};
    std::size_t chars = 0, i = 0;
    while (i < s.size()) {
        if (chars == max_chars) return {s.substr(0, i), true};
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        i += len;
        ++chars;
    }
    return {s, false};
}

// 文档扩展名（含点、小写）是否走文档抽取面（office 插件的域）。
inline bool is_document_ext(const std::string& ext) {
    return ext == ".docx" || ext == ".pptx" || ext == ".xlsx" ||
           ext == ".pdf" || ext == ".doc" || ext == ".ppt" || ext == ".xls";
}

// 分页出参（read 的文本/文档共用管线）：content 按行切窗、行首带真实行号，
// 结果形状 = {ok, path, content, truncated, total_lines, offset, limit[, doc]}。
inline json read_paged(std::string&& content, const std::string& norm, const json& args,
                       bool doc_truncated, const std::string& doc_kind) {
    // 按行分页（限制作用在「读」的动作上，结果内联一次回灌、无二次回查协议）。
    // 行按 \n 切、行内 \r 原样保留——重组与原文逐字节一致，apply 的精确块
    // 匹配不受分页展示影响。行首带行号（cat -n 形态，编号 = 文件真实行号，
    // 随 offset 对齐）：模型标注行号直接引用，不再需要逐符号 search 定位
    // （踩过: 正文无行号 ⇒ 子代理为标行号发起几十次单符号搜索）
    const bool empty = content.empty();
    const int total_lines =
        empty ? 0
              : static_cast<int>(std::count(content.begin(), content.end(), '\n')) +
                    (content.back() == '\n' ? 0 : 1);
    const int offset = std::max(args.value("offset", 1), 1);
    const int limit =
        std::clamp(args.value("limit", kReadLinesCap), 1, kReadLinesCap);
    std::string picked;
    bool line_truncated = false;
    if (!empty && offset <= total_lines) {
        std::size_t pos = 0;
        for (int ln = 1; ln < offset && pos != std::string::npos; ++ln)
            pos = content.find('\n', pos) + 1;
        for (int ln = 0; ln < limit && pos < content.size(); ++ln) {
            const std::size_t end = content.find('\n', pos);
            const std::size_t stop = end == std::string::npos ? content.size() : end + 1;
            std::string line = content.substr(pos, stop - pos);
            // 超长行按字符数截断（码点边界）；换行归属保真——原行带 \n 才补回，
            // 末行无 \n 不凭空造行（按字节截会劈码点；无条件补 \n 会给末行凭空造行）
            const bool ends_nl = !line.empty() && line.back() == '\n';
            const auto [cut, clipped] = trim_utf8_chars(
                ends_nl ? line.substr(0, line.size() - 1) : line,
                static_cast<std::size_t>(kReadLineCharCap));
            if (clipped) {
                line = cut + (ends_nl ? "\n" : "…（行尾截断）");
                line_truncated = true;
            }
            picked += std::to_string(offset + ln) + "\t" + line;
            pos = stop;
        }
    }
    bool truncated = line_truncated || offset + limit <= total_lines;
    if (picked.size() > kReadCap) {
        picked = vcws::trim_utf8(picked, kReadCap);
        truncated = true;
    }
    json out = {{"ok", true},
                {"path", norm},
                {"content", picked},
                {"truncated", truncated || doc_truncated},
                {"total_lines", total_lines},
                {"offset", offset},
                {"limit", limit}};
    if (!doc_kind.empty()) out["doc"] = doc_kind;   // 文档抽取来源（docx/…/ifilter:pdf）
    return out;
}

}  // namespace vcws
