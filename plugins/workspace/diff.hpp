#pragma once
// 行级 diff（展示用）：从 before/after 生成 hunk 集——修改邻域 + 上下文 3 行 + 行号
// （展示形态对齐 ZCode 的工具 diff）。
// LLM 不产出 diff（行号补丁是实测重灾区）——展示与生成分离，格式可独立演进。
//
// 对齐策略（对齐 ZCode/git diff 的 Myers 家族）：
//   1. 行级切分（容忍 CRLF）；空串按"零行"处理（新建文件 = 全 +，不产幻影行）；
//   2. LCS 只算变化中段（公共前缀/后缀直接作 '=' op 拼进序列——上下文因此能
//      延伸进前缀/后缀区，小文件不会丢上下文）；
//   3. 中段双侧各 ≤256 行 ⇒ 小 DP 表（65k 格，微秒级）；否则线性空间 Myers
//      （空间 O(N+M)、时间 O((N+M)·D)——代价随**编辑量** D 涨而非文件/变化区
//      大小，大块替换照样出完整 hunk）；中段总行数超 25000（远超任何真实编辑）
//      才退化为统计摘要；
//   4. 相邻变更间隔 ≤ 2×上下文 ⇒ 合并同一 hunk。
//
// 行格式（tui/webui 按此解析着色）：
//   "@@ -a,n +b,m @@"   hunk 头（1-based；- 行标旧行号，+/上下文标新行号）
//   "NNNN - 内容"        删除（NNNN = 4 列右对齐旧行号）
//   "NNNN + 内容"        新增
//   "NNNN   内容"        上下文（标记位为空格）
//   "（+A −R 行）"       文件级统计尾行

#include <cstdint>
#include <string>
#include <vector>

