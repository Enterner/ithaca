// 管道化子进程的单测（真实子进程：cmd /c 形态，均在秒内完成）。
#include "ithaca_test_framework.hpp"

#include "proc.hpp"

#include <string>
#include <vector>

using namespace vcrun;

VC_TEST("proc: echo 走 stdout 管道，退出码 0") {
    ChildPipes child;
    std::string err;
    REQUIRE(spawn(L"cmd /c echo hello", L"", child, err));

    LineSplitter splitter;
    std::vector<std::string> lines;
    std::string chunk;
    while (read_chunk(child.stdout_read, chunk))
        splitter.feed(chunk.data(), chunk.size(),
                      [&](const std::string& l) { lines.push_back(l); });
    splitter.flush([&](const std::string& l) { lines.push_back(l); });

    REQUIRE(!lines.empty());
    CHECK(lines[0] == "hello");

    WaitForSingleObject(child.process, 5000);
    DWORD code = 0xFFFFFFFFu;
    GetExitCodeProcess(child.process, &code);
    CHECK(code == 0);
}

VC_TEST("proc: 退出码透传（exit 1 ⇒ 1）") {
    ChildPipes child;
    std::string err;
    REQUIRE(spawn(L"cmd /c exit 1", L"", child, err));
    WaitForSingleObject(child.process, 5000);
    DWORD code = 0u;
    GetExitCodeProcess(child.process, &code);
    CHECK(code == 1);
}

VC_TEST("proc: stderr 独立管道路由") {
    ChildPipes child;
    std::string err;
    REQUIRE(spawn(L"cmd /c echo E 1>&2", L"", child, err));
    WaitForSingleObject(child.process, 5000);  // 先等退出，再读两端（输出 < 4KB 管道缓冲）

    std::string so, se, chunk;
    while (read_chunk(child.stdout_read, chunk)) so += chunk;
    while (read_chunk(child.stderr_read, chunk)) se += chunk;

    CHECK(so.empty());                          // stdout 没有内容 —— 路由正确
    CHECK(se.find('E') != std::string::npos);   // stderr 收到（cmd 语义可能带尾随空格）
}

VC_TEST("proc: 长任务在超时后被终止（kill 语义）") {
    ChildPipes child;
    std::string err;
    REQUIRE(spawn(L"cmd /c ping -n 5 127.0.0.1 >nul", L"", child, err));
    const DWORD w = WaitForSingleObject(child.process, 300);
    CHECK(w == WAIT_TIMEOUT);                       // 仍在跑
    TerminateProcess(child.process, 0xFFFFFFFFu);   // runner 的超时终止语义
    WaitForSingleObject(child.process, 5000);
    DWORD code = 0u;
    GetExitCodeProcess(child.process, &code);
    CHECK(code == 0xFFFFFFFFu);                     // 终止码透传
}

