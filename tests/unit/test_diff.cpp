// hunk diff 的单测（edit-diff 方案 §5）：新文件全 +、行号正确性、hunk 合并与切分、
// 前缀/后缀剥离与比较上限退化、无变化、清空、CRLF 容忍、统计尾行。
// 注：公共前缀/后缀只进 '=' op（供上下文延伸），hunk 头行号 = 绝对行号（1-based）。
#include "ithaca_test_framework.hpp"

#include "diff.hpp"
#include "text.hpp"

#include <string>

using vcws::make_diff;
using vcws::split_lines;

namespace {

std::string joined(int from, int to, const char* tag) {  // 构造 "tag1\ntag2\n..." 行集
    std::string s;
    for (int i = from; i <= to; ++i) s += tag + std::to_string(i) + "\n";
    return s;
}

}  // namespace

VC_TEST("diff: 新文件 = 全 + 单 hunk，无幻影空行") {
    const std::string d = make_diff("", "第一行\n第二行\n");
    CHECK(d.find("@@ -0,0 +1,2 @@") != std::string::npos);
    CHECK(d.find("1 + 第一行") != std::string::npos);
    CHECK(d.find("2 + 第二行") != std::string::npos);
    CHECK(d.find("- ") == std::string::npos);
    CHECK(d.find("（+2 −0 行）") != std::string::npos);
}

VC_TEST("diff: 行号语义——删除标旧行号，新增/上下文标新行号；上下文含前后缀") {
    // "第一行\n第二行\n" → "第一行\n第贰行\n"：1:1 替换，前后缀行作上下文；
    // 末尾幻影行归一后各 2 行
    const std::string d = make_diff("第一行\n第二行\n", "第一行\n第贰行\n");
    CHECK(d.find("@@ -1,2 +1,2 @@") != std::string::npos);
    CHECK(d.find("1   第一行") != std::string::npos);   // 上下文：新行号
    CHECK(d.find("2 - 第二行") != std::string::npos);   // 删除：旧行号
    CHECK(d.find("2 + 第贰行") != std::string::npos);   // 新增：新行号
    CHECK(d.find("3   ") == std::string::npos);         // 结束换行不算行
    CHECK(d.find("（+1 −1 行）") != std::string::npos);
}

VC_TEST("diff: 变更间隔 ≤ 2×上下文合并同一 hunk，> 则切分") {
    // 20 行改第 2 行与第 15 行：中间隔 12 个等行 > 6 ⇒ 两个 hunk
    std::string before, after;
    for (int i = 1; i <= 20; ++i) {
        before += "line" + std::to_string(i) + "\n";
        after += (i == 2 || i == 15 ? "CHANGED" + std::to_string(i) : "line" + std::to_string(i)) + "\n";
    }
    const std::string d = make_diff(before, after);
    CHECK(d.find("@@ -1,5 +1,5 @@") != std::string::npos);    // line1 + 改2 + ctx 3-5
    CHECK(d.find("@@ -12,7 +12,7 @@") != std::string::npos);  // ctx 12-14 + 改15 + ctx 16-18
    CHECK(d.find("15 - line15") != std::string::npos);
    CHECK(d.find("15 + CHANGED15") != std::string::npos);
    CHECK(d.find("（+2 −2 行）") != std::string::npos);

    // 改第 2 行与第 6 行：中间隔 3 个等行 ≤ 6 ⇒ 合并单 hunk
    std::string before2, after2;
    for (int i = 1; i <= 20; ++i) {
        before2 += "line" + std::to_string(i) + "\n";
        after2 += (i == 2 || i == 6 ? "CHANGED" + std::to_string(i) : "line" + std::to_string(i)) + "\n";
    }
    const std::string d2 = make_diff(before2, after2);
    CHECK(d2.find("@@ -1,9 +1,9 @@") != std::string::npos);   // line1 + 两处变更 + ctx 3-5,7-9
    CHECK(d2.find("2 - line2") != std::string::npos);
    CHECK(d2.find("6 + CHANGED6") != std::string::npos);
    CHECK(d2.find("@@ -12") == std::string::npos);            // 没有第二个 hunk
}