namespace vcws {

inline std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (const char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    lines.push_back(cur);  // 与"以 \n 结尾"的语义一致：末尾空串算一行
    return lines;
}

namespace diff_detail {

struct DiffOp {
    char kind;           // '=' / '-' / '+'
    std::size_t ai = 0;  // kind != '+' 时的 a 序列下标（绝对）
    std::size_t bi = 0;  // kind != '-' 时的 b 序列下标（绝对）
};

inline std::string num4(std::size_t n) {
    std::string s = std::to_string(n);
    return s.size() >= 4 ? s : std::string(4 - s.size(), ' ') + s;
}

// 线性空间 Myers 的共享上下文：V 数组按顶层规模分配一次，递归各层串行复用
struct MyersCtx {
    const std::vector<std::string>* a = nullptr;
    const std::vector<std::string>* b = nullptr;
    std::size_t base = 0;            // 中段在 a/b 中的绝对起始（= 公共前缀长 p）
    std::vector<DiffOp>* out = nullptr;
    std::vector<long long> vf, vr;   // k 对角线前进量
    long long voff = 0;              // k=0 的槽位偏移
    bool eq(std::size_t i, std::size_t j) const {
        return (*a)[base + i] == (*b)[base + j];
    }
    void emit(char kind, std::size_t ai, std::size_t bi) const {
        out->push_back({kind, ai, bi});
    }
};

// 在 [alo,ahi)×[blo,bhi)（中段局部坐标）上找"中蛇"：返回蛇的起止 (sx,sy)-(ex,ey)
// 与总编辑距离 D；返回 d<0 表示理论不可达的失败（调用方走防御性脚本）
inline void middle_snake(MyersCtx& c, std::size_t alo, std::size_t ahi, std::size_t blo,
                         std::size_t bhi, std::size_t& sx, std::size_t& sy, std::size_t& ex,
                         std::size_t& ey, long long& d) {
    const long long n = static_cast<long long>(ahi - alo);
    const long long m = static_cast<long long>(bhi - blo);
    const long long delta = n - m;
    const bool odd = (delta & 1) != 0;
    const long long off = c.voff;
    const auto A = [&](long long x) { return (*c.a)[c.base + alo + static_cast<std::size_t>(x)]; };
    const auto B = [&](long long y) { return (*c.b)[c.base + blo + static_cast<std::size_t>(y)]; };
    c.vf[off + 1] = 0;
    c.vr[off + 1] = 0;
    for (long long D = 0; D <= (n + m + 1) / 2; ++D) {
        for (long long k = -D; k <= D; k += 2) {  // 正向
            long long x;
            if (k == -D || (k != D && c.vf[off + k - 1] < c.vf[off + k + 1]))
                x = c.vf[off + k + 1];
            else
                x = c.vf[off + k - 1] + 1;
            long long y = x - k;
            const long long px = x, py = y;
            while (x < n && y < m && A(x) == B(y)) { ++x; ++y; }
            c.vf[off + k] = x;
            if (odd && k >= delta - (D - 1) && k <= delta + (D - 1) &&
                c.vf[off + k] + c.vr[off + delta - k] >= n) {
                sx = static_cast<std::size_t>(px);
                sy = static_cast<std::size_t>(py);
                ex = static_cast<std::size_t>(x);
                ey = static_cast<std::size_t>(y);
                d = D;
                return;
            }
        }
        for (long long k = -D; k <= D; k += 2) {  // 反向
            long long x;
            if (k == -D || (k != D && c.vr[off + k - 1] < c.vr[off + k + 1]))
                x = c.vr[off + k + 1];
            else
                x = c.vr[off + k - 1] + 1;
            long long y = x - k;
            const long long px = x, py = y;
            while (x < n && y < m && A(n - 1 - x) == B(m - 1 - y)) { ++x; ++y; }
            c.vr[off + k] = x;
            // 反向路径在反向对角线 k 上 ⇒ 正向对角线 delta - k；正向已走 D 步，
            // 同一对角线相遇即 vr[k] + vf[delta - k] ≥ n（窗口保证 delta-k ∈ [-D, D]）
            if (!odd && k + D >= delta && k - D <= delta &&
                c.vr[off + k] + c.vf[off + delta - k] >= n) {
                // 反向蛇换算回正向坐标：(n-x, m-y) → (n-px, m-py)
                sx = static_cast<std::size_t>(n - x);
                sy = static_cast<std::size_t>(m - y);
                ex = static_cast<std::size_t>(n - px);
                ey = static_cast<std::size_t>(m - py);
                d = D;
                return;
            }
        }
    }
    d = -1;  // 论文保证 D ≤ ⌈(n+m)/2⌉ 必命中——防御性兜底
}

// 中蛇分治：产出 [alo,ahi)×[blo,bhi) 的对齐 ops。sx/sy/ex/ey 是**子区间局部**坐标，
// 发 op 时必须平移回中段绝对下标（base + alo / base + blo）——漏平移时顶层碰巧正确、
// 嵌套递归内容全错位（踩过：上下文行显示文件开头的行）。op 顺序严格单调。
inline void myers_rec(MyersCtx& c, std::size_t alo, std::size_t ahi, std::size_t blo,
                      std::size_t bhi) {
    const std::size_t n = ahi - alo, m = bhi - blo;
    if (n == 0) {
        for (std::size_t j = blo; j < bhi; ++j) c.emit('+', 0, c.base + j);
        return;
    }
    if (m == 0) {
        for (std::size_t i = alo; i < ahi; ++i) c.emit('-', c.base + i, 0);
        return;
    }
    std::size_t sx, sy, ex, ey;
    long long d;
    middle_snake(c, alo, ahi, blo, bhi, sx, sy, ex, ey, d);
    if (d < 0) {  // 防御：产出正确（非最优）脚本，绝不产错内容
        for (std::size_t i = alo; i < ahi; ++i) c.emit('-', c.base + i, 0);
        for (std::size_t j = blo; j < bhi; ++j) c.emit('+', 0, c.base + j);
        return;
    }
    if (d <= 1) {
        // 编辑距离 ≤1：贪心对齐（前缀 '=' → 恰一处删/插 → 尾缀 '='）。
        // D=1 只可能是纯插/删（替换 = 删+插 = D2）；二者恰一成立（swap 是 D2）。
        // i/j 从子区间起点走，本身就是中段绝对下标
        std::size_t i = alo, j = blo;
        while (i < ahi && j < bhi && c.eq(i, j)) {
            c.emit('=', c.base + i, c.base + j);
            ++i;
            ++j;
        }
        if (i < ahi && j < bhi) {
            const auto rest_eq = [&](std::size_t i2, std::size_t j2) {
                if (ahi - i2 != bhi - j2) return false;
                while (i2 < ahi) {
                    if (!c.eq(i2, j2)) return false;
                    ++i2;
                    ++j2;
                }
                return true;
            };
            if (rest_eq(i + 1, j)) {
                c.emit('-', c.base + i, 0);
                ++i;
            } else {
                c.emit('+', 0, c.base + j);
                ++j;
            }
        }
        while (i < ahi && j < bhi && c.eq(i, j)) {
            c.emit('=', c.base + i, c.base + j);
            ++i;
            ++j;
        }
        while (i < ahi) { c.emit('-', c.base + i, 0); ++i; }
        while (j < bhi) { c.emit('+', 0, c.base + j); ++j; }
        return;
    }
    myers_rec(c, alo, alo + sx, blo, blo + sy);
    for (std::size_t t = 0; t < ex - sx; ++t)
        c.emit('=', c.base + alo + sx + t, c.base + blo + sy + t);
    myers_rec(c, alo + ex, ahi, blo + ey, bhi);
}

}  // namespace diff_detail

inline std::string make_diff(const std::string& before, const std::string& after,
                             std::size_t* out_added = nullptr,
                             std::size_t* out_removed = nullptr) {
    using namespace diff_detail;
    // 空串 = 零行（新建/清空不产幻影空行；split_lines 的末尾空串只对"有内容"成立）
    std::vector<std::string> a =
        before.empty() ? std::vector<std::string>{} : split_lines(before);
    std::vector<std::string> b =
        after.empty() ? std::vector<std::string>{} : split_lines(after);

    // 末尾幻影行归一：split_lines 把"以 \n 结尾"表示成末尾空串行。两侧同态
    //（都带或一方无内容）⇒ 各剥一个——"文件正常结束换行"不该算一行变更（POSIX
    // diff 同款直觉）；仅一侧带 ⇒ 保留（真实的"结尾换行增删"变更）
    const bool a_end = !a.empty() && a.back().empty();
    const bool b_end = !b.empty() && b.back().empty();
    if (a_end && (b.empty() || b_end)) a.pop_back();
    if (b_end && (a.empty() || a_end)) b.pop_back();

    const std::size_t n = a.size(), m = b.size();

    std::size_t p = 0;  // 公共前缀
    while (p < n && p < m && a[p] == b[p]) ++p;
    std::size_t sfx = 0;  // 公共后缀（不与前缀重叠）
    while (sfx < n - p && sfx < m - p && a[n - 1 - sfx] == b[m - 1 - sfx]) ++sfx;
    const std::size_t na = n - p - sfx, nb = m - p - sfx;

    if (na == 0 && nb == 0) {
        if (out_added) *out_added = 0;
        if (out_removed) *out_removed = 0;
        return "（无变化）\n";
    }

    // 中段规模闸门：小走 DP，大走 Myers（线性空间）；超过 Myers 上限（远超任何
    // 真实编辑的整文件重写）才退化为统计摘要
    constexpr std::size_t kDiffDpSide = 256;     // 双侧各 ≤256 行 ⇒ 65k 格 DP，微秒级
    constexpr std::size_t kDiffMyersSum = 25000; // 中段总行数上限
    const bool dp_small = na <= kDiffDpSide && nb <= kDiffDpSide;
    if (!dp_small && na + nb > kDiffMyersSum) {
        // 计数退化：hunk 没算，给不出 +/− 明细——给净变化近似（统计行本身就是
        // "仅统计"口径），不让消费方拿到默认 0 当真值
        if (out_added) *out_added = m > n ? m - n : 0;
        if (out_removed) *out_removed = n > m ? n - m : 0;
        return "（超出比较上限（中段 " + std::to_string(na) + "+" + std::to_string(nb) +
               " 行），仅统计：共 " + std::to_string(n) + " 行 → " + std::to_string(m) +
               " 行）\n";
    }

    // ---- op 序列：前缀 '=' + 中段对齐 + 后缀 '='（下标绝对）----------------------
    std::vector<DiffOp> ops;
    ops.reserve(n + m);
    for (std::size_t k = 0; k < p; ++k) ops.push_back({'=', k, k});
    if (dp_small) {
        std::vector<std::uint32_t> dp((na + 1) * (nb + 1), 0);
        const auto at = [nb](std::size_t i, std::size_t j) { return i * (nb + 1) + j; };
        for (std::size_t i = na; i-- > 0;)
            for (std::size_t j = nb; j-- > 0;)
                dp[at(i, j)] = a[p + i] == b[p + j]
                                   ? dp[at(i + 1, j + 1)] + 1
                                   : (dp[at(i + 1, j)] >= dp[at(i, j + 1)] ? dp[at(i + 1, j)]
                                                                           : dp[at(i, j + 1)]);
        std::size_t i = 0, j = 0;
        while (i < na && j < nb) {
            if (a[p + i] == b[p + j]) {
                ops.push_back({'=', p + i, p + j});
                ++i;
                ++j;
            } else if (dp[at(i + 1, j)] >= dp[at(i, j + 1)]) {
                ops.push_back({'-', p + i, 0});
                ++i;
            } else {
                ops.push_back({'+', 0, p + j});
                ++j;
            }
        }
        while (i < na) ops.push_back({'-', p + i++, 0});
        while (j < nb) ops.push_back({'+', 0, p + j++});
    } else {
        diff_detail::MyersCtx ctx;
        ctx.a = &a;
        ctx.b = &b;
        ctx.base = p;
        ctx.out = &ops;
        ctx.voff = static_cast<long long>(na + nb) + 1;
        ctx.vf.assign(2 * (na + nb) + 4, 0);
        ctx.vr.assign(2 * (na + nb) + 4, 0);
        diff_detail::myers_rec(ctx, 0, na, 0, nb);
    }
    for (std::size_t k = n - sfx; k < n; ++k)
        ops.push_back({'=', k, m - sfx + (k - (n - sfx))});

    // ---- 变更分组 → hunk（间隔 ≤ 2×ctx 的相邻变更合并）--------------------------
    constexpr std::size_t kCtx = 3;
    std::vector<std::size_t> changes;  // ops 中非 '=' 的下标
    for (std::size_t k = 0; k < ops.size(); ++k)
        if (ops[k].kind != '=') changes.push_back(k);

    struct Hunk {
        std::size_t lo, hi;  // ops 区间（含）
    };
    std::vector<Hunk> hunks;
    for (std::size_t c = 0; c < changes.size();) {
        std::size_t e = c;
        while (e + 1 < changes.size() &&
               changes[e + 1] - changes[e] - 1 <= 2 * kCtx)  // 中间 '=' 行数
            ++e;
        std::size_t lo = changes[c], hi = changes[e];
        std::size_t taken = 0;  // 向前扩上下文（gap > 2*ctx 保证不越入上一 hunk）
        while (lo > 0 && ops[lo - 1].kind == '=' && taken < kCtx) --lo, ++taken;
        taken = 0;  // 向后扩
        while (hi + 1 < ops.size() && ops[hi + 1].kind == '=' && taken < kCtx) ++hi, ++taken;
        if (hunks.empty() || lo > hunks.back().hi) hunks.push_back({lo, hi});
        else hunks.back().hi = hi;  // 防御：理论上 gap 约束保证不相交，重叠则并入
        c = e + 1;
    }

    std::string out;
    std::size_t old_no = 1, new_no = 1, added = 0, removed = 0;
    std::size_t h = 0;
    const auto advance = [&](const DiffOp& o) {
        if (o.kind == '=') { ++old_no; ++new_no; }
        else if (o.kind == '-') { ++old_no; }
        else { ++new_no; }
    };
    for (std::size_t k = 0; k < ops.size(); ++k) {
        const bool in_hunk = h < hunks.size() && k >= hunks[h].lo;
        if (in_hunk && k == hunks[h].lo) {  // hunk 头：区间内的旧行数 / 新行数
            std::size_t oc = 0, nc = 0;
            for (std::size_t t = hunks[h].lo; t <= hunks[h].hi; ++t) {
                if (ops[t].kind != '+') ++oc;
                if (ops[t].kind != '-') ++nc;
            }
            // 空侧行号按 unified 惯例取 0（新建 = -0,0；清空 = +0,0）
            out += "@@ -" + std::to_string(oc == 0 ? old_no - 1 : old_no) + "," +
                   std::to_string(oc) + " +" +
                   std::to_string(nc == 0 ? new_no - 1 : new_no) + "," +
                   std::to_string(nc) + " @@\n";
        }
        const DiffOp& o = ops[k];
        if (in_hunk) {
            if (o.kind == '=') {
                out += num4(new_no) + "   " + a[o.ai] + "\n";
            } else if (o.kind == '-') {
                out += num4(old_no) + " - " + a[o.ai] + "\n";
                ++removed;
            } else {
                out += num4(new_no) + " + " + b[o.bi] + "\n";
                ++added;
            }
        }
        advance(o);
        if (in_hunk && k == hunks[h].hi) ++h;
    }
    out += "（+" + std::to_string(added) + " −" + std::to_string(removed) + " 行）\n";
    if (out_added) *out_added = added;
    if (out_removed) *out_removed = removed;
    return out;
}

}  // namespace vcws
