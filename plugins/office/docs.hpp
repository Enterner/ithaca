#pragma once
// 主流办公文档 → 纯文本抽取（plugin/office 的核心源，只读能力域的唯一实现）。
//
// 两层：
//   - 容器底座在 zip.hpp（vcoffice::zip）：ZIP 自解（EOCD 尾扫 → central
//     directory 定位条目 + raw DEFLATE 解码），与文档语义零耦合；
//   - 本头是文档语义层：OOXML 族（docx/pptx/xlsx）容忍式 XML 文本扫描（只认
//     <w:t>/<a:t>/<si>-<t>/<c>-<v> 等文本元素与实体解码）+ 其余注册格式
//     （pdf/doc/ppt/xls）经系统 IFilter（COM，ntquery 的 LoadIFilter 动态解析）。
//     不引第三方依赖——抽取目标只是正文文本，输入面收敛为「Office 写出的合法
//     文件」，容忍式扫描足够，坏文件点名失败即可；
//   - IFilter 路径：机器装了对应过滤器（Office / Adobe 等）即可读，没装则点名
//     拒绝——不静默失败。
//
// 上限：归档 64MB、抽取文本 512KB（read 分页护栏 kReadCap=256KB 的头寸），
// 超限截断并标 truncated。
//
// 线程纪律：调用方（read 工具）在宿主线程 / webui 串行锁内同步执行——OOXML
// 路径纯 CPU；IFilter 路径 CoInitializeEx(APARTMENTTHREADED) 现场初始化
// （宿主线程默认无 COM），第三方过滤器内同步执行（量级百毫秒）。

#include "zip.hpp"  // vcoffice::zip：ZIP 容器 + raw DEFLATE（抽取基建单源）

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
// IFilter 接口与 STAT_CHUNK 取官方头——手抄 ABI 镜像必然漂移（踩过: STAT_CHUNK
// 的 attribute 是 FULLPROPSPEC，x64 真实 64 字节，按 GUID 的 16 字节镜像只有 40
// ⇒ 第三方过滤器按真实布局写满 64、缓冲 40，每次 GetChunk 栈越界写 24 字节；
// 文本仍能抽出来是因为 flags 偏移 8 碰巧对齐，栈帧损坏被产物正确性掩盖）。
// 手抄镜像的 HRESULT 语义值同样会抄错（踩过: 0x8004177B 系——真值
// 0x80041700/0x00041709 见 filterr.h——错值靠 hr<0 兜底碰巧跑通）。
#include <propidl.h>
#include <filterr.h>
#include <filter.h>

