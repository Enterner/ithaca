#pragma once
// 主流办公文档 → 纯文本抽取（plugin/workspace 的只读面扩展）。
//
// 两层：
//   - OOXML 族（docx/pptx/xlsx）原生解析：ZIP 容器自解（EOCD 尾扫 → central
//     directory 定位条目 + raw DEFLATE 解码）+ 容忍式 XML 文本扫描（只认
//     <w:t>/<a:t>/<si>-<t>/<c>-<v> 等文本元素与实体解码）。不引第三方依赖——
//     抽取目标只是正文文本，输入面收敛为「Office 写出的合法文件」，容忍式
//     扫描足够，坏文件点名失败即可；
//   - 其余注册格式（pdf/doc/ppt/xls）经系统 IFilter（COM，ntquery 的
//     LoadIFilter 动态解析）：机器装了对应过滤器（Office / Adobe 等）即可读，
//     没装则点名拒绝——不静默失败。
//
// 上限：归档 64MB、抽取文本 512KB（read 分页护栏 kReadCap=256KB 的头寸），
// 超限截断并标 truncated；同时是 zip 炸弹的解压护栏。
//
// 线程纪律：调用方（read 工具）在宿主线程 / webui 串行锁内同步执行——OOXML
// 路径纯 CPU；IFilter 路径 CoInitializeEx(APARTMENTTHREADED) 现场初始化
// （宿主线程默认无 COM），第三方过滤器内同步执行（量级百毫秒）。

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

