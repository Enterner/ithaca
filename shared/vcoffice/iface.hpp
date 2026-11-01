#pragma once
// office 域的服务名与跨插件表形状（shared/ 共享树）：消费者 = office 本体与
// webui（/api/fs/read 文档分流 + /api/fs/preview 版式预览）。ToolTable 的形状
// 定义在 contract/vc_tool.hpp（agent 工具面的唯一共享面）；预览表不是模型能力，
// 不进 agent 工具清单，形状归本头（两插件消费方，shared/ 准入即此）。
// 竞争性：别的抽取实现可发布同名工具表顶掉本实现（与 llm/workspace 同款机制）。
#include <nostos/abi/interface.hpp>
#include <nostos/svc_id.hpp>

#include <cstdint>

namespace vcoffice {

inline constexpr std::uint64_t kToolSvc = nostos::svc_id<"ithaca.tool.office">;
inline constexpr const char* kToolSvcName = "ithaca.tool.office";

// ---- 预览转换表（PreviewTable）------------------------------------------------
// C 布局与 tail-append 纪律同 contract 表（TableHeader 首成员、出参调用期有效）。
//
// 线程与生命周期纪律（消费方必须遵守，提供方保证）：
//   · convert 只做校验与入队，立即返回（µs 级）——重活（COM 自动化，秒级）在
//     提供方专用 STA 线程执行，调用线程（webui 宿主排水）不许阻塞。
//   · 结果经 on_done 在提供方转换线程回调，恰好一次（成功/失败/停机取消/超时
//     四路同归此口）；result_json 只在回调期有效，消费方同步消费。
//   · on_done/ud 的存活面由消费方自管：ud 必须是插件无关的堆对象（服务快照 +
//     连接号），deactivate 顺序不定也不悬垂；回调迟到时消费方补发会被内核静默
//     丢弃（Server::respond 契约），无需对账。
//   · 提供方 deactivate：停收队 → 未决 job 以 ok:false 回调 → join 工作线程。
//
// 出参形状（req_json / out_json / result_json 均为 UTF-8 JSON 文本）：
//   req_json     = {"path":"<工作区相对路径>"}
//   out_json     = {"ok":true,"pending":true}（已入队）| {"ok":false,"error":...}
//                  （校验失败/停机/队满——此时 on_done 不会来）
//   result_json  = {"ok":true,"pdf":"<缓存 PDF 绝对路径>"} | {"ok":false,"error":...}
using PreviewDoneFn = void (*)(void* ud, const char* result_json);

struct PreviewTable {
    nostos::abi::TableHeader header{};
    void* self = nullptr;
    nostos_status (*convert)(void* self, const char* req_json, PreviewDoneFn on_done,
                             void* ud, const char** out_json) = nullptr;
};

inline constexpr std::uint64_t kPreviewSvc = nostos::svc_id<"ithaca.preview.office">;
inline constexpr const char* kPreviewSvcName = "ithaca.preview.office";

}  // namespace vcoffice
