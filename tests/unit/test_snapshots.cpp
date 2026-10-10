// 快照栈的单测：turn 分组与合并、LIFO 与上限、陈旧检测两段式（check 只读 / write 落地）。
#include "ithaca_test_framework.hpp"

#include "snapshots.hpp"

#include <string>

using vcws::FileSnap;
using vcws::MemoryStore;
using vcws::SnapshotStack;

VC_TEST("snapshots: 同 turn 同文件合并——before 取首次 after 取最新") {
    SnapshotStack s;
    s.add(1, {FileSnap{"f.txt", L"f", "", "V1", true}});
    s.add(1, {FileSnap{"f.txt", L"f", "V1", "V2", false}});  // 同 turn 第二次触碰

    const vcws::TurnGroup* g = s.peek_latest();
    REQUIRE(g != nullptr);
    CHECK(g->turn_id == 1);
    REQUIRE(g->files.size() == 1);  // 合并成一条
    CHECK(g->files[0].before.empty());
    CHECK(g->files[0].is_new);      // 保留首次的 is_new
    CHECK(g->files[0].after == "V2");
}

VC_TEST("snapshots: 不同 turn 各自成组，undo 默认弹最新") {
    SnapshotStack s;
    s.add(1, {FileSnap{"a.txt", L"a", "", "A1", true}});
    s.add(2, {FileSnap{"b.txt", L"b", "", "B1", true}});
    CHECK(s.depth() == 2);

    const vcws::TurnGroup* top = s.peek_latest();
    REQUIRE(top != nullptr);
    CHECK(top->turn_id == 2);

    CHECK(s.pop_latest());
    CHECK(s.depth() == 1);
    CHECK(s.find(2) == nullptr);
    CHECK(s.find(1) != nullptr);
    CHECK(s.pop_turn(1));
    CHECK(s.empty());
}

VC_TEST("snapshots: 栈上限 20——超限丢最旧") {
    SnapshotStack s;
    for (int turn = 1; turn <= 25; ++turn)
        s.add(turn, {FileSnap{"t" + std::to_string(turn) + ".txt", L"k", "", "C", false}});
    CHECK(s.depth() == SnapshotStack::kMaxTurns);
    CHECK(s.find(1) == nullptr);    // 最旧的 5 个被丢弃
    CHECK(s.find(5) == nullptr);
    CHECK(s.find(6) != nullptr);
    CHECK(s.peek_latest()->turn_id == 25);
}

VC_TEST("snapshots: 陈旧检测——外部修改与删除都拒绝，匹配才放行") {
    MemoryStore store;
    FileSnap snap{"f.txt", L"f", "", "V2", false};
    store.write(L"f", "V2");

    std::string err;
    CHECK(vcws::undo_check(store, snap, err));  // 当前 == after：健康

    store.write(L"f", "被手改了");
    CHECK(!vcws::undo_check(store, snap, err));
    CHECK(err.find("被外部修改") != std::string::npos);

    store.remove(L"f");
    CHECK(!vcws::undo_check(store, snap, err));
    CHECK(err.find("被外部删除") != std::string::npos);
}

VC_TEST("snapshots: 恢复写回——既有文件还原旧内容，本批新写即删除") {
    MemoryStore store;
    std::string err;

    const FileSnap existing{"g.txt", L"g", "旧内容", "新内容", false};
    store.write(L"g", "新内容");
    CHECK(vcws::undo_check(store, existing, err));
    CHECK(vcws::undo_write(store, existing, err));
    std::string content;
    REQUIRE(store.read(L"g", content));
    CHECK(content == "旧内容");

    const FileSnap created{"n.txt", L"n", "", "内容", true};
    store.write(L"n", "内容");
    CHECK(vcws::undo_check(store, created, err));
    CHECK(vcws::undo_write(store, created, err));
    CHECK(!store.exists(L"n"));  // 本批新建的文件，撤销 = 删除
}
