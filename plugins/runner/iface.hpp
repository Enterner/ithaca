#pragma once
// plugin/runner 自己的契约位：本插件发布的服务名。
// ToolTable 形状定义在 contract/vc_tool.hpp；工具名 "run"（异步：返回 run_id，
// 输出走 vc.run.line / vc.run.exit 事件）。
#include <nostos/svc_id.hpp>

namespace vcrun {

inline constexpr std::uint64_t kToolSvc = nostos::svc_id<"ithaca.tool.runner">;
inline constexpr const char* kToolSvcName = "ithaca.tool.runner";

}  // namespace vcrun
