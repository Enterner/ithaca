#pragma once
// plugin/workspace 自己的契约位：本插件发布的服务名。
// ToolTable 的形状定义在 contract/vc_tool.hpp（唯一共享面）；这里只有"名字"。
// 竞争性：别的实现可发布同名表顶掉本实现（方案 §5.2 的 llm 同款机制）。
#include <nostos/svc_id.hpp>

namespace vcws {

inline constexpr std::uint64_t kToolSvc = nostos::svc_id<"ithaca.tool.workspace">;
inline constexpr const char* kToolSvcName = "ithaca.tool.workspace";

}  // namespace vcws
