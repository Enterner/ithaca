#pragma once
// 编辑事务：LLM 只产出两种 op——write（整文件重写）与 replace（精确
// search/replace 块，不产出行号补丁）。事务语义与内核"激活即事务"同构：
//   · 校验阶段全部通过才动手——任一失败（路径越界 / replace 0 或多命中 / 目标缺失）
//     ⇒ 整批拒绝，零写盘；错误点名到 op 序号与路径，回灌给模型自我纠正；
//   · 写盘阶段任一步失败 ⇒ 按 LIFO 用批前原状恢复已写文件（本批新写的删除），
//     rolled_back = true。
// dry_run ⇒ 只校验 + 生成 diff，不落盘——错误在批准门之前暴露。
// 回滚原语：内核 Scope（经 plugin_scope.hpp 门面），defer +
// committed 标志实现"成功路径零副作用、失败路径 LIFO 全量还原"。
#include "diff.hpp"
#include "fs.hpp"
#include "jail.hpp"

#include <nostos/plugin_scope.hpp>

#include <nostos/nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace vcws {

struct EditOp {
    enum class Kind { Write, Replace };
    Kind kind = Kind::Write;
    std::string path;
    std::string content;   // write
    std::string find;      // replace
    std::string replace;   // replace（新文本）
    bool replace_all = false;  // replace：命中多次时全部替换（缺省仍要求恰好 1 次）
};

struct OpError {
    int index = -1;
    std::string path;
    std::string message;
};

struct FileEdit {
    std::string path;      // 折叠后的相对路径（'/' 分隔；同文件多 op 取首次写法）
    std::wstring full;     // jail 解析出的全路径（Store 的键）
    std::string before;    // 批前内容（新文件为空串，is_new = true）
    std::string after;     // 批内末次编辑的结果（同文件多 op 折叠后 = 最终态）
    std::string diff;      // before → after 的 hunk diff
    std::size_t add = 0;   // diff 统计：新增行数（与统计尾行同源，随事件结构化下发）
    std::size_t rem = 0;   // diff 统计：删除行数
    bool is_new = false;
    int ops_count = 1;     // 折叠进本项的 op 数（≥2 时展示标注「N 处编辑」）
};

struct ApplyResult {
    bool ok = false;
    bool rolled_back = false;
    std::vector<FileEdit> edits;  // 成功/dry_run：全部；写盘失败：含失败项在内的本批计划
    std::vector<OpError> errors;
};

// ops JSON → 结构化。未知 op / 缺字段 / 空锚点都点名到 index。
inline bool parse_ops(const nlohmann::json& doc, std::vector<EditOp>& out,
                      std::vector<OpError>& errors) {
    if (!doc.is_array()) {
        errors.push_back({-1, {}, "\"ops\" 必须是数组"});
        return false;
    }
    bool good = true;
    for (std::size_t i = 0; i < doc.size(); ++i) {
        const auto& item = doc[i];
        if (!item.is_object()) {
            errors.push_back({static_cast<int>(i), {}, "op 必须是对象"});
            good = false;
            continue;
        }
        EditOp op;
        // op 键名兼容：真实 LLM 常写 "type":"write"（合理命名）而非 "op"——都接受
        // （踩过：DeepSeek 首选 type 键 ⇒ 未知 op ⇒ 写盘失败整批回滚）
        const std::string kind = item.contains("op") && item["op"].is_string()
                                     ? item["op"].get<std::string>()
                                     : (item.contains("type") && item["type"].is_string()
                                            ? item["type"].get<std::string>()
                                            : "");
        op.path = item.value("path", "");
        if (kind == "write") {
            op.kind = EditOp::Kind::Write;
            op.content = item.value("content", "");
        } else if (kind == "replace") {
            op.kind = EditOp::Kind::Replace;
            op.find = item.value("find", "");
            op.replace = item.value("replace", "");
            if (item.contains("replace_all") && item["replace_all"].is_boolean())
                op.replace_all = item["replace_all"].get<bool>();
        } else {
            errors.push_back(
                {static_cast<int>(i), op.path, "未知 op: \"" + kind + "\"（write|replace）"});
            good = false;
            continue;
        }
        if (op.path.empty()) {
            errors.push_back({static_cast<int>(i), op.path, "path 为空"});
            good = false;
            continue;
        }
        if (op.kind == EditOp::Kind::Replace && op.find.empty()) {
            errors.push_back({static_cast<int>(i), op.path, "find 为空（拒绝空锚点）"});
            good = false;
            continue;
        }
        out.push_back(std::move(op));
    }
    return good;
}

