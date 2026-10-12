// 事件总线订阅（plugin/webui 的事件切片，事件面与 tui 同构）。
// 各回调在宿主线程上经 drain_posted 串行化执行，只做：拼 JSON → 落盘 → 广播；
// 流式增量只广播不落盘，完整内容由 LlmDone 的 assistant 记录收口。

#include "webui_plugin.hpp"

#include "vc_events.hpp"  // 事件结构/事件 id（tool_register/llm_*/tool_*/edit_*/run_*/notice）

#include "../../workspace/iface.hpp"  // vcws::kToolSvcName：workspace 工具表的稳定契约名

namespace vc_webui {

// ---- 事件订阅（宿主线程序列化；各 handler 只做：拼 JSON → 落盘 → 广播）-----

void WebuiPlugin::subscribe_() {
    // 事件面与 tui 同构；raw 订阅形态（agent/tui 同款）：on(event_id, fn(const void*))
    // workspace 工具表后到接入的兜底：activate 时解析不到（装载顺序在 webui
    // 之后）就等它 tool_register 时再解析——回调在宿主线程上执行，TLS 桥在。
    subs_.push_back(host_.on(vc_api::tool_register, [this](const void* p) {
        const auto* e = static_cast<const vc_api::ToolRegister*>(p);
        if (e->svc_name == nullptr ||
            std::string_view(e->svc_name) != vcws::kToolSvcName || tools_ != nullptr)
            return;
        tools_ = host_.service<vc_api::ToolTable>(vcws::kToolSvc);
        if (tools_ != nullptr) {
            host_.log(2, "ithaca.webui: workspace 工具表已接入（后到）");
            backfill_workspace_();   // 工具表就绪 ⇒ 一次性补发当前会话工作区
        }
    }));
    subs_.push_back(host_.on(vc_api::llm_delta, [this](const void* p) {
        const auto* e = static_cast<const vc_api::LlmDelta*>(p);
        if (e->text == nullptr) return;
        broadcast_({{"type", "assistant_delta"}, {"text", std::string(e->text)}});
    }));
    subs_.push_back(host_.on(vc_api::llm_reasoning, [this](const void* p) {
        const auto* e = static_cast<const vc_api::LlmReasoning*>(p);
        if (e->text == nullptr) return;
        broadcast_({{"type", "assistant_reasoning"}, {"text", std::string(e->text)}});
    }));
    subs_.push_back(host_.on(vc_api::llm_done, [this](const void* p) {
        const auto* e = static_cast<const vc_api::LlmDone*>(p);
        std::string content;
        json usage = json::object();
        bool turn_done = true;  // 无 tool_calls ⇒ 本 turn 收口（前端「本轮修改」卡时机）
        if (e->assistant_json != nullptr) {
            json a = json::parse(e->assistant_json, nullptr, false);
            if (a.is_object() && a.contains("content") && a["content"].is_string())
                content = a["content"].get<std::string>();
            if (a.is_object() && a.contains("tool_calls") && a["tool_calls"].is_array() &&
                !a["tool_calls"].empty())
                turn_done = false;
        }
        if (e->usage != nullptr) {
            json u = json::parse(e->usage, nullptr, false);
            if (u.is_object()) {
                usage = u;
                token_total_ += u.value("total_tokens", 0LL);
            }
        }
        in_flight_.store(false);
        persist_status_();
        record_({{"type", "assistant"},
                 {"text", content},
                 {"usage", usage},
                 {"turn_done", turn_done}});
    }));
    subs_.push_back(host_.on(vc_api::llm_error, [this](const void* p) {
        const auto* e = static_cast<const vc_api::LlmError*>(p);
        in_flight_.store(false);
        persist_status_();
        record_({{"type", "error"},
                 {"code", e->code},
                 {"msg", e->message != nullptr ? std::string(e->message) : ""}});
    }));
    subs_.push_back(host_.on(vc_api::tool_call, [this](const void* p) {
        const auto* e = static_cast<const vc_api::ToolCall*>(p);
        record_({{"type", "tool_call"},
                 {"tool", e->tool != nullptr ? std::string(e->tool) : ""},
                 {"brief", e->args_brief != nullptr ? std::string(e->args_brief) : ""}});
    }));
    subs_.push_back(host_.on(vc_api::tool_result, [this](const void* p) {
        const auto* e = static_cast<const vc_api::ToolResult*>(p);
        record_({{"type", "tool"},
                 {"tool", e->tool != nullptr ? std::string(e->tool) : ""},
                 {"ok", e->ok},
                 {"brief", e->result_brief != nullptr ? std::string(e->result_brief) : ""}});
    }));
    subs_.push_back(host_.on(vc_api::edit_proposed, [this](const void* p) {
        const auto* e = static_cast<const vc_api::EditProposed*>(p);
        pending_turn_ = e->turn_id;
        persist_status_();   // 提案待裁决（聚合徽标同步）
        record_({{"type", "proposal"},
                 {"turn_id", e->turn_id},
                 {"diff", e->diff_text != nullptr ? std::string(e->diff_text) : ""}});
    }));
    subs_.push_back(host_.on(vc_api::edit_applied, [this](const void* p) {
        const auto* e = static_cast<const vc_api::EditApplied*>(p);
        json paths = json::array();
        if (e->paths_json != nullptr) {
            json pa = json::parse(e->paths_json, nullptr, false);
            if (pa.is_array()) paths = pa;
        }
        // edit-diff 方案 v1.1：applied 携带 hunk diff——落盘完成后前端立即渲染；
        // diff_text 为 null = 旧生产者组合（前端退化为路径列表）
        record_({{"type", "applied"},
                 {"turn_id", e->turn_id},
                 {"paths", paths},
                 {"diff", e->diff_text != nullptr ? std::string(e->diff_text) : ""}});
    }));
    subs_.push_back(host_.on(vc_api::edit_undone, [this](const void* p) {
        const auto* e = static_cast<const vc_api::EditUndone*>(p);
        record_({{"type", "undone"}, {"turn_id", e->turn_id}, {"depth", e->snapshot_depth}});
    }));
    subs_.push_back(host_.on(vc_api::run_line, [this](const void* p) {
        const auto* e = static_cast<const vc_api::RunLine*>(p);
        record_({{"type", "run_line"},
                 {"run_id", e->run_id},
                 {"stream", e->stream},
                 {"text", e->text != nullptr ? std::string(e->text) : ""}});
    }));
    subs_.push_back(host_.on(vc_api::run_exit, [this](const void* p) {
        const auto* e = static_cast<const vc_api::RunExit*>(p);
        record_({{"type", "run_exit"}, {"run_id", e->run_id}, {"exit_code", e->exit_code}});
    }));
    subs_.push_back(host_.on(vc_api::notice, [this](const void* p) {
        const auto* e = static_cast<const vc_api::Notice*>(p);
        record_({{"type", "notice"},
                 {"level", e->level},
                 {"text", e->text != nullptr ? std::string(e->text) : ""},
                 {"sys", e->sys}});
    }));
    // 插件 UI 贡献（方案 A）：schema 是"界面长什么样"，值在 config 的 plugins 节。
    // 载荷三串均调用期有效——立即拷贝。map 插入在宿主线程串行化，无锁。
    subs_.push_back(host_.on(vc_api::ui_register, [this](const void* p) {
        const auto* e = static_cast<const vc_api::UiRegister*>(p);
        if (e->plugin == nullptr || e->schema_json == nullptr) return;
        json schema = json::parse(e->schema_json, nullptr, false);
        if (!schema.is_array()) return;
        ui_registry_[e->plugin] = {
            {"label", e->label != nullptr ? std::string(e->label) : std::string(e->plugin)},
            {"schema", std::move(schema)}};
    }));
}

// 工作区补发（tool_register 兜底）：workspace 工具表后于 webui 装载时，启动恢复
// 的 switch_to_ 发出的 WorkspaceSet（defer 到宿主线程）可能先于 workspace 的订阅
// 被丢——事件总线无保留语义（与 vc.ui.register 的 query 握手同款动机）。按当前
// 活跃会话重发一次：同值热切幂等（早到没丢的场景重复 emit 无害）；一次性标志防
// tool_register 多次触发叠发。host_workspace_ 已在 switch_to_ 同步段被矫正为会话
// 工作区，此处直接取用。
void WebuiPlugin::backfill_workspace_() {
    if (ws_backfilled_ || session_.empty() || host_workspace_.empty()) return;
    ws_backfilled_ = true;
    nostos::kit::poster_submit(poster_, [this] {
        const vc_api::WorkspaceSet e{host_workspace_.c_str()};
        host_.emit(vc_api::workspace_set, &e);
    });
}

}  // namespace vc_webui
