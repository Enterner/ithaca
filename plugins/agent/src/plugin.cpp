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

    sub_input_ = host.on(vc_api::turn_input, [this](const void* p) {
        ctx_.on_input(static_cast<const vc_api::TurnInput*>(p)->text);
    });
    sub_decision_ = host.on(vc_api::turn_decision, [this](const void* p) {
        ctx_.on_decision(*static_cast<const vc_api::TurnDecision*>(p));
    });
    sub_done_ = host.on(vc_api::llm_done, [this](const void* p) {
        ctx_.on_done(*static_cast<const vc_api::LlmDone*>(p));
    });
    sub_error_ = host.on(vc_api::llm_error, [this](const void* p) {
        ctx_.on_llm_error(*static_cast<const vc_api::LlmError*>(p));
    });
    // M5：异步长任务（runner.run）的输出/退出归流——run_id 匹配后才入结果
    sub_run_line_ = host.on(vc_api::run_line, [this](const void* p) {
        const auto* e = static_cast<const vc_api::RunLine*>(p);
        if (AgentCtx::AsyncRun* run = ctx_.find_run(e->run_id);
            run != nullptr && e->text != nullptr)
            run->output += std::string(e->text) + "\n";  // 截断通告也随事件到达
    });
    sub_run_exit_ = host.on(vc_api::run_exit, [this](const void* p) {
        const auto* e = static_cast<const vc_api::RunExit*>(p);
        AgentCtx::AsyncRun* run = ctx_.find_run(e->run_id);
        if (run == nullptr) return;  // 非本 agent 启动的 run：忽略
        run->exited = true;
        run->exit_code = e->exit_code;
        ctx_.try_finish_tools();
    });
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

    // M6/session 持久化：ITHACA_SESSION_FILE ⇒ 恢复历史与用量（load 与切换共用）
    if (const char* v = std::getenv("ITHACA_SESSION_FILE"); v != nullptr && *v != '\0')
        ctx_.load_session_state_(v);

    // 上下文压缩：外置 raw 目录 = 会话文件
    // 旁的 raw/；阈值 env ITHACA_CONTEXT_COMPRESS_TOKENS（默认 32000，0 = 关闭）
    // ——判定信号用 provider 回报的 prompt_tokens（确定性，不由模型自报）
    if (!ctx_.session_path_.empty()) {
        ctx_.raw_dir_ = (fs::path(ctx_.session_path_).parent_path() / "raw").string();
        std::error_code ec;
        fs::create_directories(ctx_.raw_dir_, ec);
    }
    ctx_.compress_threshold_ = 32000;
    if (const char* v = std::getenv("ITHACA_CONTEXT_COMPRESS_TOKENS"))
        ctx_.compress_threshold_ = std::atoll(v);

    host.log(2, "ithaca.agent: 就绪（工具集 " + std::to_string(ctx_.tool_svcs_.size()) +
                    " 个；需批准 " + std::to_string(ctx_.confirm_.size()) + " 项）");
    return NOSTOS_OK;
}

void AgentPlugin::deactivate() noexcept {
    sub_input_ = {};
    sub_decision_ = {};
    sub_done_ = {};
    sub_error_ = {};
    sub_run_line_ = {};
    sub_run_exit_ = {};
    sub_session_ = {};
    sub_mode_ = {};
    ctx_.host_.log(2, "ithaca.agent: 已停用");
}

}  // namespace vc_agent

NOSTOS_PLUGIN(vc_agent::AgentPlugin)