VC_TEST("diff: 大中段走 Myers——整块替换出完整 hunk（不再退化）") {
    // 前后各 50 行公共 + 中段 800:800 全不同：双侧 >256 ⇒ Myers 路径
    const std::string before = joined(1, 50, "head") + joined(1, 800, "old") + joined(1, 50, "tail");
    const std::string after = joined(1, 50, "head") + joined(1, 800, "new") + joined(1, 50, "tail");
    const std::string d = make_diff(before, after);
    CHECK(d.find("超出") == std::string::npos);
    // hunk 头：两侧各 3 行上下文 + 800 行变更（ctx 48-50 + 改 + ctx 851-853）
    CHECK(d.find("@@ -48,806 +48,806 @@") != std::string::npos);
    CHECK(d.find("51 - old1") != std::string::npos);      // old1 在 a 的绝对行 51
    CHECK(d.find("803 + new753") != std::string::npos);   // new753 在 b 的绝对行 803
    CHECK(d.find("851   tail1") != std::string::npos);    // 尾部上下文 3 行
    CHECK(d.find("（+800 −800 行）") != std::string::npos);
}

VC_TEST("diff: Myers 散布公共行——统计与 hunk 收敛正确") {
    // 600 行，每 50 行有 1 行公共（i%50==0 共 12 行）⇒ 588 行变更，
    // 变更间隔 1 行 ≤ 2×上下文 ⇒ 全部并入一个 hunk
    std::string before, after;
    for (int i = 0; i < 600; ++i) {
        before += (i % 50 == 0 ? "s" + std::to_string(i) : "old" + std::to_string(i)) + "\n";
        after += (i % 50 == 0 ? "s" + std::to_string(i) : "new" + std::to_string(i)) + "\n";
    }
    const std::string d = make_diff(before, after);
    CHECK(d.find("超出") == std::string::npos);
    CHECK(d.find("（+588 −588 行）") != std::string::npos);
    CHECK(d.find("s50") != std::string::npos);      // 公共行保留为上下文
    CHECK(d.find("old1") != std::string::npos);
    CHECK(d.find("new599") != std::string::npos);
    std::size_t hunks = 0;
    for (std::size_t pos = d.find("\n@@"); pos != std::string::npos;
         pos = d.find("\n@@", pos + 1))
        ++hunks;                                    // 每个 hunk 头占一行（行首 @@）
    if (d.compare(0, 2, "@@") == 0) ++hunks;        // 首行就是 hunk 头
    CHECK(hunks == 1);
}

VC_TEST("diff: Myers 纯插入/纯删除大段（单侧 >256 行）") {
    std::string base;
    for (int i = 1; i <= 20; ++i) base += "line" + std::to_string(i) + "\n";
    std::string extra;
    for (int i = 1; i <= 300; ++i) extra += "x" + std::to_string(i) + "\n";
    const std::string d = make_diff(base, base + extra);   // 尾部插 300 行
    CHECK(d.find("（+300 −0 行）") != std::string::npos);
    CHECK(d.find("21 + x1") != std::string::npos);
    const std::string d2 = make_diff(extra + base, base);  // 头部删 300 行
    CHECK(d2.find("（+0 −300 行）") != std::string::npos);
    CHECK(d2.find("1 - x1") != std::string::npos);
}

VC_TEST("diff: 中段超 Myers 上限（总行数 25000）退化为统计摘要") {
    // 前后各 50 行公共 + 中段 13000:13000 ⇒ 总行数 26000 > 25000 ⇒ 退化
    const std::string before = joined(1, 50, "head") + joined(1, 13000, "old") + joined(1, 50, "tail");
    const std::string after = joined(1, 50, "head") + joined(1, 13000, "new") + joined(1, 50, "tail");
    const std::string d = make_diff(before, after);
    CHECK(d.find("超出比较上限") != std::string::npos);
    CHECK(d.find("13100 行 → 13100 行") != std::string::npos);
    CHECK(d.find("@@") == std::string::npos);  // 不产 hunk
}

