// 编辑事务的单测：op 解析点名、dry_run 不落盘、replace 命中数、中途失败全量回滚、
// 同批同文件叠加（overlay 语义）。
#include "ithaca_test_framework.hpp"

#include "edit.hpp"
#include "jail.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using vcws::ApplyResult;
using vcws::EditOp;
using vcws::Jail;
using vcws::MemoryStore;
using vcws::OpError;

namespace {

fs::path make_temp_root() {
    std::error_code ec;
    // 同 test_jail.cpp：测试根放产品仓内（TEMP 对子进程建目录在本环境不可靠）。
    const fs::path root = fs::path(__FILE__).parent_path() / ".tmp" / "ithaca_m1_edit_test";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

// MemoryStore 的键 = jail 解析后的全路径——种子数据必须用同一把键。
std::wstring key_of(Jail& jail, const std::string& rel) {
    std::wstring full;
    std::string norm, err;
    if (!jail.resolve(rel, full, norm, err)) return L"<resolve 失败>";
    return full;
}

// 在指定键上写盘失败的存储（回滚测试用）。
class FailingStore final : public MemoryStore {
public:
    bool write(const std::wstring& key, const std::string& data) override {
        if (key.find(L"locked") != std::wstring::npos) return false;
        return MemoryStore::write(key, data);
    }
};

vcws::ApplyResult run(Jail& jail, vcws::TextStore& store, std::vector<EditOp> ops, bool dry) {
    return vcws::apply_ops(jail, store, ops, dry);
}

}  // namespace

VC_TEST("edit: parse_ops 对未知 op 与缺字段点名到 index") {
    const auto doc = nlohmann::json::parse(
        R"([{"op":"write","path":"a.txt","content":"x"},)"
        R"({"op":"delete","path":"b"},)"
        R"({"path":"c"},)"
        R"({"op":"replace","path":"d.txt","find":"","replace":"y"}])",
        nullptr, /*allow_exceptions=*/false);
    REQUIRE(!doc.is_discarded());

    std::vector<EditOp> ops;
    std::vector<OpError> errors;
    const bool good = vcws::parse_ops(doc, ops, errors);

    CHECK(!good);
    CHECK(ops.size() == 1);        // 只有第一个合法
    REQUIRE(errors.size() == 3);
    CHECK(errors[0].index == 1);   // 未知 op
    CHECK(errors[0].message.find("write|replace") != std::string::npos);
    CHECK(errors[1].index == 2);   // 非对象
    CHECK(errors[2].index == 3);   // 空锚点
}

VC_TEST("edit: dry_run 只校验并生成 diff，不落盘") {
    auto root = make_temp_root();
    Jail jail{root.wstring()};
    MemoryStore store;

    std::vector<EditOp> ops;
    ops.push_back(EditOp{EditOp::Kind::Write, "new.txt", "第一行\n第二行\n", {}, {}});

    const ApplyResult r = run(jail, store, ops, /*dry=*/true);
    CHECK(r.ok);
    CHECK(!r.rolled_back);
    REQUIRE(r.edits.size() == 1);
    CHECK(r.edits[0].is_new);
    CHECK(r.edits[0].diff.find("+ 第一行") != std::string::npos);
    CHECK(!store.exists(L"new.txt"));  // dry_run 的本质承诺：零写盘
}

VC_TEST("edit: replace 的 0 与多命中点名次数，恰好一次才通过") {
    auto root = make_temp_root();
    Jail jail{root.wstring()};
    MemoryStore store;
    store.write(key_of(jail, "dup.txt"), "ab_ab");
    store.write(key_of(jail, "single.txt"), "one two");

    const auto apply = [&](const std::string& path, const std::string& find,
                           const std::string& rep) {
        std::vector<EditOp> ops;
        ops.push_back(EditOp{EditOp::Kind::Replace, path, {}, find, rep});
        return run(jail, store, ops, false);
    };

    ApplyResult r = apply("dup.txt", "ab", "X");
    CHECK(!r.ok);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].message.find("2 次") != std::string::npos);

    r = apply("dup.txt", "zz", "X");
    CHECK(!r.ok);
    CHECK(r.errors[0].message.find("0 次") != std::string::npos);

    r = apply("single.txt", "one", "1");
    CHECK(r.ok);
    std::string content;
    REQUIRE(store.read(key_of(jail, "single.txt"), content));
    CHECK(content == "1 two");
}

