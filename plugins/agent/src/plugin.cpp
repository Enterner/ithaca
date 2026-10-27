// plugin/agent —— turn 状态机 + 泛型工具桥 + 批准流 + 异步长任务归流。
//
// 本翻译单元：AgentPlugin 薄壳（事件订阅分发到 ctx_，M2 对象化后 v1 单槽）、
// manifest（inject vc.llm.v1）+ NOSTOS_PLUGIN 入口。死锁纪律/职责边界/流图
// 与各切片职责地图见 include/agent_plugin.hpp。

#include "agent_plugin.hpp"

#include <cstdlib>  // std::getenv：工具清单/权限模式/会话文件

namespace vc_agent {

// manifest（POD 静态表；plugin.hpp 的 manifest_slot 按指针成员约定取用）
const char* const k_agent_inject[] = {"vc.llm.v1", nullptr};
const nostos_manifest k_agent_manifest = {sizeof(nostos_manifest), k_agent_inject, nullptr,
                                          nullptr};
const nostos_manifest* AgentPlugin::manifest = &k_agent_manifest;

nostos_status AgentPlugin::activate(const nostos::abi::Host& host) {
    ctx_.host_ = host;
    // poster：dispatch 推迟一拍的投递通道（见 AgentCtx::dispatch_after_done_）。
    // 领取失败不阻激活——回退同步分发，只是事件顺序回到旧行为。
    if (host.acquire_poster(&ctx_.poster_) != NOSTOS_OK)
        host.log(3, "ithaca.agent: 领取 poster 失败——工具分发回退同步（事件顺序退回旧行为）");

    sub_input_ = host.on(vc_api::turn_input, [this](const void* p) {
        ctx_.on_input(static_cast<const vc_api::TurnInput*>(p)->text);
    });
    sub_decision_ = host.on(vc_api::turn_decision, [this](const void* p) {
        ctx_.on_decision(*static_cast<const vc_api::TurnDecision*>(p));
    });
    sub_done_ = host.on(vc_api::llm_done, [this](const void* p) {
        // 主会话槽 = 广播（slot=0）；子会话的 LLM 事件按槽单播、各自订阅直达
        // 所属 ctx——不再总线扇出 + request_id 反查
        ctx_.on_done(*static_cast<const vc_api::LlmDone*>(p));
    });
    sub_error_ = host.on(vc_api::llm_error, [this](const void* p) {
        ctx_.on_llm_error(*static_cast<const vc_api::LlmError*>(p));
    });
    // run.line/exit 迁移到槽订阅（subscribe_turn_slot_）：runner 按 slot=turn_id
    // 单播，归属 ctx 直达；主会话槽的行/退出由 agent 重播广播供 UI 消费
    // 会话切换（webui）——仅 Idle 时接受；加载 state 并重定向落盘
    sub_session_ = host.on(vc_api::session_load, [this](const void* p) {
        const auto* e = static_cast<const vc_api::SessionLoad*>(p);
        if (e->path == nullptr) return;
        if (ctx_.state_ != AgentCtx::State::Idle) {
            ctx_.host_.log(3, "ithaca.agent: 会话切换被拒（turn 进行中）");
            return;
        }
        ctx_.load_session_state_(e->path);
    });

    if (const char* v = std::getenv("ITHACA_AGENT_TOOLS"))
        ctx_.tool_svcs_ = vcagent::split_csv(v);
    if (ctx_.tool_svcs_.empty())
        ctx_.tool_svcs_ = {"ithaca.tool.workspace", "ithaca.tool.mcp",
                           "ithaca.tool.runner"};  // runner 入默认清单；mcp 未配置时空表零影响
    // 权限模式：确认清单由模式映射（见 rebuild_confirm_）；
    // ITHACA_PERMISSION_MODE 设默认，vc.permission.mode 事件热切。
    // （旧 ITHACA_AGENT_CONFIRM_TOOLS 静态确认清单机制随白名单一并移除）
    ctx_.mode_ = vc_api::normalize_permission_mode(
        std::getenv("ITHACA_PERMISSION_MODE") != nullptr
            ? std::getenv("ITHACA_PERMISSION_MODE") : "");
    ctx_.rebuild_confirm_();
    sub_mode_ = host.on(vc_api::permission_mode, [this](const void* p) {
        const auto* e = static_cast<const vc_api::PermissionMode*>(p);
        const std::string next = vc_api::normalize_permission_mode(
            e->mode != nullptr ? e->mode : "");
        // 启动对齐（webui activate 回放 config 值）：值未变化 ⇒ 静默——通告会
        // 写进 transcript，宿主每次启动恒 +1 条 ⇒ e2e 重放计数恒定偏移（踩过）。
        // 变化也不发通告：模式在 UI 选择器上即时可见，写 transcript 属刷屏噪音
        // （踩过：反复切换时通告连发刷屏）。
        if (next == ctx_.mode_) return;
        ctx_.mode_ = next;
        ctx_.rebuild_confirm_();
    });

    // 上下文压缩阶段二：动态阈值 0.9×W——env ITHACA_CONTEXT_WINDOW 显式覆盖
    // （测试/便携）> set_active 事件携带的窗口；ITHACA_CONTEXT_SUMMARY=off 关闭
    // LLM 摘要（永远机械折叠，阶段一行为）。
    if (const char* v = std::getenv("ITHACA_CONTEXT_WINDOW"); v != nullptr && *v != '\0') {
        ctx_.ctx_window_env_ = true;
        ctx_.set_context_window_(std::atoll(v));
    }
    if (const char* v = std::getenv("ITHACA_CONTEXT_SUMMARY"))
        ctx_.summary_enabled_ = std::string_view(v) != "off";
    sub_llm_set_ = host.on(vc_api::llm_set_active, [this](const void* p) {
        const auto* e = static_cast<const vc_api::LlmSetActive*>(p);
        if (!ctx_.ctx_window_env_) ctx_.set_context_window_(e->context_window);
    });

    // M6/session 持久化：ITHACA_SESSION_FILE ⇒ 恢复历史与用量（load 与切换共用）
    if (const char* v = std::getenv("ITHACA_SESSION_FILE"); v != nullptr && *v != '\0')
        ctx_.load_session_state_(v);

    // 上下文压缩阈值 env ITHACA_CONTEXT_COMPRESS_TOKENS（默认 32000 = 成员初值，
    // 0 = 关闭）——判定信号用 provider 回报的 prompt_tokens（确定性，不由模型自报）。
    // raw 外置目录随 load_session_state_ 走（activate 的 env 装载与会话切换同源）。
    if (const char* v = std::getenv("ITHACA_CONTEXT_COMPRESS_TOKENS"))
        ctx_.compress_threshold_ = std::atoll(v);

    // 子代理类型目录：env ITHACA_AGENT_DIR 显式覆盖（测试/便携）> 启动工作区
    // 推导（ITHACA_WORKSPACE_DIR，两个启动器均已传递）> 工作区热切事件跟随
    // （<工作区>/.ithaca/agent）。目录缺失 = 仅内置 general
    if (const char* v = std::getenv("ITHACA_AGENT_DIR"); v != nullptr && *v != '\0')
        ctx_.agent_dir_ = v;
    else if (const char* ws = std::getenv("ITHACA_WORKSPACE_DIR"); ws != nullptr && *ws != '\0')
        ctx_.agent_dir_ = (fs::path(ws) / ".ithaca" / "agent").string();
    sub_workspace_ = host.on(vc_api::workspace_set, [this](const void* p) {
        const auto* e = static_cast<const vc_api::WorkspaceSet*>(p);
        if (e->root == nullptr || *e->root == '\0') return;
        if (const char* v = std::getenv("ITHACA_AGENT_DIR"); v != nullptr && *v != '\0')
            return;  // env 显式覆盖最高，事件不覆写（对齐 ITHACA_CONTEXT_WINDOW 的优先级语义）
        ctx_.agent_dir_ = (fs::path(e->root) / ".ithaca" / "agent").string();
    });

    host.log(2, "ithaca.agent: 就绪（工具集 " + std::to_string(ctx_.tool_svcs_.size()) +
                    " 个；需批准 " + std::to_string(ctx_.confirm_.size()) + " 项）");
    return NOSTOS_OK;
}

void AgentPlugin::deactivate() noexcept {
    sub_input_ = {};
    sub_decision_ = {};
    sub_done_ = {};
    sub_error_ = {};
    sub_session_ = {};
    sub_mode_ = {};
    sub_llm_set_ = {};
    sub_workspace_ = {};
    ctx_.host_.log(2, "ithaca.agent: 已停用");
}

}  // namespace vc_agent

NOSTOS_PLUGIN(vc_agent::AgentPlugin)