namespace vcoffice::docs {

// FILTER_S_LAST_CHUNK 在现行 SDK 头里更名 FILTER_S_LAST_VALUES（同值 0x0004170A）
// ——语义仍按 IFilter 文档的 LAST_CHUNK 消费
constexpr long kFilterSLastChunk = static_cast<long>(FILTER_S_LAST_VALUES);

inline constexpr std::size_t kArchiveCap = 64ull * 1024 * 1024;
inline constexpr std::size_t kTextCap = 512ull * 1024;

struct DocText {
    bool ok = false;
    bool truncated = false;
    std::string text;    // 抽取出的正文（\n 分行；失败时为空）
    std::string kind;    // "docx"/"pptx"/"xlsx"/"ifilter:<ext>"
    std::string error;   // ok=false 时的人话原因
};

// ---- XML 扫描基础件（实体解码/UTF-8 编码/属性取值/元素定位）------------------

inline void push_codepoint(std::string& out, unsigned cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    // 越界码点（拆不完整的数字引用）静默丢弃
}

// XML 实体解码（OOXML 面只出五种命名实体与数字引用）。
inline void xml_append_text(std::string& out, const char* p, const char* e) {
    while (p < e) {
        if (*p != '&') { out.push_back(*p++); continue; }
        const char* semi = static_cast<const char*>(std::memchr(p, ';', static_cast<size_t>(e - p)));
        if (semi == nullptr || semi - p > 10) { out.push_back(*p++); continue; }
        const std::string ent(p + 1, semi);
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (!ent.empty() && ent[0] == '#') {
            const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
            const unsigned long cp = std::strtoul(ent.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
            if (cp > 0) push_codepoint(out, static_cast<unsigned>(cp));
        } else {
            out.append(p, static_cast<size_t>(semi - p + 1));   // 未知实体按原样保留
        }
        p = semi + 1;
    }
}

inline void append_utf16(std::string& out, const wchar_t* s, size_t k) {
    for (size_t i = 0; i < k; ++i) {
        unsigned c = static_cast<unsigned short>(s[i]);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < k) {
            const unsigned lo = static_cast<unsigned short>(s[i + 1]);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        push_codepoint(out, c);
    }
}

// 标签内属性取值：attr="value" / attr='value'。要求属性名前是标签起始或空白
//（防 <sid 命中 <si 一类），值实体照解码。
inline std::string xml_attr_value(const char* tag, const char* tag_end, const char* attr) {
    const size_t alen = std::strlen(attr);
    for (const char* p = tag; p + alen + 2 < tag_end; ++p) {
        if (std::memcmp(p, attr, alen) != 0) continue;
        if (p != tag && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n' && p[-1] != '\r') continue;
        const char* v = p + alen;
        while (v < tag_end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) ++v;
        if (v >= tag_end || *v != '=') continue;
        ++v;
        while (v < tag_end && (*v == ' ' || *v == '\t')) ++v;
        if (v >= tag_end || (*v != '"' && *v != '\'')) continue;
        const char q = *v++;
        const char* ve = static_cast<const char*>(std::memchr(v, q, static_cast<size_t>(tag_end - v)));
        if (ve == nullptr) return {};
        std::string out;
        xml_append_text(out, v, ve);
        return out;
    }
    return {};
}

// 定位首个「<name…>」开标签：返回内容区间 [content_begin, content_end) 与闭合后
// 位置 tag_end。开标签要求名字边界（下一个字符不是名字字符）。
inline bool find_element(const std::string& xml, const char* name, size_t from,
                         size_t* content_begin, size_t* content_end, size_t* tag_end) {
    const std::string open = std::string("<") + name;
    size_t pos = xml.find(open, from);
    while (pos != std::string::npos) {
        const char next = pos + open.size() < xml.size() ? xml[pos + open.size()] : '\0';
        if (next != '>' && next != ' ' && next != '\t' && next != '\n' && next != '\r' && next != '/') {
            pos = xml.find(open, pos + open.size());
            continue;
        }
        const size_t gt = xml.find('>', pos);
        if (gt == std::string::npos) return false;
        *content_begin = gt + 1;
        const std::string close = std::string("</") + name + ">";
        const size_t ce = xml.find(close, *content_begin);
        *content_end = ce == std::string::npos ? xml.size() : ce;
        *tag_end = ce == std::string::npos ? xml.size() : ce + close.size();
        return true;
    }
    return false;
}

// ---- OOXML：ZIP 容器（vcoffice::zip，见同目录 zip.hpp）----------------------

namespace zip = vcoffice::zip;   // 本命名空间内的 zip:: 引用即容器底座

// ---- OOXML 文本抽取 ----------------------------------------------------------

// 容忍式扫描：提取 <text_tag>…</text_tag> 的文本（文档序）；</para_tag> 输出
// 换行；自闭合 <br_tag/> → \n、<tab_tag/> → \t（标签可空 = 不处理该类）。
inline void scan_xml_text(const std::string& xml, const char* text_tag,
                          const char* para_tag, const char* br_tag, const char* tab_tag,
                          std::string& out, bool* truncated) {
    const std::string to = std::string("<") + text_tag;
    const std::string pc = std::string("</") + para_tag + ">";
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        if (out.size() > kTextCap) { *truncated = true; return; }
        if (xml.compare(pos, pc.size(), pc) == 0) { out.push_back('\n'); pos += pc.size(); continue; }
        // 自闭合控制元素（<w:br/> / <w:tab/> / <a:br/>）：整标签按映射输出
        bool handled = false;
        for (const auto& ctl : {std::pair<const char*, const char*>{br_tag, "\n"},
                                std::pair<const char*, const char*>{tab_tag, "\t"}}) {
            if (ctl.first == nullptr) continue;
            const std::string so = std::string("<") + ctl.first;
            if (xml.compare(pos, so.size(), so) != 0) continue;
            const size_t gt = xml.find('>', pos);
            if (gt == std::string::npos) return;
            if (gt > pos && xml[gt - 1] == '/') out.append(ctl.second);   // 仅自闭合形态
            pos = gt + 1;
            handled = true;
            break;
        }
        if (handled) continue;
        if (xml.compare(pos, to.size(), to) == 0) {
            const char next = pos + to.size() < xml.size() ? xml[pos + to.size()] : '\0';
            if (next == '>' || next == ' ' || next == '\t' || next == '\n' || next == '\r') {
                const size_t gt = xml.find('>', pos);
                if (gt == std::string::npos) return;
                const std::string close = std::string("</") + text_tag + ">";
                const size_t ce = xml.find(close, gt + 1);
                if (ce == std::string::npos) return;
                xml_append_text(out, xml.data() + gt + 1, xml.data() + ce);
                pos = ce + close.size();
                continue;
            }
        }
        // 无关标签：整段跳过
        pos = xml.find('>', pos);
        if (pos == std::string::npos) return;
        ++pos;
    }
}

// xlsx 单元格抽取：<c r="B3" t="s"><v>12</v></c> —— t="s" 引共享字符串、
// t="inlineStr" 用 <is><t>、t="str" 是公式串 <v>、无 t 为数字 <v>。行号变化
// 即换行，同行单元格以 \t 分隔（表格语义的最小人话形态）。
inline void scan_sheet(const std::string& xml, const std::vector<std::string>& shared,
                       std::string& out, bool* truncated) {
    size_t pos = 0;
    long cur_row = -1;
    bool row_has = false;
    const std::string ct = "<c ";
    while ((pos = xml.find(ct, pos)) != std::string::npos) {
        if (out.size() > kTextCap) { *truncated = true; return; }
        const size_t gt = xml.find('>', pos);
        if (gt == std::string::npos) return;
        if (gt > pos && xml[gt - 1] == '/') { pos = gt + 1; continue; }   // 空单元格
        const std::string r = xml_attr_value(xml.data() + pos, xml.data() + gt + 1, "r");
        const std::string t = xml_attr_value(xml.data() + pos, xml.data() + gt + 1, "t");
        long row = 0;
        for (const char ch : r) {
            if (ch >= '0' && ch <= '9') row = row * 10 + (ch - '0');
            else row = 0;   // 列字母段丢弃，尾段数字 = 行号
        }
        if (row > 0 && row != cur_row) {
            if (row_has) out.push_back('\n');
            cur_row = row;
            row_has = true;
        } else if (row_has) {
            out.push_back('\t');
        }
        const std::string cc = "</c>";
        const size_t ce = xml.find(cc, gt);
        if (ce == std::string::npos) return;
        const std::string body = xml.substr(gt + 1, ce - gt - 1);
        size_t b = 0, e2 = 0, te = 0;
        if (t == "s") {
            if (find_element(body, "v", 0, &b, &e2, &te)) {
                const long idx = std::strtol(body.c_str() + b, nullptr, 10);
                if (idx >= 0 && static_cast<size_t>(idx) < shared.size())
                    out += shared[static_cast<size_t>(idx)];
            }
        } else if (t == "inlineStr") {
            while (find_element(body, "t", e2, &b, &e2, &te))
                xml_append_text(out, body.data() + b, body.data() + e2);
        } else if (find_element(body, "v", 0, &b, &e2, &te)) {
            xml_append_text(out, body.data() + b, body.data() + e2);
        }
        pos = ce + cc.size();
    }
    if (row_has && !out.empty() && out.back() != '\n') out.push_back('\n');
}

inline bool extract_ooxml(const unsigned char* p, size_t n, const std::string& kind,
                          DocText& d) {
    std::vector<zip::Entry> entries;
    if (!zip::list_entries(p, n, entries)) {
        d.error = "不是有效的 OOXML 包（ZIP 目录解析失败）";
        return false;
    }
    auto find_entry = [&](const std::string& name) -> const zip::Entry* {
        for (const auto& e : entries)
            if (e.name == name) return &e;
        return nullptr;
    };
    auto load = [&](const std::string& name, std::string& out) {
        const zip::Entry* e = find_entry(name);
        return e != nullptr && zip::read_entry(p, n, *e, out, zip::kEntryCap);
    };
    if (kind == "docx") {
        std::string xml;
        if (!load("word/document.xml", xml)) { d.error = "docx 缺少 word/document.xml"; return false; }
        scan_xml_text(xml, "w:t", "w:p", "w:br", "w:tab", d.text, &d.truncated);
        d.kind = "docx";
    } else if (kind == "pptx") {
        // 幻灯按编号排序（slide10 排 slide2 后——字典序会错）
        std::vector<int> nums;
        for (const auto& e : entries) {
            if (e.name.rfind("ppt/slides/slide", 0) == 0 && e.name.size() >= 21 &&
                e.name.compare(e.name.size() - 4, 4, ".xml") == 0)
                nums.push_back(std::atoi(e.name.c_str() + 16));
        }
        std::sort(nums.begin(), nums.end());
        if (nums.empty()) { d.error = "pptx 里没有幻灯页"; return false; }
        for (const int no : nums) {
            if (d.truncated) break;
            std::string xml;
            if (!load("ppt/slides/slide" + std::to_string(no) + ".xml", xml)) continue;
            d.text += "── 第 " + std::to_string(no) + " 页 ──\n";
            scan_xml_text(xml, "a:t", "a:p", "a:br", nullptr, d.text, &d.truncated);
            if (!d.text.empty() && d.text.back() != '\n') d.text.push_back('\n');
        }
        d.kind = "pptx";
    } else {   // xlsx
        std::string xml;
        std::vector<std::string> shared;
        if (load("xl/sharedStrings.xml", xml)) {
            size_t b = 0, e2 = 0, te = 0;
            while (find_element(xml, "si", e2, &b, &e2, &te) && e2 <= xml.size()) {
                std::string item;
                size_t tb = b, t2 = b, t3 = b;
                while (find_element(xml, "t", t2, &tb, &t2, &t3) && t2 <= e2)
                    xml_append_text(item, xml.data() + tb, xml.data() + t2);
                shared.push_back(std::move(item));
            }
        }
        // 表名与顺序：workbook.xml 的 <sheet name r:id> + workbook.xml.rels 的 rId→目标
        std::string wb, rels;
        std::vector<std::pair<std::string, std::string>> sheets;   // {name, target}
        if (load("xl/workbook.xml", wb)) {
            std::vector<std::pair<std::string, std::string>> rid;  // {Id, Target}
            if (load("xl/_rels/workbook.xml.rels", rels)) {
                size_t pos = 0;
                while ((pos = rels.find("<Relationship ", pos)) != std::string::npos) {
                    const size_t gt = rels.find('>', pos);
                    if (gt == std::string::npos) break;
                    rid.emplace_back(xml_attr_value(rels.data() + pos, rels.data() + gt, "Id"),
                                     xml_attr_value(rels.data() + pos, rels.data() + gt, "Target"));
                    pos = gt + 1;
                }
            }
            size_t pos = 0;
            while ((pos = wb.find("<sheet ", pos)) != std::string::npos) {
                const size_t gt = wb.find('>', pos);
                if (gt == std::string::npos) break;
                const std::string id = xml_attr_value(wb.data() + pos, wb.data() + gt, "r:id");
                const std::string name = xml_attr_value(wb.data() + pos, wb.data() + gt, "name");
                for (const auto& [i, t] : rid)
                    if (i == id && !t.empty()) { sheets.emplace_back(name, t); break; }
                pos = gt + 1;
            }
        }
        for (const auto& [name, target] : sheets) {
            if (d.truncated) break;
            std::string sx;
            const std::string path = target.rfind("/", 0) == 0 ? "xl" + target : "xl/" + target;
            if (!load(path, sx)) continue;
            d.text += "── 表：" + (name.empty() ? "（未命名）" : name) + " ──\n";
            scan_sheet(sx, shared, d.text, &d.truncated);
        }
        d.kind = "xlsx";
    }
    if (d.text.empty() && !d.truncated) {
        d.error = kind + " 里没有可抽取的文本内容";
        return false;
    }
    d.ok = true;
    return true;
}

// ---- IFilter（pdf/doc/ppt/xls 等注册格式）------------------------------------
// 接口与 STAT_CHUNK 均来自官方 <filter.h>（SDK um/ 下仍在）。语义值同头内
// FILTER_E_END_OF_CHUNKS / FILTER_S_LAST_CHUNK / FILTER_S_LAST_TEXT。
inline bool extract_ifilter(const std::wstring& path, const std::string& ext, DocText& d) {
    // COM 现场初始化（宿主线程默认无 COM）；线程已在别的模式时沿用（RPC_E_CHANGED_MODE）
    const long hr_init = CoInitializeEx(nullptr, 0x2 /*COINIT_APARTMENTTHREADED*/);
    const bool com = hr_init >= 0;
    struct ComGuard { bool on; ~ComGuard() { if (on) CoUninitialize(); } } guard{com};

    using LoadIFilterFn = long(__stdcall*)(const wchar_t*, IUnknown*, void**);
    // 新老 Windows 的载体不同：ntquery.dll（老）→ query.dll（Win11 起合并）
    HMODULE q = GetModuleHandleW(L"ntquery.dll");
    if (q == nullptr) q = GetModuleHandleW(L"query.dll");
    if (q == nullptr) q = LoadLibraryW(L"ntquery.dll");
    if (q == nullptr) q = LoadLibraryW(L"query.dll");
    if (q == nullptr) {
        d.error = "系统缺少 IFilter 基础设施（ntquery/query.dll）——" + ext.substr(1) + " 无法读取";
        return false;
    }
    const auto load_filter = reinterpret_cast<LoadIFilterFn>(
        static_cast<void*>(GetProcAddress(q, "LoadIFilter")));
    if (load_filter == nullptr) {
        d.error = "系统 IFilter 入口缺失（LoadIFilter）——" + ext.substr(1) + " 无法读取";
        return false;
    }
    void* raw = nullptr;
    if (load_filter(path.c_str(), nullptr, &raw) < 0 || raw == nullptr) {
        d.error = "本机没有安装 " + ext.substr(1) +
                  " 的文本过滤器（需 Office / Adobe 等提供）——可转存为 docx 再读";
        return false;
    }
    IFilter* f = static_cast<IFilter*>(raw);
    unsigned long oflags = 0;
    if (f->Init(0, 0, nullptr, &oflags) < 0) {
        d.error = ext.substr(1) + " 的文本过滤器初始化失败";
        f->Release();
        return false;
    }
    bool last_text = false, last_chunk = false;
    while (!last_chunk && !last_text) {
        STAT_CHUNK ch{};
        const long hr = f->GetChunk(&ch);
        if (hr < 0 || hr == FILTER_E_END_OF_CHUNKS) break;
        last_chunk = hr == kFilterSLastChunk;   // LAST_CHUNK 仍携带有效 chunk
        if (ch.flags & 1) {
            for (;;) {
                unsigned long got = 4096;
                wchar_t buf[4096];
                const long tr = f->GetText(&got, buf);
                if (tr >= 0 && got > 0 && d.text.size() <= kTextCap)
                    append_utf16(d.text, buf, got);
                if (tr != 0) {   // 非 S_OK：LAST_TEXT（终块）或错误（本 chunk 无更多文本）
                    last_text = tr == FILTER_S_LAST_TEXT;
                    break;
                }
            }
            if (!d.text.empty() && d.text.back() != '\n') d.text.push_back('\n');
            if (d.text.size() > kTextCap) { d.truncated = true; break; }
        }
    }
    f->Release();
    if (d.text.empty() && !d.truncated) {
        d.error = "文档里没有可抽取的文本（扫描件 / 纯图 " + ext.substr(1) + " 无文本层）";
        return false;
    }
    d.kind = "ifilter:" + ext.substr(1);
    d.ok = true;
    return true;
}

// ---- 入口 ---------------------------------------------------------------------
// 唯一入口：OOXML 族（docx/pptx/xlsx）走字节面 p/n，其余注册格式经 IFilter 走
// 真实文件 path；失败以 DocText.error 点名，ok=false。

inline DocText extract(const unsigned char* p, size_t n, const std::string& ext,
                       const std::wstring& path) {
    DocText d;
    if (ext == ".docx" || ext == ".pptx" || ext == ".xlsx") {
        extract_ooxml(p, n, ext.substr(1), d);
        return d;
    }
    extract_ifilter(path, ext, d);   // IFilter 按真实文件路径打开
    return d;
}

}  // namespace vcoffice::docs
