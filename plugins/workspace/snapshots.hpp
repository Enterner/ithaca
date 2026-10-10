#pragma once
// turn 分组快照栈：撤销单位 = turn（一个 turn 可能多次 apply，全部并入同一组——
// 方案 §4.4/§5.2）。陈旧检测在恢复侧：当前内容必须 == after（turn 结束态），
// 不一致 ⇒ 整组拒绝——不覆盖用户手改。恢复用两段式（先全组只读裁决，再统一写回），
// 保证"任一冲突 ⇒ 整组零改动"。
#include "fs.hpp"
#include "text.hpp"  // wc_lower：合并键用大小写折叠的 full（路径写法变体同一文件）

#include <cstddef>
#include <string>
#include <vector>

namespace vcws {

struct FileSnap {
    std::string path;    // 折叠相对路径（展示用）
    std::wstring full;   // Store 的键
    std::string before;  // 批前内容（本批新写 ⇒ 空串 + is_new）
    std::string after;   // turn 结束态（陈旧检测的基准）
    bool is_new = false;
};

struct TurnGroup {
    int turn_id = 0;
    std::vector<FileSnap> files;  // 按首次触碰顺序
};

class SnapshotStack {
public:
    static constexpr std::size_t kMaxTurns = 20;  // 超限丢最旧（撤销深度上限）

    // 并入一次 apply 的编辑（该批已成功落盘）。同 turn 同文件多次触碰：
    // before/is_new 取首次，after 取最新。
    void add(int turn_id, std::vector<FileSnap> edits) {
        const auto find_group = [this](int id) -> TurnGroup* {
            for (auto& g : stack_)
                if (g.turn_id == id) return &g;
            return nullptr;
        };
        if (find_group(turn_id) == nullptr) {
            stack_.push_back({turn_id, {}});
            while (stack_.size() > kMaxTurns) stack_.erase(stack_.begin());
        }
        TurnGroup* group = find_group(turn_id);
        for (auto& e : edits) {
            bool merged = false;
            for (auto& f : group->files)
                if (wc_lower(f.full) == wc_lower(e.full)) {  // 大小写不敏感：Windows 语义
                    f.after = e.after;  // before / is_new 保留首次
                    merged = true;
                    break;
                }
            if (!merged) group->files.push_back(std::move(e));
        }
    }

    bool empty() const noexcept { return stack_.empty(); }
    std::size_t depth() const noexcept { return stack_.size(); }

    const TurnGroup* peek_latest() const { return stack_.empty() ? nullptr : &stack_.back(); }
    const TurnGroup* find(int turn_id) const {
        for (const auto& g : stack_)
            if (g.turn_id == turn_id) return &g;
        return nullptr;
    }

    // 恢复成功后由调用方弹出（先 peek/find 裁决，再写回，最后弹出——整组原子）。
    bool pop_latest() {
        if (stack_.empty()) return false;
        stack_.pop_back();
        return true;
    }
    bool pop_turn(int turn_id) {
        for (auto it = stack_.begin(); it != stack_.end(); ++it)
            if (it->turn_id == turn_id) {
                stack_.erase(it);
                return true;
            }
        return false;
    }

private:
    std::vector<TurnGroup> stack_;  // back = 最新
};

// ---- 恢复的两段式（裁决只读；写回才动文件）------------------------------------

inline bool undo_check(TextStore& store, const FileSnap& snap, std::string& err) {
    std::string current;
    if (!store.read(snap.full, current)) {
        err = "文件已不存在（被外部删除）: " + snap.path;
        return false;
    }
    if (current != snap.after) {
        err = "内容与 turn 结束态不符（被外部修改）: " + snap.path;
        return false;
    }
    return true;
}

inline bool undo_write(TextStore& store, const FileSnap& snap, std::string& err) {
    if (snap.is_new) {
        if (!store.remove(snap.full)) {
            err = "删除本批新建文件失败: " + snap.path;
            return false;
        }
        return true;
    }
    if (!store.write(snap.full, snap.before)) {
        err = "写回快照内容失败: " + snap.path;
        return false;
    }
    return true;
}

}  // namespace vcws
