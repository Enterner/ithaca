// 路径 jail 的单测：合法形态折叠、根外解析为 outside 标记（墙→闸）、危险形态
// 点名拒绝、规范化复核。
#include "ithaca_test_framework.hpp"

#include <vckit/path_jail.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using Jail = vckit::PathJail;

namespace {

fs::path make_temp_root() {
    const fs::path root = test_root("ithaca_m1_jail_test");   // 清空+建目录走框架共用件
    std::error_code ec;
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

    // ".." 不再一刀切拒：词法折叠回根内的按普通相对路径放行（墙→闸）
    REQUIRE(jail.resolve("sub/../a.txt", full, norm, err));
    CHECK(norm == "a.txt");

    // 绝对路径在根内 = 放行（模型常把工具输出里的绝对形态原样送回），剥根前缀
    // 转相对；盘符大小写差异随前缀一起剥掉
    REQUIRE(jail.resolve((root / "a.txt").string(), full, norm, err));
    CHECK(norm == "a.txt");
}

VC_TEST("jail: 根外解析为 outside 标记（墙→闸），把关上移权限模式层") {
    const fs::path root = make_temp_root();
    const Jail jail{root.wstring()};

    // 绝对根外（盘符/根斜杠/UNC）与 .. 上溢：解析为自身，标 outside，
    // norm = '/' 分隔的规范化绝对形态（模型可见可复用，永不含 ".."）
    std::wstring full;
    std::string norm, err;
    bool outside = false;
    REQUIRE(jail.resolve("C:/x", full, norm, err, &outside));
    CHECK(outside);
    CHECK(norm == "C:/x");
    CHECK(fs::path(full).is_absolute());

    REQUIRE(jail.resolve("\\\\srv\\share", full, norm, err, &outside));
    CHECK(outside);
    CHECK(norm == "//srv/share");   // 纯词法解析，无网络访问

    REQUIRE(jail.resolve("/abs", full, norm, err, &outside));
    CHECK(outside);   // 当前盘根下的 /abs：与临时根不重叠 ⇒ 必在根外

    REQUIRE(jail.resolve("../outside", full, norm, err, &outside));
    CHECK(outside);
    CHECK(norm.find("..") == std::string::npos);   // canonical 已折叠上溢
    CHECK(fs::path(full) == root.parent_path() / "outside");

    // 聚合出参同语义：outside 标志随 Resolved 走
    Jail::Resolved r;
    REQUIRE(jail.resolve("C:/x", r));
    CHECK(r.outside);
    CHECK(jail.resolve("a.txt", r));
    CHECK(!r.outside);
}

VC_TEST("jail: 危险形态仍被点名拒绝（Windows 正确性防线，与策略无关）") {
    const fs::path root = make_temp_root();
    const Jail jail{root.wstring()};
    const auto rejected = [&](const std::string& rel) {
        std::wstring full;
        std::string norm, err;
        bool outside = false;
        return jail.resolve(rel, full, norm, err, &outside) ? std::string("意外被接受") : err;
    };

    // 盘符相对形态语义含混（相对该盘符当前目录），在根判定前明确不支持
    CHECK(rejected("C:foo").find("盘符相对") != std::string::npos);
    // 段卫生对根内根外一体适用：Windows 静默剥离 / 保留设备名是正确性防线
    CHECK(rejected("a.").find("静默剥离") != std::string::npos);
    CHECK(rejected("a ").find("静默剥离") != std::string::npos);
    CHECK(rejected("C:/x/a.").find("静默剥离") != std::string::npos);
    CHECK(rejected("CON").find("保留设备名") != std::string::npos);
    CHECK(rejected("sub/COM1").find("保留设备名") != std::string::npos);
    CHECK(rejected("C:/x/CON").find("保留设备名") != std::string::npos);
    CHECK(rejected(std::string("a\tb")).find("控制字符") != std::string::npos);
    CHECK(rejected("").find("为空") != std::string::npos);
}

VC_TEST("jail: 在根解析结果始终落在根内（规范化复核）") {
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
