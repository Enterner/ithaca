#pragma once
// ithaca 微型断言框架（与内核 nostos_test 的 tframe 同思路的本地版）：
// 产品与插件的测试**不进内核 CI**（内核边界纪律），所以在产品仓自备一份。
// 用法：VC_TEST("名字") { CHECK(cond); ... } —— 自注册，main 只调 vctest::run_all()。
#include <cstdio>
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

// 名字传**带引号的字符串**（内核 tframe 同款）；用例标识符经 __LINE__ 双层拼接生成
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
