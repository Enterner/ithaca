// ithaca_unit_tests 入口：main 只调 vctest::run_all()，用例经 VC_TEST 自注册统一运行。
#include "ithaca_test_framework.hpp"

#include <cstdio>

int main() {
    // 无缓冲：用例崩溃（如栈溢出）时已完成的输出不丢失，崩在哪一例立即可见
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return ::vctest::run_all();
}
