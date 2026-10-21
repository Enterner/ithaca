#pragma once
// 权限模式三档的纯逻辑：归一与确认清单映射。
// 沙箱（AppContainer）在 runner/proc.hpp；模式事件在 contract/vc_events.hpp。
//
//   suggest    —— 逐条问：编辑与命令都走提案→批准（对应旧 confirm 全开）
//   auto-edit  —— 编辑免批（applied 携带 diff 事后展示 + undo 兜底），命令逐条问
//   full-auto  —— 确认清单为空：runner 在 AppContainer 沙箱内执行
//                  （文件系统限工作区 + 断网；把关责任从人移到沙箱）
//
// 保留偏置：宁可多问，不可错放。
#include <algorithm>
#include <string>
#include <vector>

namespace vcagent {

inline constexpr const char* kModeSuggest = "suggest";
inline constexpr const char* kModeAutoEdit = "auto-edit";
inline constexpr const char* kModeFullAuto = "full-auto";

inline bool valid_mode(const std::string& m) {
    return m == kModeSuggest || m == kModeAutoEdit || m == kModeFullAuto;
}
inline std::string normalize_mode(const std::string& m) {
    return valid_mode(m) ? m : std::string(kModeAutoEdit);
}

// 确认清单 = 模式映射（runner.run 的把关在 suggest/auto-edit 走提案卡；
// full-auto 交给沙箱）
inline std::vector<std::string> mode_confirm(const std::string& mode) {
    if (mode == kModeSuggest) return {"workspace.apply", "runner.run"};
    if (mode == kModeFullAuto) return {};
    return {"runner.run"};  // auto-edit
}

}  // namespace vcagent