VC_TEST("diff: 无变化与清空") {
    CHECK(make_diff("a\nb\n", "a\nb\n") == "（无变化）\n");
    CHECK(make_diff("", "") == "（无变化）\n");
    const std::string d = make_diff("a\nb\n", "");
    // 末尾幻影行归一："a\nb\n" 是 2 行内容 + 结束换行，清空 = 2 行删除
    CHECK(d.find("@@ -1,2 +0,0 @@") != std::string::npos);
    CHECK(d.find("1 - a") != std::string::npos);
    CHECK(d.find("2 - b") != std::string::npos);
    CHECK(d.find("+ ") == std::string::npos);
    CHECK(d.find("（+0 −2 行）") != std::string::npos);
}

VC_TEST("diff: CRLF 容忍——切行剥 \\r，输出不含 \\r") {
    const std::string d = make_diff("alpha\r\nbeta\r\n", "alpha\r\ngamma\r\n");
    CHECK(d.find('\r') == std::string::npos);
    CHECK(d.find("1   alpha") != std::string::npos);
    CHECK(d.find("2 - beta") != std::string::npos);
    CHECK(d.find("2 + gamma") != std::string::npos);
}

VC_TEST("diff: 行号列 4 位右对齐（≥1000 行可读）") {
    // 1200 行改第 1050 行：删除行行号占满 4 列
    std::string before, after;
    for (int i = 1; i <= 1200; ++i) {
        before += "line" + std::to_string(i) + "\n";
        after += (i == 1050 ? "CHANGED" : "line" + std::to_string(i)) + std::string("\n");
    }
    const std::string d = make_diff(before, after);
    CHECK(d.find("1050 - line1050") != std::string::npos);
    CHECK(d.find("1050 + CHANGED") != std::string::npos);
}

VC_TEST("diff: utf8_change_span——变化子串按码点对齐（行内高亮的底座）") {
    std::string pre, ma, mb, suf;
    vcws::utf8_change_span("任务一：稳定前缀 + 一两天可落地 |",
                           "任务一：稳定前缀 + ✅ 已落地（§八） |", pre, ma, mb, suf);
    CHECK(pre == "任务一：稳定前缀 + ");
    CHECK(ma == "一两天可落地");
    CHECK(mb == "✅ 已落地（§八）");
    CHECK(suf == " |");
    CHECK(pre + ma + suf == "任务一：稳定前缀 + 一两天可落地 |");
    CHECK(pre + mb + suf == "任务一：稳定前缀 + ✅ 已落地（§八） |");

    // 字节级失配落在多字节字符中间（"い" vs "う" 共享前两字节）⇒ 前缀收缩到码点
    vcws::utf8_change_span("\xE3\x81\x82\xE3\x81\x84", "\xE3\x81\x82\xE3\x81\x86", pre, ma, mb,
                           suf);
    CHECK(pre == "\xE3\x81\x82");  // あ
    CHECK(ma == "\xE3\x81\x84");   // い
    CHECK(mb == "\xE3\x81\x86");   // う
    CHECK(suf.empty());

    // 完全不同：前缀/后缀均空，mid 为全串
    vcws::utf8_change_span("abc", "xyz", pre, ma, mb, suf);
    CHECK(pre.empty() && suf.empty() && ma == "abc" && mb == "xyz");
}

VC_TEST("diff: split_lines 语义——末尾空串算一行、CRLF 剥离") {
    const auto a = split_lines("x\ny\n");
    REQUIRE(a.size() == 3);
    CHECK(a[0] == "x" && a[1] == "y" && a[2].empty());
    const auto b = split_lines("x\r\ny");
    REQUIRE(b.size() == 2);
    CHECK(b[0] == "x" && b[1] == "y");
}