namespace vcws::docs {

inline constexpr std::size_t kArchiveCap = 64ull * 1024 * 1024;
inline constexpr std::size_t kTextCap = 512ull * 1024;
inline constexpr std::size_t kEntryCap = 32ull * 1024 * 1024;   // 单条目解压上限

struct DocText {
    bool ok = false;
    bool truncated = false;
    std::string text;    // 抽取出的正文（\n 分行；失败时为空）
    std::string kind;    // "docx"/"pptx"/"xlsx"/"ifilter:<ext>"
    std::string error;   // ok=false 时的人话原因
};

// 扩展名（含点、小写）是否走文档抽取面。
inline bool is_document_ext(const std::string& ext) {
    return ext == ".docx" || ext == ".pptx" || ext == ".xlsx" ||
           ext == ".pdf" || ext == ".doc" || ext == ".ppt" || ext == ".xls";
}

// ---- 基础件 -----------------------------------------------------------------

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

// ---- OOXML：ZIP 容器 + raw DEFLATE ------------------------------------------

namespace zip {

// RFC 1951 原始 DEFLATE 解码（puff 形态：counts/symbols 两级表，逐位游走）。
// 返回 false = 数据损坏 / 超 max_out——坏文件点名失败，不做容错续读。
struct Bits {
    const unsigned char* p;
    size_t n;
    size_t pos = 0;     // 字节游标
    unsigned bit = 0;   // 字节内位游标（LSB 先行）
    int take(int count) {
        int v = 0;
        for (int i = 0; i < count; ++i) {
            if (pos >= n) return -1;
            v |= ((p[pos] >> bit) & 1) << i;
            if (++bit == 8) { bit = 0; ++pos; }
        }
        return v;
    }
    void align() { if (bit != 0) { bit = 0; ++pos; } }
};

struct Huff {
    short counts[16] = {};
    short symbols[288] = {};
    bool build(const unsigned char* lengths, int n) {
        for (int i = 0; i < n; ++i) counts[lengths[i]]++;
        counts[0] = 0;
        short left = 1;
        for (int len = 1; len <= 15; ++len) {
            left = static_cast<short>(left << 1);
            left -= counts[len];
            if (left < 0) return false;   // 过满：坏码
        }
        short offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<short>(offs[len] + counts[len]);
        for (int sym = 0; sym < n; ++sym)
            if (lengths[sym]) symbols[offs[lengths[sym]]++] = static_cast<short>(sym);
        return true;
    }
    int decode(Bits& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            const int b = br.take(1);
            if (b < 0) return -1;
            code |= b;
            const int count = counts[len];
            if (code - first < count) return symbols[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }
};

inline bool inflate(const unsigned char* src, size_t n, std::string& out, size_t max_out) {
    static const unsigned short len_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
        35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const unsigned char len_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
        3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const unsigned short dist_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
        257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const unsigned char dist_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
        7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    static const unsigned char clc_order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

    Bits br{src, n};
    for (;;) {
        const int final_block = br.take(1);
        const int type = br.take(2);
        if (final_block < 0 || type < 0) return false;
        if (type == 0) {   // stored
            br.align();
            if (br.pos + 4 > br.n) return false;
            const unsigned len = br.p[br.pos] | (br.p[br.pos + 1] << 8);
            br.pos += 4;   // LEN + NLEN（NLEN 不校验——Office 不出坏块）
            if (br.pos + len > br.n || out.size() + len > max_out) return false;
            out.append(reinterpret_cast<const char*>(br.p + br.pos), len);
            br.pos += len;
        } else {
            Huff lit, dist;
            if (type == 1) {   // 固定 Huffman
                unsigned char l[288];
                int i = 0;
                for (; i < 144; ++i) l[i] = 8;
                for (; i < 256; ++i) l[i] = 9;
                for (; i < 280; ++i) l[i] = 7;
                for (; i < 288; ++i) l[i] = 8;
                lit.build(l, 288);
                unsigned char d[30];
                for (i = 0; i < 30; ++i) d[i] = 5;
                dist.build(d, 30);
            } else if (type == 2) {   // 动态 Huffman
                const int hlit = br.take(5), hdist = br.take(5), hclen = br.take(4);
                if (hlit < 0 || hdist < 0 || hclen < 0) return false;
                // RFC 1951：三计数器都是「差值」——码数 = HLIT+257 / HDIST+1 / HCLEN+4
                const int nlit = hlit + 257, ndist = hdist + 1, ncode = hclen + 4;
                if (nlit > 286 || ndist > 30) return false;
                unsigned char cl[19] = {};
                for (int i = 0; i < ncode; ++i) {
                    const int v = br.take(3);
                    if (v < 0) return false;
                    cl[clc_order[i]] = static_cast<unsigned char>(v);
                }
                Huff clh;
                if (!clh.build(cl, 19)) return false;
                unsigned char lengths[288 + 30] = {};
                int idx = 0;
                while (idx < nlit + ndist) {
                    const int sym = clh.decode(br);
                    if (sym < 0) return false;
                    if (sym < 16) { lengths[idx++] = static_cast<unsigned char>(sym); continue; }
                    int rep = 0;
                    unsigned char val = 0;
                    if (sym == 16) {
                        if (idx == 0) return false;
                        val = lengths[idx - 1];
                        rep = br.take(2);
                        if (rep < 0) return false;
                        rep += 3;
                    } else if (sym == 17) {
                        rep = br.take(3);
                        if (rep < 0) return false;
                        rep += 3;
                    } else {
                        rep = br.take(7);
                        if (rep < 0) return false;
                        rep += 11;
                    }
                    if (idx + rep > nlit + ndist) return false;
                    while (rep-- > 0) lengths[idx++] = val;
                }
                if (!lit.build(lengths, nlit)) return false;
                if (!dist.build(lengths + nlit, ndist)) return false;
            } else {
                return false;   // type 3 = 保留（坏数据）
            }
            for (;;) {
                const int sym = lit.decode(br);
                if (sym < 0) return false;
                if (sym < 256) {
                    if (out.size() >= max_out) return false;
                    out.push_back(static_cast<char>(sym));
                    continue;
                }
                if (sym == 256) break;
                const int li = sym - 257;
                if (li >= 29) return false;
                const int le = br.take(len_extra[li]);
                if (le < 0) return false;
                const int di = dist.decode(br);
                if (di < 0 || di >= 30) return false;
                const int de = br.take(dist_extra[di]);
                if (de < 0) return false;
                const size_t distance = dist_base[di] + static_cast<size_t>(de);
                const size_t length = len_base[li] + static_cast<size_t>(le);
                if (distance > out.size() || out.size() + length > max_out) return false;
                for (size_t k = 0; k < length; ++k)
                    out.push_back(out[out.size() - distance]);
            }
        }
        if (final_block) return true;
    }
}

struct Entry {
    std::string name;
    unsigned method = 0;
    size_t csize = 0, usize = 0, data_off = 0;
};

// central directory 定位（EOCD 尾扫）取条目清单。ZIP64 不支持（文档面不出现
// >4GB / 65535 项的归档，命中即点名失败）。
inline bool list_entries(const unsigned char* p, size_t n, std::vector<Entry>& out) {
    if (n < 22) return false;
    size_t eocd = std::string::npos;
    const size_t scan_from = n > 66000 ? n - 66000 : 0;
    for (size_t i = n - 22 + 1; i-- > scan_from;)
        if (p[i] == 0x50 && p[i + 1] == 0x4b && p[i + 2] == 0x05 && p[i + 3] == 0x06) { eocd = i; break; }
    if (eocd == std::string::npos) return false;
    const unsigned count = p[eocd + 10] | (p[eocd + 11] << 8);
    const unsigned cd_size = static_cast<unsigned>(p[eocd + 12]) |
                             (static_cast<unsigned>(p[eocd + 13]) << 8) |
                             (static_cast<unsigned>(p[eocd + 14]) << 16) |
                             (static_cast<unsigned>(p[eocd + 15]) << 24);
    const size_t cd = static_cast<size_t>(p[eocd + 16]) | (static_cast<size_t>(p[eocd + 17]) << 8) |
                      (static_cast<size_t>(p[eocd + 18]) << 16) | (static_cast<size_t>(p[eocd + 19]) << 24);
    if (cd == 0xFFFFFFFFu || cd + cd_size > n) return false;
    size_t cur = cd;
    for (unsigned i = 0; i < count && cur + 46 <= n; ++i) {
        if (!(p[cur] == 0x50 && p[cur + 1] == 0x4b && p[cur + 2] == 0x01 && p[cur + 3] == 0x02)) return false;
        Entry e;
        e.method = p[cur + 10] | (p[cur + 11] << 8);
        e.csize = static_cast<size_t>(p[cur + 20]) | (static_cast<size_t>(p[cur + 21]) << 8) |
                  (static_cast<size_t>(p[cur + 22]) << 16) | (static_cast<size_t>(p[cur + 23]) << 24);
        e.usize = static_cast<size_t>(p[cur + 24]) | (static_cast<size_t>(p[cur + 25]) << 8) |
                  (static_cast<size_t>(p[cur + 26]) << 16) | (static_cast<size_t>(p[cur + 27]) << 24);
        const unsigned nlen = p[cur + 28] | (p[cur + 29] << 8);
        const unsigned elen = p[cur + 30] | (p[cur + 31] << 8);
        const unsigned clen = p[cur + 32] | (p[cur + 33] << 8);
        const unsigned lho = static_cast<unsigned>(p[cur + 42]) | (static_cast<unsigned>(p[cur + 43]) << 8) |
                             (static_cast<unsigned>(p[cur + 44]) << 16) | (static_cast<unsigned>(p[cur + 45]) << 24);
        if (cur + 46u + nlen + elen + clen > n) return false;
        e.name.assign(reinterpret_cast<const char*>(p + cur + 46), nlen);
        // 数据偏移 = local header + 30 + local name len + local extra len（local
        // extra 与 central 的可以不同——必须回读 local 头）
        if (lho + 30 > n || !(p[lho] == 0x50 && p[lho + 1] == 0x4b && p[lho + 2] == 0x03 && p[lho + 3] == 0x04))
            return false;
        const unsigned lnlen = p[lho + 26] | (p[lho + 27] << 8);
        const unsigned lelen = p[lho + 28] | (p[lho + 29] << 8);
        e.data_off = lho + 30u + lnlen + lelen;
        if (e.data_off + e.csize > n) return false;
        out.push_back(std::move(e));
        cur += 46u + nlen + elen + clen;
    }
    return !out.empty() || count == 0;
}

inline bool read_entry(const unsigned char* p, size_t n, const Entry& e,
                       std::string& out, size_t max_out) {
    if (e.method == 0) {   // stored
        if (e.usize > max_out) return false;
        out.assign(reinterpret_cast<const char*>(p + e.data_off), e.usize);
        return true;
    }
    if (e.method == 8) return inflate(p + e.data_off, e.csize, out, max_out);
    return false;   // 加密（method 99）等一律点名失败
}

}  // namespace zip

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
        return e != nullptr && zip::read_entry(p, n, *e, out, kEntryCap);
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
// filterr.h 的 HRESULT 语义值（新版 SDK 已裁掉 query 头，就地声明）：
constexpr long kFilterEEndOfChunks = static_cast<long>(0x8004177B);
constexpr long kFilterSLastChunk = static_cast<long>(0x0004177B);
constexpr long kFilterSLastText = static_cast<long>(0x0004177C);

// 最小 COM 面：只声明用到的 IFilter 方法与镜像 STAT_CHUNK（40 字节，字段偏移
// 与 filterr.h 一致）。GetChunk 的 chunkType 判定只用 flags 字段。
struct StatChunk {
    unsigned long id_chunk;
    unsigned long break_type;
    unsigned long flags;      // 1 = CHUNK_TEXT
    unsigned long locale;
    unsigned char attribute[16];
    unsigned long cwc_start_source;
    unsigned long cwc_len_source;
};

struct Ifilter : IUnknown {
    virtual long __stdcall Init(unsigned long flags, unsigned long mode,
                                unsigned long* elt_type, unsigned long* out_flags) = 0;
    virtual long __stdcall GetChunk(StatChunk* chunk) = 0;
    virtual long __stdcall GetText(unsigned long* count, wchar_t* buf) = 0;
    virtual long __stdcall GetValue(void** prop) = 0;
    virtual long __stdcall BindRegion(void*) = 0;
};

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
    Ifilter* f = static_cast<Ifilter*>(raw);
    unsigned long etype = 0, oflags = 0;
    if (f->Init(0, 0 /*STGM_READ*/, &etype, &oflags) < 0) {
        d.error = ext.substr(1) + " 的文本过滤器初始化失败";
        f->Release();
        return false;
    }
    bool last_text = false, last_chunk = false;
    while (!last_chunk && !last_text) {
        StatChunk ch{};
        const long hr = f->GetChunk(&ch);
        if (hr < 0 || hr == kFilterEEndOfChunks) break;
        last_chunk = hr == kFilterSLastChunk;   // LAST_CHUNK 仍携带有效 chunk
        if (ch.flags & 1) {
            for (;;) {
                unsigned long got = 4096;
                wchar_t buf[4096];
                const long tr = f->GetText(&got, buf);
                if (tr >= 0 && got > 0 && d.text.size() <= kTextCap)
                    append_utf16(d.text, buf, got);
                if (tr != 0) {   // 非 S_OK：LAST_TEXT（终块）或错误（本 chunk 无更多文本）
                    last_text = tr == kFilterSLastText;
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

}  // namespace vcws::docs
