#pragma once
// ZIP 容器底座（plugin/office 的抽取基建切片）：EOCD 尾扫 → central directory
// 定位条目 + RFC 1951 raw DEFLATE 解码（puff 形态：counts/symbols 两级表，逐位
// 游走）。与文档语义零耦合——上层（docs.hpp 的 OOXML 扫描）只按条目名取字节流。
// 不引第三方依赖：抽取目标只是正文文本，输入面收敛为「Office 写出的合法文件」，
// 坏文件点名失败即可（不做容错续读）。
//
// 上限：单条目解压 kEntryCap——同时是 zip 爆炸（decompression bomb）的解压护栏。

#include <string>
#include <vector>

namespace vcoffice::zip {

inline constexpr std::size_t kEntryCap = 32ull * 1024 * 1024;   // 单条目解压上限

// RFC 1951 原始 DEFLATE 解码。返回 false = 数据损坏 / 超 max_out。
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

}  // namespace vcoffice::zip
