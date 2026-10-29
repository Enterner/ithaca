#pragma once
// ithaca 契约 —— 上下文管理策略表（vc.ctx.v1）。
//
// 边界原则：信号与策略归插件，时机与历史所有权归编排（agent）。压缩判定 /
// 折叠计划 / 工具结果外置回查 / 超窗错误分类 / 阈值退避等策略知识在策略插件；
// turn 循环、消息历史（messages_）所有权、摘要请求的发送、会话持久化留在 agent。
//
// 同名服务可竞争：ctx_basic /（未来的）其他策略发布同一张表，替换 = 另一个
// DLL 发布同名服务 + 选择开关（env 门 ITHACA_CTX_ACTIVE + vc.ctx.set_active
// 热切，参照 llm 两供应商同构）。消费方（agent）不 manifest inject 本表——
// inject 会让无插件时 LazyGate 永远 PENDING；本表是"可选依赖 + 内置回退"：
// 轮边界 service<> 解析（null = agent 内置同名逻辑），缺插件零行为变化。
//
// 生命周期纪律（NOTE）：agent 不 inject ⇒ 内核不知这条依赖边、revoke_dependents
// 保护不到它；当前安全仅因宿主运行期只 unpublish、不物理卸载 DLL。策略插件
// 运行期只许 unpublish（热切休眠），不得假设自身可被安全卸载；agent 轮边界
// 解析、轮内持有表指针，不跨轮缓存。
//
// 槽位级回退：任一槽为 null 时 agent 用内置同名逻辑（C 表既有纪律）。
// 内聚约束：plan 的折叠行/ref 格式与 ingest_tool_result 的产出互认——
// 这一组默认整体绑定（"压缩套件"，拆开混搭会产生互不认识的格式）；
// mission 槽可独立替换。
//
// 数据面全 JSON、无 C++ 类型泄漏；字符串只在调用期有效（出参缓冲由实现方
// 持有至下次同槽调用，与 ToolTable 同纪律）。
// 布局纪律：C 布局、字段只在尾部追加、null 槽探测回退。
#include <nostos/abi/interface.hpp>
#include <nostos/svc_id.hpp>

#include <cstdint>

namespace vc_api {

struct CtxTable {
    nostos::abi::TableHeader header{};
    void* self = nullptr;

    // 策略自述：{"name":"basic","capabilities":["spool","recall","llm_summary",…]}。
    // name 供 vc.ctx.set_active 热切匹配；capabilities 供 agent 开合内置工具
    //（如无 recall 能力则不暴露 context.recall）。
    nostos_status (*info)(void* self, const char** out_json);

    // 任务卡建档/折叠重建。调用点 = 首轮建档与压缩折叠切割前（切割后原位替换）；
    // 轮边界不调用——卡跨轮逐字节冻结是缓存前缀纪律（卡在 system 之后，逐轮
    // 重写会让其后整段历史前缀失效）。入：{"messages":[…]}；出：要垫/改写的
    // mission 消息（{"role":"system","content":…}），null 出参 = 不动。NOTE:
    // agent 靠内容头部标记识别既有 mission 做原位替换——产出应沿用内置标记
    // 形态，否则重复插入。
    nostos_status (*mission)(void* self, const char* messages_json, const char** out_json);

    // 工具结果收编：外置 + 最小充分记录一步完成（落盘策略插件自理）。
    //   入：{"tool":"office.read","ok":true,"body":<完整结果 JSON 文本>,
    //        "turn":N,"seq":N}
    //   出：内联记录 JSON 文本（进 role=tool 消息），可带 "ref"（外置锚点，
    //        消费方 plan 产出与 recall 输入互认该格式）。
    nostos_status (*ingest_tool_result)(void* self, const char* args_json,
                                        const char** out_json);

    // context.recall 执行体：{"ref":"…","offset":N,"limit":N} → 带 provenance
    // 标记与分页元数据（total/has_more/next_offset）的回查结果。
    nostos_status (*recall)(void* self, const char* args_json, const char** out_json);

    // 压缩规划（同步、必须快——发送前预检不能等 LLM）。机械路径即时返回；
    // LLM 精修由 agent 侧摘要环异步完成（见 summary_prompt/apply_summary）。
    //   入：{"messages":[…], "signals":{"last_prompt_tokens":N,"usage_real":bool,
    //        "usage":{"prompt_tokens":N,…},"window":N,"max_output":N,
    //        "estimate_prompt_tokens":N（仅 precheck：请求体粗估——真实 usage 尚
    //        未回报时的门槛信号）}, "reason":"precheck|turn_end|overflow|manual",
    //        "subagent":bool}
    //   出：{"action":"none"}
    //     | {"action":"fold","plan_id":N,"digest_message":{…},"cut_begin":N,
    //        "cut_end":N,"refs":["…"],
    //        "segments_folded":N,"anchors_total":N,"anchors_dropped":N（可选，
    //        通告/保真观测用，缺省 0）}
    // digest_message 为一条 role=system 消息（沿用内置标记约定：任务卡/摘要的
    // 原位替换与旧摘要并入靠 agent 的标记识别，产出应保留既有标记形态）；
    // cut 区间 = 待删除的 messages 下标 [cut_begin, cut_end)。判定门槛（阈值/
    // 硬保护/退避）全部在策略内——agent 不再持有阈值状态，只透传原始信号。
    // verify 退避等策略状态由实现方自理（不持久化、重启重置——取舍：代价小，
    // 避免策略状态污染会话文件）。
    nostos_status (*plan)(void* self, const char* state_json, const char** out_json);

    // provider 错误文案分类：入 = 错误文本；出 {"overflow":bool}
    //（超窗自救判定属策略知识——各家文案不一，误判/漏判代价不对称）。
    nostos_status (*classify_error)(void* self, const char* error_text,
                                    const char** out_json);

    // 尾部追加（摘要数据面——摘要请求的发送权留 agent：request_id 出自 agent
    // 的发号器，插件自设第二发号器必然数值重叠，回包会被 agent 收进历史污染
    // 上下文；agent 保持"全进程唯一 chat 发送方"不变量）。阶段二摘要的提示词
    // 与截断规则；null = 不做 LLM 精修、机械 digest 直接定稿。
    //   入：{"digest":{…机械 digest 消息…}}；出：{"system":<摘要 system 指令>,
    //        "instructions":<折叠给摘要器的原文>}
    nostos_status (*summary_prompt)(void* self, const char* digest_json,
                                    const char** out_json);

    // 尾部追加：摘要回包后处理（裁切/加 provenance/重建外置索引）；null = 原样用。
    //   入：{"digest":{…机械 digest…},"summary":<模型回包文本>,"refs":[…]}；
    //   出：定稿的摘要消息（{"role":"system","content":…}）。
    nostos_status (*apply_summary)(void* self, const char* args_json,
                                   const char** out_json);
    //（尾部追加空间：observe / estimate / 句柄式增量面…）
};

// 服务 id：vc.ctx.v1
inline constexpr std::uint64_t ctx = nostos::svc_id<"vc.ctx.v1">;

}  // namespace vc_api