VC_TEST("edit: replace_all 多命中全部替换；0 命中仍拒绝") {
    auto root = make_temp_root();
    Jail jail{root.wstring()};
    MemoryStore store;
    store.write(key_of(jail, "dup.txt"), "ab_ab_ab");
    store.write(key_of(jail, "none.txt"), "xyz");

    std::vector<EditOp> ops;
    ops.push_back(EditOp{EditOp::Kind::Replace, "dup.txt", {}, "ab", "X", /*replace_all=*/true});
    const ApplyResult r = run(jail, store, ops, false);
    CHECK(r.ok);
    std::string content;
    REQUIRE(store.read(key_of(jail, "dup.txt"), content));
    CHECK(content == "X_X_X");
    REQUIRE(r.edits.size() == 1);
    CHECK(r.edits[0].diff.find("X_X_X") != std::string::npos);  // 全部命中都进了结果
    CHECK(r.edits[0].diff.find("（+1 −1 行）") != std::string::npos);  // 单行文件：行级统计

    std::vector<EditOp> ops2;
    ops2.push_back(EditOp{EditOp::Kind::Replace, "none.txt", {}, "zz", "Y", /*replace_all=*/true});
    const ApplyResult r2 = run(jail, store, ops2, false);
    CHECK(!r2.ok);
    CHECK(r2.errors[0].message.find("0 次") != std::string::npos);
}

VC_TEST("edit: replace_all 多处散布命中产出多个 hunk") {
    auto root = make_temp_root();
    Jail jail{root.wstring()};
    MemoryStore store;
    std::string seed;
    for (int i = 1; i <= 20; ++i)
        seed += (i == 3 || i == 15 ? "HIT" + std::to_string(i) : "line" + std::to_string(i)) + "\n";
    store.write(key_of(jail, "spread.txt"), seed);

    std::vector<EditOp> ops;
    ops.push_back(
        EditOp{EditOp::Kind::Replace, "spread.txt", {}, "HIT", "CHANGED", /*replace_all=*/true});
    const ApplyResult r = run(jail, store, ops, false);
    CHECK(r.ok);
    std::string content;
    REQUIRE(store.read(key_of(jail, "spread.txt"), content));
    CHECK(content.find("CHANGED3") != std::string::npos &&
          content.find("CHANGED15") != std::string::npos);
    // 两处相距 12 行 > 2×上下文 ⇒ 两个 hunk
    std::size_t hunks = 0;
    const std::string& d = r.edits[0].diff;
    for (std::size_t pos = d.find("\n@@"); pos != std::string::npos;
         pos = d.find("\n@@", pos + 1))
        ++hunks;
    if (d.compare(0, 2, "@@") == 0) ++hunks;
    CHECK(hunks == 2);
}

VC_TEST("edit: 多步事务中途写盘失败 ⇒ 全量回滚到批前原状") {
    auto root = make_temp_root();
    Jail jail{root.wstring()};
    FailingStore store;  // 键含 "locked" 的写盘失败

    std::vector<EditOp> ops;
    ops.push_back(EditOp{EditOp::Kind::Write, "good.txt", "G", {}, {}});
    ops.push_back(EditOp{EditOp::Kind::Write, "locked.txt", "X", {}, {}});
    ops.push_back(EditOp{EditOp::Kind::Write, "after.txt", "A", {}, {}});

    const ApplyResult r = run(jail, store, ops, false);

    CHECK(!r.ok);
    CHECK(r.rolled_back);
    CHECK(!store.exists(key_of(jail, "good.txt")));   // 本批新建 ⇒ 回滚即删除
    CHECK(!store.exists(key_of(jail, "after.txt")));  // 失败步之后根本没写
}

VC_TEST("edit: 覆盖既有文件的批在失败后保留原内容") {
    auto root = make_temp_root();
    Jail jail{root.wstring()};
    FailingStore store;
    store.write(key_of(jail, "keep.txt"), "ORIG");

    std::vector<EditOp> ops;
    ops.push_back(EditOp{EditOp::Kind::Write, "keep.txt", "NEW", {}, {}});
    ops.push_back(EditOp{EditOp::Kind::Write, "locked.txt", "L", {}, {}});

    const ApplyResult r = run(jail, store, ops, false);
    CHECK(!r.ok);
    CHECK(r.rolled_back);
    std::string content;
    REQUIRE(store.read(key_of(jail, "keep.txt"), content));
    CHECK(content == "ORIG");  // 不是空串、不是 NEW——是批前原状
}

