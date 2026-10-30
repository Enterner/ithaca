// read 分页管线的单测：workspace 文本 read 与 office 文档 read 共用的出参形状
//（vcws::read_paged——形状分叉即事故：agent 摘要分派、前端徽标都消费它）。
// 锁：字段齐全性、行号对齐、total_lines 口径、超长行字符截断、truncated 语义、
// 参数钳制与 256KB 护栏。
#include "ithaca_test_framework.hpp"

#include "vcws/read_paged.hpp"

#include <string>

using nlohmann::json;
using vcws::kReadLineCharCap;
using vcws::read_paged;
using vcws::trim_utf8_chars;

namespace {

json run(const std::string& content, int offset, int limit) {
    return read_paged(std::string(content), "src/a.txt",
                      json{{"offset", offset}, {"limit", limit}}, false, "");
}

}  // namespace

VC_TEST("read_paged: 出参形状逐字段齐全（双路一致性的根基）") {
    const json r = run("第一行\n第二行\n", 1, 10);
    CHECK(r.value("ok", false));
    CHECK(r.value("path", "") == "src/a.txt");
    CHECK(r.contains("content") && r["content"].is_string());
    CHECK(r.contains("truncated") && r["truncated"].is_boolean());
    CHECK(r.value("total_lines", -1) == 2);
    CHECK(r.value("offset", -1) == 1);
    CHECK(r.value("limit", -1) == 10);
    CHECK(!r.contains("doc"));   // 文本路无 doc 徽标
}

VC_TEST("read_paged: 行号 = 文件真实行号，随 offset 对齐；truncated 判定") {
    std::string c;
    for (int i = 1; i <= 5; ++i) c += "L" + std::to_string(i) + "\n";
    const json r = run(c, 3, 2);
    CHECK(r.value("content", "") == "3\tL3\n4\tL4\n");
    CHECK(r.value("truncated", false) == true);   // 3+2 ≤ 5：还有第 5 行未读

    const json all = run(c, 1, 2000);
    CHECK(all.value("truncated", true) == false);   // 一次读全：无截断
}

VC_TEST("read_paged: total_lines 口径——尾换行不算行，无尾换行算行，空文件 0 行") {
    CHECK(run("a\nb\n", 1, 10).value("total_lines", -1) == 2);
    CHECK(run("a\nb", 1, 10).value("total_lines", -1) == 2);
    const json empty = read_paged(std::string(), "e.txt", json{{"offset", 1}, {"limit", 10}}, false, "");
    CHECK(empty.value("total_lines", -1) == 0);
    CHECK(empty.value("content", "x") == "");
    CHECK(empty.value("truncated", true) == false);
}

VC_TEST("read_paged: 末行无换行不凭空造行；行内 \\r 原样保留") {
    CHECK(run("a\nb", 2, 10).value("content", "") == "2\tb");
    CHECK(run("a\r\nb\r\n", 1, 10).value("content", "") == "1\ta\r\n2\tb\r\n");
}

VC_TEST("read_paged: 超长行按字符数截断（码点边界），换行归属保真") {
    // 中文 3 字节/字：2500 字 ≈ 7500 字节 > 2000 字符上限——按字符截而非按字节
    //（字节口径会让模型拿到半截锚点，replace 必失败）
    std::string cn;
    for (int i = 0; i < 2500; ++i) cn += "中";
    // 带换行的行：截断体 = 2000 字符 × 3 字节 = 6000 字节（字符口径的直接证据；
    // 字节口径会是 2000 字节 + 半个多字节残片），行尾换行保真、无截断标记
    const json r = run(cn + "\n短行\n", 1, 10);
    CHECK(r.value("content", "") == "1\t" + cn.substr(0, kReadLineCharCap * 3) + "\n2\t短行\n");
    CHECK(r.value("truncated", false) == true);
    // 无换行的末行：截断以「…（行尾截断）」收尾（替代缺失的换行作视觉终止）
    const json last = run("短行\n" + cn, 2, 10);
    CHECK(last.value("content", "") ==
          "2\t" + cn.substr(0, kReadLineCharCap * 3) + "…（行尾截断）");
    CHECK(last.value("truncated", false) == true);
}

VC_TEST("read_paged: trim_utf8_chars——字节零开销快路径与 CJK 字符口径") {
    const std::string ascii(100, 'x');
    const auto [s1, cut1] = trim_utf8_chars(ascii, 200);
    CHECK(!cut1 && s1 == ascii);   // 字节数 ≤ cap：原样零开销
    std::string cn3;
    for (int i = 0; i < 10; ++i) cn3 += "中";   // 30 字节 = 10 字符
    const auto [s2, cut2] = trim_utf8_chars(cn3, 5);
    CHECK(cut2);
    CHECK(s2.size() == 15);        // 5 个字符 = 15 字节（不是 5 字节——码点边界）
}

VC_TEST("read_paged: 参数钳制与越界——offset<1 归 1、limit 钳 1..2000、越界空内容") {
    std::string c;
    for (int i = 1; i <= 3; ++i) c += "L\n";
    CHECK(run(c, 0, 10).value("offset", -1) == 1);       // offset ≥ 1
    CHECK(run(c, 1, 0).value("limit", -1) == 1);         // limit ≥ 1
    CHECK(run(c, 1, 99999).value("limit", -1) == 2000);  // limit ≤ 2000
    const json beyond = run(c, 99, 10);                  // 越界：空内容不崩
    CHECK(beyond.value("content", "x") == "");
    CHECK(beyond.value("truncated", true) == false);
}

VC_TEST("read_paged: 256KB 内容护栏——超限 trim 并标记 truncated") {
    std::string c;   // 2000 行 × 150 字节 = 300KB > kReadCap(256KB)，limit 钳在 2000 行内
    for (int i = 0; i < 2000; ++i) c += std::string(149, 'a') + "\n";
    const json r = run(c, 1, 2000);
    CHECK(r.value("content", "").size() <= 256 * 1024);
    CHECK(r.value("truncated", false) == true);
}
