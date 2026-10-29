#pragma once
// office 域的服务名发布位（shared/ 共享树）：消费者 = office 本体与 webui
//（/api/fs/read 文档分流）。ToolTable 的形状定义在 contract/vc_tool.hpp（唯一
// 共享面）；这里只有"名字"。
// 竞争性：别的抽取实现可发布同名表顶掉本实现（与 llm/workspace 同款机制）。
#include <nostos/svc_id.hpp>

namespace vcoffice {

inline constexpr std::uint64_t kToolSvc = nostos::svc_id<"ithaca.tool.office">;
inline constexpr const char* kToolSvcName = "ithaca.tool.office";

}  // namespace vcoffice
