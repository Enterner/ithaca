#pragma once
// 权限模式三档的纯逻辑：归一与确认清单映射。
// 沙箱（AppContainer）在 runner/proc.hpp；模式事件在 contract/vc_events.hpp；
// jail（shared/vckit/path_jail.hpp）只做解析器 + 在根判定器（是闸不是墙）——
// 根外路径可解析（outside 标记），可达性把关收拢在本文件的确认清单 + dispatch
// 的 outside_root 判定，策略边界不在 jail。
//
//   suggest    —— 逐条问：编辑与命令都走提案→批准（工作区外改动同规，提案卡
//                 展示绝对路径）
//   auto-edit  —— 工作区内编辑免批（applied 携带 diff 事后展示 + undo 兜底）；
//                 命令逐条问；工作区外编辑升级为提案——判源 = apply dry 结果的
//                 outside_root 标记（插件持有 jail，agent 不复制在根判定）
//   full-auto  —— 确认清单为空：无审批、路径不受工作区限制（jail 仅解析）；
//                 AppContainer 沙箱默认关闭（ITHACA_RUN_SANDBOX=1 才启用且仅
//                 授工作区 ACL），把关责任完全在用户对模式的选择
//
// 保留偏置：宁可多问，不可错放。
#include <algorithm>
#include <string>
#include <vector>

namespace vcagent {

inline constexpr const char* kModeSuggest = "suggest";
inline constexpr const char* kModeAutoEdit = "auto-edit";
inline constexpr const char* kModeFullAuto = "full-auto";

// 编辑事务工具的全名（确认清单与 dispatch 的 outside 预检共用——字符串单源，
// 双写必漂移）
inline constexpr const char* kApplyTool = "workspace.apply";

// 归一/校验用契约单源（vc_events.hpp 的 valid/normalize_permission_mode——三档
// 与回落语义全仓只此一份），本头只留常量与确认清单映射。

// 确认清单 = 模式映射（runner.run 的把关在 suggest/auto-edit 走提案卡；
// full-auto 交给用户选择；auto-edit 的工作区外编辑把关在 dispatch 的
// outside_root 预检，不进清单——清单是工具名精确匹配，表达不了路径条件）。
// NOTE: 新增「有路径条件」的把关时同样走 dry 结果标记通道，不要往清单里塞。
inline std::vector<std::string> mode_confirm(const std::string& mode) {
    if (mode == kModeSuggest) return {kApplyTool, "runner.run"};
    if (mode == kModeFullAuto) return {};
    return {"runner.run"};  // auto-edit
}

}  // namespace vcagent