inline std::size_t count_occurrences(const std::string& text, const std::string& needle) {
    std::size_t hits = 0;
    for (std::size_t pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + needle.size()))
        ++hits;
    return hits;
}

inline std::string replace_once(std::string text, const std::string& needle,
                                const std::string& replacement) {
    const std::size_t pos = text.find(needle);
    if (pos != std::string::npos) text.replace(pos, needle.size(), replacement);
    return text;
}

// 全部替换：扫描拼接（不做原位迭代——replacement 含 needle 时原位循环会死转）
inline std::string replace_every(const std::string& text, const std::string& needle,
                                 const std::string& replacement) {
    std::string out;
    out.reserve(text.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = text.find(needle, pos);
        if (hit == std::string::npos) {
            out += text.substr(pos);
            return out;
        }
        out += text.substr(pos, hit - pos);
        out += replacement;
        pos = hit + needle.size();
    }
}

// 单 op 校验并产出 after。current = nullptr ⇒ 文件不存在（write 视作新建）。
// diff 不在这里算——apply_ops 统一按「折叠后的批前原状 → 末次结果」生成。
inline bool validate_op(const EditOp& op, const std::string* current, FileEdit& out,
                        std::string& err) {
    out.path = op.path;
    if (op.kind == EditOp::Kind::Write) {
        out.is_new = current == nullptr;
        out.before = current != nullptr ? *current : std::string{};
        out.after = op.content;
    } else {
        if (current == nullptr) {
            err = "replace 的目标文件不存在: " + op.path;
            return false;
        }
        const std::size_t hits = count_occurrences(*current, op.find);
        if (hits == 0) {
            err = "find 命中 0 次: " + op.path;
            return false;
        }
        if (hits > 1 && !op.replace_all) {
            err = "find 命中 " + std::to_string(hits) +
                  " 次（必须恰好 1 次；要全部替换请加 replace_all=true）: " + op.path;
            return false;
        }
        out.is_new = false;
        out.before = *current;
        out.after = op.replace_all ? replace_every(*current, op.find, op.replace)
                                   : replace_once(*current, op.find, op.replace);
    }
    return true;
}

