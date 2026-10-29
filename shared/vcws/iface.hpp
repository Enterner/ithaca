#pragma once
// workspace 域的服务名发布位（shared/ 共享树）：消费者 = workspace 本体与 webui
//（文件树工具表解析）。ToolTable 的形状定义在 contract/vc_tool.hpp（唯一共享
// 面）；这里只有"名字"。
// 竞争性：别的实现可发布同名表顶掉本实现（与 llm 同款机制）。
#include <nostos/svc_id.hpp>

namespace vcws {

inline constexpr std::uint64_t kToolSvc = nostos::svc_id<"ithaca.tool.workspace">;
inline constexpr const char* kToolSvcName = "ithaca.tool.workspace";

}  // namespace vcws