VC_TEST("edit: 同批同文件两步编辑叠加（后续 op 看到前序 after）+ 折叠为单条") {
    auto root = make_temp_root();
    Jail jail{root.wstring()};
    MemoryStore store;

    std::vector<EditOp> ops;
    ops.push_back(EditOp{EditOp::Kind::Write, "f.txt", "A\n", {}, {}});
    ops.push_back(EditOp{EditOp::Kind::Replace, "f.txt", {}, "A", "B"});

    const ApplyResult r = run(jail, store, ops, false);
    CHECK(r.ok);
    std::string content;
    REQUIRE(store.read(key_of(jail, "f.txt"), content));
    CHECK(content == "B\n");
    // edit-diff 方案：同文件多 op 折叠为一条——before=批前原状，after=末次结果
    REQUIRE(r.edits.size() == 1);
    CHECK(r.edits[0].is_new);              // 批前不存在
    CHECK(r.edits[0].before.empty());
    CHECK(r.edits[0].after == "B\n");
    CHECK(r.edits[0].ops_count == 2);
    CHECK(r.edits[0].diff.find("+ B") != std::string::npos);
    CHECK(r.edits[0].diff.find("A") == std::string::npos ||
          r.edits[0].diff.find("- A") != std::string::npos);  // 中间态 A 不单独出 diff
}

VC_TEST("edit: 路径写法变体折叠为同一文件（./x 与 x、大小写、反斜杠）") {
    auto root = make_temp_root();
    Jail jail{root.wstring()};
    MemoryStore store;
    store.write(key_of(jail, "dir/f.txt"), "hello world");

    std::vector<EditOp> ops;
    ops.push_back(EditOp{EditOp::Kind::Replace, "dir/f.txt", {}, "hello", "HI"});
    ops.push_back(EditOp{EditOp::Kind::Replace, "./dir/f.txt", {}, "world", "WORLD"});
    ops.push_back(EditOp{EditOp::Kind::Replace, "DIR\\F.TXT", {}, "HI", "HEY"});

    const ApplyResult r = run(jail, store, ops, false);
    CHECK(r.ok);
    std::string content;
    REQUIRE(store.read(key_of(jail, "dir/f.txt"), content));
    // 三个变体叠加：第二步看到第一步结果、第三步看到第二步结果（原文键会静默丢失）
    CHECK(content == "HEY WORLD");
    REQUIRE(r.edits.size() == 1);
    CHECK(r.edits[0].ops_count == 3);
}

VC_TEST("edit: 非 UTF-8 文件整批拒绝（点名），不落盘") {
    auto root = make_temp_root();
    Jail jail{root.wstring()};
    MemoryStore store;
    store.write(key_of(jail, "gbk.txt"), "\xC0\x80 bad utf8");  // overlong：严格校验必拒

    std::vector<EditOp> ops;
    ops.push_back(EditOp{EditOp::Kind::Write, "ok.txt", "fine", {}, {}});
    ops.push_back(EditOp{EditOp::Kind::Replace, "gbk.txt", {}, "bad", "good"});

    const ApplyResult r = run(jail, store, ops, false);
    CHECK(!r.ok);
    CHECK(!r.rolled_back);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].message.find("UTF-8") != std::string::npos);
    CHECK(!store.exists(key_of(jail, "ok.txt")));  // 整批拒绝：零写盘
}

VC_TEST("edit: 新内容含非法 UTF-8 整批拒绝") {
    auto root = make_temp_root();
    Jail jail{root.wstring()};
    MemoryStore store;

    std::vector<EditOp> ops;
    ops.push_back(EditOp{EditOp::Kind::Write, "bin.txt", std::string("x\xFFy\n"), {}, {}});

    const ApplyResult r = run(jail, store, ops, false);
    CHECK(!r.ok);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].message.find("UTF-8") != std::string::npos);
    CHECK(!store.exists(key_of(jail, "bin.txt")));
}
