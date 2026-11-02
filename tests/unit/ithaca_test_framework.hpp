#pragma once
// ithaca 微型断言框架（极简自注册形态，仅 CHECK 断言一个概念）：
// 产品与插件的测试**不进内核仓**（内核边界纪律），所以在产品仓自备一份。
// 用法：VC_TEST("名字") { CHECK(cond); ... } —— 自注册，main 只调 vctest::run_all()。
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace vctest {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}

inline bool& current_ok() {
    static bool ok = true;
    return ok;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { cases().push_back({name, fn}); }
};

/** 跑全部自注册用例，逐例打印 [ OK /FAIL]，返回失败数（0 = 全绿，作进程退出码）。 */
inline int run_all() {
    int failed = 0;
    for (const auto& c : cases()) {
        current_ok() = true;
        c.fn();
        std::printf("[%s] %s\n", current_ok() ? " OK " : "FAIL", c.name);
        if (!current_ok()) ++failed;
    }
    std::printf("共 %zu 例，失败 %d\n", cases().size(), failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace vctest

// ---- 测试共用小件（非断言框架）----------------------------------------------

// 测试根目录：tests/unit/.tmp/<name>（TEMP 在本环境对子进程建目录不可靠，工作区
// 路径确定可用）；每次运行先清空，同域用例互不污染。
inline std::filesystem::path test_root(const char* name) {
    std::error_code ec;
    const auto root = std::filesystem::path(__FILE__).parent_path() / ".tmp" / name;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

// diff 文本的 hunk 头计数：换行后跟 hunk 头的出现次数 + 首行即头
inline std::size_t count_hunks(const std::string& d) {
    const std::string mark = std::string(1, '\n') + "@@";
    std::size_t n = 0;
    for (auto pos = d.find(mark); pos != std::string::npos;
         pos = d.find(mark, pos + 1))
        ++n;
    if (d.compare(0, 2, "@@") == 0) ++n;
    return n;
}

// 名字传**带引号的字符串**（宏按字符串字面量登记用例名）；用例标识符经 __LINE__ 双层拼接生成
// （直接 ##__LINE__ 不会展开行号——经典坑，必须两级宏）。
#define VC_CONCAT_(a, b) a##b
#define VC_CONCAT(a, b) VC_CONCAT_(a, b)
#define VC_TEST(name)                                                                        \
    static void VC_CONCAT(ithaca_test_impl_, __LINE__)();                                        \
    static ::vctest::Registrar VC_CONCAT(ithaca_test_reg_, __LINE__){                            \
        name, &VC_CONCAT(ithaca_test_impl_, __LINE__)};                                          \
    static void VC_CONCAT(ithaca_test_impl_, __LINE__)()

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            ::vctest::current_ok() = false;                                        \
            std::printf("    CHECK 失败: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        }                                                                          \
    } while (0)

#define REQUIRE(cond) CHECK(cond)
