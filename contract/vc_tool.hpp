#pragma once
// ithaca 契约 —— 统一工具表（方案 v1.1 §4.1）。
//
// 任何插件都能成为"工具提供者"：svc 名自选（vc.tool.workspace / vc.tool.runner / …），
// 表形状相同 ⇒ agent 泛型桥接，新增工具插件零 agent 改动。
// 发现走 config 显式清单（agent.tools），本表不约定名字、只约定形状。
// 布局纪律：C 布局、字段只在尾部追加、字符串只在调用期有效。
#include <nostos/abi/interface.hpp>

#include <cstdint>

namespace vc_api {

struct ToolTable {
    nostos::abi::TableHeader header{};
    void* self = nullptr;

    // 工具清单 JSON（调用期有效）：
    //   [{"name":"apply","description":"...","schema":{...}}]
    // agent 据此拼 LLM 的 tools 数组 —— 工具的 schema 是插件的自述，agent 不解释内容。
    nostos_status (*list)(void* self, const char** out_json);

    // 执行一个工具。
    //   只读/短工具：同步返回结果 JSON（调用期有效）。
    //   长任务（runner.run）：立即返回 {"run_id":N}，输出走 vc.run.line / vc.run.exit 事件。
    //   编辑类（workspace.apply）：args 带 "dry_run":true 时只校验 + 生成 diff、不落盘。
    // turn_id 由 agent 传入：workspace 据此把同一 turn 的多次 apply 聚合进同一快照组。
    // 返回值约定：NOSTOS_OK 只代表"调用成功"——工具级失败（文件缺失、命中不为 1、
    // 路径被拒、外部改动冲突…）返回 NOSTOS_OK + {"ok":false,"error":...}，
    // 这是给模型的正常回灌通路；NOSTOS_ERR_* 只留给基础设施级失败（内部异常等）。
    nostos_status (*execute)(void* self, const char* name, const char* args_json, int turn_id,
                             const char** out_json);
};

}  // namespace vc_api
