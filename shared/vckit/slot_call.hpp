#pragma once
// vckit —— 表槽 JSON 调用助手（产品级共享件，经 shared/ 裸名共用）：
// 「dump → 调槽 → parse → 拷出参」的单一实现。
//
// ABI 纪律：槽出参的 `const char*` 只保证**调用期有效**——历史上消费方各说各话
// （agent 立即拷贝、webui 持 out_buf_ 存活缓冲）。本助手在槽返回后**立即**
// 消费出参（拷成 string / parse 成 json），消费方不再需要任何存活缓冲，
// 出参生命周期口径收口到这一处。
//
// 对 json 类型无感：仓里两份 vendored json 并存（产品 shared/nlohmann 全局域、
// 内核 nostos/nlohmann 包装域），本头不挑——入参与回包 body 的 json 类型随
// 调用方（模板参数/实参推导，duck `.dump()`/`::parse`/`::object()`），消费方
// 各用各的 vendored 副本，互不越界。
//
// 回包两态：ok=true ⇒ body 可用（json 形态恒为 object；str 形态为原文）；
// ok=false ⇒ 槽返回非 NOSTOS_OK、出参为空或（json 形态）不可 parse 为 object，
// 状态码原样带回（st）——错误文案是调用方策略，本助手不代拟。

#include <string>
#include <utility>

#include "nostos/abi/nostos_abi.h"

namespace vckit {

// 原文形态的回包（槽出参为裸字符串的槽：ingest/recall 等；无 json 依赖）。
struct SlotText {
    bool ok = false;
    nostos_status st = NOSTOS_ERR_FAILED;
    std::string body;
};

// JSON 形态的回包（槽出参须为 JSON object——契约里绝大多数槽如此）。
template <typename Json>
struct SlotReplyT {
    bool ok = false;
    nostos_status st = NOSTOS_ERR_FAILED;
    Json body = Json::object();
};

// exec 适配器：调用方把「表槽调用」写成 lambda——槽指针/槽名/额外实参各表
// 不同，本助手不抽象表形状，只收编出参消费纪律。
//     vckit::slot_call_json(state, [&](const char* s, const char** out) {
//         return table->plan(table->self, s, out);
//     });

namespace detail {

// 调槽 + 立即拷贝出参（调用期有效纪律的唯一实现点）：状态码原样透传，
// 出参缺失/为空按 ok=false（st 保留真实值）。
template <typename Exec>
SlotText slot_run(const char* in, Exec&& exec) {
    SlotText r;
    const char* out = nullptr;
    r.st = exec(in, &out);
    if (r.st != NOSTOS_OK || out == nullptr) return r;
    r.ok = true;
    r.body = out;
    return r;
}

}  // namespace detail

// JSON 形态：入参为 json 值（duck .dump()；body 类型随入参推导）。
template <typename Json, typename Exec>
SlotReplyT<Json> slot_call_json(const Json& in, Exec&& exec) {
    const SlotText t = detail::slot_run(in.dump().c_str(), std::forward<Exec>(exec));
    SlotReplyT<Json> r;
    r.st = t.st;
    if (!t.ok) return r;
    Json parsed = Json::parse(t.body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return r;
    r.ok = true;
    r.body = std::move(parsed);
    return r;
}

// JSON 形态：入参已是序列化 JSON 文本（透传，避免二次 dump）；body 类型由
// 调用方显式指定（webui 的 nostos 包装域 / agent 的全域副本各取所需）。
template <typename Json, typename Exec>
SlotReplyT<Json> slot_call_json(const std::string& in, Exec&& exec) {
    const SlotText t = detail::slot_run(in.c_str(), std::forward<Exec>(exec));
    SlotReplyT<Json> r;
    r.st = t.st;
    if (!t.ok) return r;
    Json parsed = Json::parse(t.body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return r;
    r.ok = true;
    r.body = std::move(parsed);
    return r;
}

// 原文形态：入参为 json 值（duck .dump()）。
template <typename Json, typename Exec>
SlotText slot_call_str(const Json& in, Exec&& exec) {
    return detail::slot_run(in.dump().c_str(), std::forward<Exec>(exec));
}

}  // namespace vckit
