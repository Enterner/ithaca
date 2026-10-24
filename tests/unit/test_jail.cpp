// 路径 jail 的单测：合法形态折叠、危险形态点名拒绝、规范化复核。
#include "ithaca_test_framework.hpp"

#include "jail.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using vcws::Jail;

namespace {

fs::path make_temp_root() {
    std::error_code ec;
    // 测试根放产品仓内：TEMP 在本开发环境对部分子进程的建目录会 Access denied，
    // 工作区路径确定可用；每次运行先清空，互不污染。
    const fs::path root =
        fs::path(__FILE__).parent_path() / ".tmp" / "ithaca_m1_jail_test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "sub" / "nested", ec);
    { std::ofstream(root / "a.txt").put('x'); }
    return root;
}

}  // namespace

VC_TEST("jail: 合法相对路径被接受并折叠") {
    const fs::path root = make_temp_root();
    const Jail jail{root.wstring()};
    std::wstring full;
    std::string norm, err;

    REQUIRE(jail.resolve("sub/nested", full, norm, err));
    CHECK(norm == "sub/nested");
    CHECK(fs::exists(full));  // 解析结果指向真实目录，且在根内

    REQUIRE(jail.resolve("./sub//nested/", full, norm, err));  // 点段/空段/尾分隔符折叠
    CHECK(norm == "sub/nested");

    // 绝对路径在根内 = 放行（模型常把工具输出里的绝对形态原样送回），剥根前缀
    // 转相对；盘符大小写差异随前缀一起剥掉
    REQUIRE(jail.resolve((root / "a.txt").string(), full, norm, err));
    CHECK(norm == "a.txt");
}

VC_TEST("jail: 越界与危险形态被点名拒绝") {
    const fs::path root = make_temp_root();
    const Jail jail{root.wstring()};
    const auto rejected = [&](const std::string& rel) {
        std::wstring full;
        std::string norm, err;
        return jail.resolve(rel, full, norm, err) ? std::string("意外被接受") : err;
    };

    CHECK(rejected("../outside").find("越界") != std::string::npos);
    CHECK(rejected("a/../b").find("'..'") != std::string::npos);
    CHECK(rejected("a/../../b").find("'..'") != std::string::npos);
    // 盘符/根斜杠/UNC：根外一律按越界拒绝（jail 边界不松，只放开根内绝对形态）
    CHECK(rejected("C:/x").find("越出工作区根") != std::string::npos);
    CHECK(rejected("\\\\srv\\share").find("越出工作区根") != std::string::npos);
    CHECK(rejected("/abs").find("越出工作区根") != std::string::npos);
    CHECK(rejected("a.").find("静默剥离") != std::string::npos);
    CHECK(rejected("a ").find("静默剥离") != std::string::npos);
    CHECK(rejected("CON").find("保留设备名") != std::string::npos);
    CHECK(rejected("sub/COM1").find("保留设备名") != std::string::npos);
    CHECK(rejected(std::string("a\tb")).find("控制字符") != std::string::npos);
    CHECK(rejected("").find("为空") != std::string::npos);
}

VC_TEST("jail: 解析结果始终落在根内（规范化复核）") {
    const fs::path root = make_temp_root();
    const Jail jail{root.wstring()};
    std::wstring full;
    std::string norm, err;

    // 大小写变体仍落在根内（Windows 文件系统大小写不敏感）
    REQUIRE(jail.resolve("SUB/NESTED", full, norm, err));
    CHECK(fs::exists(full));

    // 折叠后的 norm 永远不含 ".." 与空段——这是给快照/展示用的稳定形态
    REQUIRE(jail.resolve("a/b/c/./d", full, norm, err));
    CHECK(norm.find("..") == std::string::npos);
    CHECK(norm.find("//") == std::string::npos);
}
