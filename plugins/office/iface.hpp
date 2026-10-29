#pragma once
// plugin/office 自己的契约位：本插件发布的服务名。
// ToolTable 的形状定义在 contract/vc_tool.hpp（唯一共享面）；这里只有"名字"。
// 竞争性：别的抽取实现可发布同名表顶掉本实现（与 llm/workspace 同款机制）。
// 跨插件 include 只许本文件（拿服务名），不许伸进实现头。
#include <nostos/svc_id.hpp>

namespace vcoffice {

inline constexpr std::uint64_t kToolSvc = nostos::svc_id<"ithaca.tool.office">;
inline constexpr const char* kToolSvcName = "ithaca.tool.office";

}  // namespace vcoffice