// 事务执行。同批对同文件的后续 op 看到前序 op 的 after（overlay）——LLM 分步编辑
// 同一文件的常规形状。文件标识键 = wc_lower(edit.full)（jail 解析产物的小写折叠），
// **不用模型原文**——路径写法变体（"./x"、大小写、反斜杠）指向同一真实文件，
// 按原文做键会让编辑静默丢失。同文件多 op 折叠为一条
// planned（before=批前原状，after=末次结果，ops_count 计数），diff 随之只出一张。
inline ApplyResult apply_ops(const Jail& jail, TextStore& store, const std::vector<EditOp>& ops,
                             bool dry_run) {
    ApplyResult r;
    std::vector<FileEdit> planned;                              // 每文件一项
    std::vector<std::pair<std::wstring, std::size_t>> slot;     // 折叠键 → planned 下标

    const auto find_slot = [&slot](const std::wstring& key) -> std::size_t {
        for (std::size_t k = 0; k < slot.size(); ++k)
            if (slot[k].first == key) return k;
        return static_cast<std::size_t>(-1);
    };

    // ---- 校验阶段（零写盘；任一失败 ⇒ 整批拒绝）--------------------------------
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const EditOp& op = ops[i];
        FileEdit edit;
        std::string err;
        edit.path = op.path;
        if (!jail.resolve(op.path, edit.full, edit.path, err)) {
            r.errors.push_back({static_cast<int>(i), op.path, "路径被拒: " + err});
            break;
        }
        const std::wstring key = wc_lower(edit.full);
        const std::size_t hit = find_slot(key);
        std::string current;
        bool exists;
        if (hit != static_cast<std::size_t>(-1)) {
            exists = true;  // 批内前序 op 已写过它：overlay 取其 after
            current = planned[hit].after;
        } else {
            exists = store.exists(edit.full);
            if (exists) {
                if (!store.read(edit.full, current)) {
                    r.errors.push_back({static_cast<int>(i), op.path, "文件存在但不可读"});
                    break;
                }
                // 非 UTF-8 文件进 before/diff 后 json dump 必抛——点名拒绝，
                // 由模型改走其它路径（错误回灌），绝不带着坏编码落盘
                if (!utf8_valid(current)) {
                    r.errors.push_back({static_cast<int>(i), op.path,
                                        "文件现有内容不是有效的 UTF-8 文本（拒绝编辑以避免损坏）"});
                    break;
                }
            }
        }
        if (!validate_op(op, exists ? &current : nullptr, edit, err)) {
            r.errors.push_back({static_cast<int>(i), op.path, err});
            break;
        }
        if (!utf8_valid(edit.after)) {
            r.errors.push_back({static_cast<int>(i), op.path, "新内容不是有效的 UTF-8 文本"});
            break;
        }
        if (hit != static_cast<std::size_t>(-1)) {
            FileEdit& e = planned[hit];
            e.after = std::move(edit.after);
            e.diff = make_diff(e.before, e.after, &e.add, &e.rem);  // 折叠 diff：批前原状 → 末次结果
            ++e.ops_count;
        } else {
            edit.diff = make_diff(edit.before, edit.after, &edit.add, &edit.rem);
            slot.emplace_back(std::move(key), planned.size());
            planned.push_back(std::move(edit));
        }
    }

    // 校验结论：ok 必须由证据显式赋值 = errors.empty()（踩过：早退守卫把"没有
    // 错误"误判成失败——默认值 false 直接当结论用，写盘从未发生）。
    r.ok = r.errors.empty();
    if (!r.ok || dry_run) {
        r.edits = std::move(planned);
        return r;
    }

    // ---- 写盘阶段（任一失败 ⇒ LIFO 回滚到批前原状）------------------------------
    // 回滚原语：内核 Scope（plugin_scope.hpp 门面，F9 落地）。每个首次触碰的文件
    // 登记一个 defer（成功后 committed=true 使其变空操作）；失败 ⇒ undo.reset()
    // 逆序全量还原。
    nostos::Scope undo;
    bool committed = false;
    std::vector<std::wstring> touched;  // 首次触碰去重（还原依据 = 批前原状）
    for (std::size_t k = 0; k < planned.size(); ++k) {
        const FileEdit& edit = planned[k];
        if (std::find(touched.begin(), touched.end(), edit.full) == touched.end()) {
            touched.push_back(edit.full);
            std::wstring full = edit.full;
            const bool existed = store.exists(full);
            std::string original;
            if (existed) store.read(full, original);
            undo.defer([store = &store, full = std::move(full), existed,
                        original = std::move(original), &committed] {
                if (committed) return;  // 成功路径：defer 变空操作
                if (existed) store->write(full, original);
                else store->remove(full);
            });
        }
        if (!store.write(edit.full, edit.after)) {
            undo.reset();          // ← 内核原语：LIFO 全量还原（先于 committed 封口）
            committed = true;
            r.ok = false;
            r.rolled_back = true;
            r.errors.push_back({static_cast<int>(k), edit.path,
                                "写盘失败（" + store.last_error() + "）（本批已全部回滚）"});
            r.edits = std::move(planned);
            return r;
        }
    }
    committed = true;  // defer 全部转空操作；scope 析构零副作用
    r.ok = true;
    r.edits = std::move(planned);
    return r;
}

}  // namespace vcws
