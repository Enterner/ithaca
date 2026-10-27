#pragma once
// ithaca 契约 —— LLM provider 服务表（vc.llm.v1）。
//
// 同名服务可竞争：llm_cloud / llm_mock 发布同一张表，
// agent 与宿主只认契约不认实现（hello ↔ hello_v2 的"换插件不换宿主"）。
// 布局纪律：C 布局、字段只在尾部追加、字符串只在调用期有效。
#include <nostos/abi/interface.hpp>
#include <nostos/svc_id.hpp>

#include <cstdint>

namespace vc_api {

struct LlmTable {
    nostos::abi::TableHeader header{};
    void* self = nullptr;

    // 非阻塞提交一轮对话。
    //   messages_json：[{"role":"user","content":"..."}]（OpenAI 兼容子集，调用期有效）
    //   增量以 vc.llm.delta、结束以 vc.llm.done / vc.llm.error 事件回（都在宿主线程）。
    //   NOTE: messages 可含多条 role=system（任务目标卡/压缩摘要垫在历史中间）——
    //   provider 必须归一为各家 API 的实际形态（拼接/映射），不得假设单 system。
    // 返回 NOSTOS_OK 表示已受理；忙/参数错等用 NOSTOS_ERR_* 点名。
    nostos_status (*chat)(void* self, int request_id, const char* messages_json);

    // 取消在途请求；已完成的请求返回 NOSTOS_ERR_BAD_STATE。
    nostos_status (*cancel)(void* self, int request_id);

    // 能力自述（调用期有效）：{"provider":"mock","model":"mock-1"} 必填；可选
    // 元数据 context_window / max_output_tokens（模型窗口/输出上限——已知模型
    // 查表，未知不报），消费方（agent 压缩阈值 / 输出预留）以非零为有效。
    nostos_status (*info)(void* self, const char** out_json);

    // 尾部追加：带工具定义的对话提交（function calling）。body_json =
    //   {"messages":[...], "tools":[{"type":"function","function":{...}}]}
    // 语义与 chat 一致；chat 等价于"无 tools 的 chat_ex"。旧 provider 未实现时
    // 该槽为 nullptr——调用方以"槽位是否为 null"探测（本契约随产品仓演进，
    // 宿主与插件同步重编；无外部冻结消费者）。
    nostos_status (*chat_ex)(void* self, int request_id, const char* body_json);

    // 尾部追加：热切模型（Trae 式右下角选择器）。model 为 API 模型 id（调用期
    // 有效，provider 必须拷贝）。对"下一个"请求生效，不影响在途请求。
    // 旧 provider 未实现时该槽为 nullptr——调用方以 null 探测并回退"重启生效"。
    nostos_status (*set_model)(void* self, const char* model);
};

inline constexpr std::uint64_t llm = nostos::svc_id<"vc.llm.v1">;

// vc.llm.set_active 事件（LlmSetActive）在 vc_events.hpp——事件集中一份头，
// 此处只留服务表；本头单独包含的消费者经 vc_events.hpp 拿事件定义。

// ---- 产品默认值（跨插件共用出处；尾部追加）----------------------------------

// 默认上下文窗口（tokens）：webui 建档/未知模型兜底、llm_cloud 窗口查表、
// llm_mock 合成自述共用一份——字面量多处分写必漂移（踩过：前端 PRESETS 与
// 后端默认目录曾对同一模型写出两个不同窗口值，配置显示与 agent 实际拿到的
// 窗口不一致）。
inline constexpr long long kDefaultContextWindow = 128000;

// 内置云端供应商默认 Base URL（llm_cloud env 兜底 / webui 迁移建档共用）。
inline constexpr const char* kDefaultCloudBaseUrl = "https://api.deepseek.com";

}  // namespace vc_api
