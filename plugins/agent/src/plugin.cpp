// plugin/agent —— turn 状态机 + 泛型工具桥 + 批准流 + 异步长任务归流。
//
// 本翻译单元：AgentPlugin 薄壳（事件按会话标路由到多会话槽、槽的创建与 env 初始
// 化）+ manifest（inject vc.llm.v1）+ NOSTOS_PLUGIN 入口。死锁纪律/职责边界/流图
// 与各切片职责地图见 include/agent_plugin.hpp。
//
// 多会话并行：每个已加载会话一个 AgentCtx（ensure_ctx_ 按需创建），turn 输入/
// 裁决/命令按事件尾部 session 标路由（null/空回落最近加载槽——selftest 等单驱动
// 场景零改动）；槽间共享同一 provider（请求体 slot=turn_no 单播事件回槽）与同一
// 工作区池（跨工作区并行被 webui 门卫拒绝，本插件不校验）。

#include "agent_plugin.hpp"

#include <cstdlib>  // std::getenv：工具清单/权限模式/会话文件
#include <fstream>  // std::ifstream：系统提示词文件通道
#include <iterator>  // std::istreambuf_iterator：提示词整读

namespace vc_agent {

namespace {

// state 文件路径 → 会话 id 的派生单点在 agent_plugin.hpp（session_id_of_state），
// 本翻译单元与 load_session_state_ 共用

}  // namespace

// manifest（POD 静态表；plugin.hpp 的 manifest_slot 按指针成员约定取用）
const char* const k_agent_inject[] = {"vc.llm.v1", nullptr};
const nostos_manifest k_agent_manifest = {sizeof(nostos_manifest), k_agent_inject, nullptr,
                                          nullptr};
const nostos_manifest* AgentPlugin::manifest = &k_agent_manifest;

// 会话槽创建（幂等）：env 派生的初始态在此灌入。env 每槽读取一遍——同进程 env
// 恒定，值一致；目录/提示词文件的重复读取只在槽创建时发生（每会话一次）。
AgentCtx& AgentPlugin::ensure_ctx_(const std::string& id) {
    auto& slot = ctxs_[id];
    if (slot != nullptr) return *slot;
    slot = std::make_unique<AgentCtx>();
    AgentCtx& c = *slot;
    c.host_ = host_;
    c.poster_ = poster_;
    c.session_id_ = id;
    if (const char* v = std::getenv("ITHACA_AGENT_TOOLS"))
        c.tool_svcs_ = vcagent::split_csv(v);
    if (c.tool_svcs_.empty())
        // runner 入默认清单；office/mcp 未装载时 discover_tools 跳过并通告
        //（可选能力不阻激活）
        c.tool_svcs_ = {"ithaca.tool.workspace", "ithaca.tool.office",
                        "ithaca.tool.mcp", "ithaca.tool.runner"};
    // 权限模式：确认清单由模式映射（见 rebuild_confirm_）——env 设默认，事件
    // 活值优先（boot 回放先于首槽的时序兜底）
    c.mode_ = vc_api::normalize_permission_mode(
        !mode_live_.empty() ? mode_live_.c_str()
                            : (std::getenv("ITHACA_PERMISSION_MODE") != nullptr
                                   ? std::getenv("ITHACA_PERMISSION_MODE") : ""));
    c.rebuild_confirm_();
    // 上下文压缩：动态阈值 0.9×W——env ITHACA_CONTEXT_WINDOW 显式覆盖
    //（测试/便携）> set_active 事件携带的窗口；ITHACA_CONTEXT_SUMMARY=off 关闭
    // LLM 摘要（恒机械折叠）。
    if (const char* v = std::getenv("ITHACA_CONTEXT_WINDOW"); v != nullptr && *v != '\0') {
        c.ctx_window_env_ = true;
        c.set_context_window_(std::atoll(v));
    } else if (window_live_ > 0) {
        c.set_context_window_(window_live_);   // set_active 事件先于首槽的时序兜底
    }
    if (const char* v = std::getenv("ITHACA_CONTEXT_SUMMARY"))
        c.summary_enabled_ = std::string_view(v) != "off";
    // 上下文压缩阈值 env ITHACA_CONTEXT_COMPRESS_TOKENS（默认 32000 = 成员初值，
    // 0 = 关闭）——判定信号用 provider 回报的 prompt_tokens（确定性，不由模型自报）。
    // raw 外置目录随 load_session_state_ 走（activate 的 env 装载与会话切换同源）。
    if (const char* v = std::getenv("ITHACA_CONTEXT_COMPRESS_TOKENS"))
        c.compress_threshold_ = std::atoll(v);
    // 子代理类型目录：env ITHACA_AGENT_DIR 显式覆盖（测试/便携）> 启动工作区
    // 推导（ITHACA_WORKSPACE_DIR，两个启动器均已传递）> 工作区热切事件跟随
    // （<工作区>/.ithaca/agent）。目录缺失 = 仅内置 general
    if (const char* v = std::getenv("ITHACA_AGENT_DIR"); v != nullptr && *v != '\0')
        c.agent_dir_ = v;
    else if (!ws_live_.empty())
        c.agent_dir_ = (std::filesystem::path(ws_live_) / ".ithaca" / "agent").string();
    else if (const char* ws = std::getenv("ITHACA_WORKSPACE_DIR"); ws != nullptr && *ws != '\0')
        c.agent_dir_ = (std::filesystem::path(ws) / ".ithaca" / "agent").string();
    if (!ws_live_.empty())
        c.ws_root_ = ws_live_;   // 工作区事件活值 > env 起源（事件是最新的事实）
    else if (const char* ws = std::getenv("ITHACA_WORKSPACE_DIR"); ws != nullptr && *ws != '\0')
        c.ws_root_ = ws;
    // 产品身份：profile 注入的系统提示词（内联优先，文件次之；都缺 = 内置
    // kSystemPrompt）。只影响新会话首轮注入的 system 消息——已落盘会话的历史
    // system 不追改（历史是事实），换身份 = 开新会话；子代理仍用各自 AgentType
    // 的 prompt（type 优先，产品身份不覆盖）。
    if (const char* v = std::getenv("ITHACA_SYSTEM_PROMPT"); v != nullptr && *v != '\0') {
        c.system_prompt_ = v;
    } else if (const char* f = std::getenv("ITHACA_SYSTEM_PROMPT_FILE");
               f != nullptr && *f != '\0') {
        std::ifstream in(std::filesystem::path(f), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        if (!text.empty())
            c.system_prompt_ = std::move(text);
        else
            host_.log(3, std::string("ithaca.agent: 系统提示词文件读取失败或为空"
                                    "（回退内置）：") + f);
    }
    host_.log(2, "ithaca.agent: 会话槽 " + id + " 就绪（工具集 " +
                     std::to_string(c.tool_svcs_.size()) + " 个；需批准 " +
                     std::to_string(c.confirm_.size()) + " 项）");
    return c;
}

// 事件 session 标 → 槽。null/空回落 active_（最近加载——单驱动场景的语义）；
// 带未知会话 = 调用方未先 session_load（webui 契约保证先加载），忽略并留痕。
AgentCtx* AgentPlugin::route_(const char* session) {
    if (session != nullptr && *session != '\0') {
        const auto it = ctxs_.find(session);
        if (it == ctxs_.end()) {
            host_.log(3, "ithaca.agent: 事件目标会话未加载: " + std::string(session));
            return nullptr;
        }
        return it->second.get();
    }
    return active_;
}

nostos_status AgentPlugin::activate(const nostos::abi::Host& host) {
    host_ = host;
    // poster：dispatch 推迟一拍的投递通道（见 AgentCtx::dispatch_after_done_），
    // 全槽共享（poster 无状态）。领取失败不阻激活——回退同步分发（工具事件与
    // llm_done 派发同帧，失去推迟一拍的序保证）。
    if (host.acquire_poster(&poster_) != NOSTOS_OK)
        host.log(3, "ithaca.agent: 领取 poster 失败——工具分发回退同步（事件顺序退回旧行为）");

    // 输入/裁决/命令：按事件尾部 session 标路由到归属槽
    sub_input_ = host.on(vc_api::turn_input, [this](const void* p) {
        const auto* in = static_cast<const vc_api::TurnInput*>(p);
        if (AgentCtx* ctx = route_(in->session); ctx != nullptr)
            ctx->on_input(in->text != nullptr ? in->text : "",
                          in->attachments_json != nullptr ? in->attachments_json : "");
    });
    sub_decision_ = host.on(vc_api::turn_decision, [this](const void* p) {
        const auto* d = static_cast<const vc_api::TurnDecision*>(p);
        if (AgentCtx* ctx = route_(d->session); ctx != nullptr) ctx->on_decision(*d);
    });
    sub_cmd_ = host.on(vc_api::session_cmd, [this](const void* p) {
        const auto* c = static_cast<const vc_api::SessionCmd*>(p);
        if (AgentCtx* ctx = route_(c->session); ctx != nullptr) ctx->on_session_cmd(*c);
    });

    // 会话加载 = 槽的创建/装载（多会话并行的基础；同会话重复加载 = 幂等重载）。
    // 加载与该槽是否在跑 turn 无关——每槽状态机独立。
    sub_session_ = host.on(vc_api::session_load, [this](const void* p) {
        const auto* e = static_cast<const vc_api::SessionLoad*>(p);
        if (e->path == nullptr) return;
        const std::string id = session_id_of_state(e->path);
        AgentCtx& ctx = ensure_ctx_(id);
        active_ = &ctx;
        ctx.load_session_state_(e->path);
    });

    // 全局事件扇出到所有槽：权限模式（各槽重算确认清单）、模型窗口（各槽动态
    // 阈值）、工作区热切（各槽类型目录/摘要根跟随）。值未变化的静默纪律见各槽
    // 内注释（通告刷屏噪音，都踩过）。
    sub_mode_ = host.on(vc_api::permission_mode, [this](const void* p) {
        const auto* e = static_cast<const vc_api::PermissionMode*>(p);
        mode_live_ = vc_api::normalize_permission_mode(
            e->mode != nullptr ? e->mode : "");   // 活值先行（槽可能还未创建）
        for (auto& [id, ctx] : ctxs_) {
            if (mode_live_ == ctx->mode_) continue;
            ctx->mode_ = mode_live_;
            ctx->rebuild_confirm_();
        }
    });
    sub_llm_set_ = host.on(vc_api::llm_set_active, [this](const void* p) {
        const auto* e = static_cast<const vc_api::LlmSetActive*>(p);
        if (e->context_window > 0) window_live_ = e->context_window;
        for (auto& [id, ctx] : ctxs_)
            if (!ctx->ctx_window_env_ && e->context_window > 0)
                ctx->set_context_window_(e->context_window);
    });
    sub_workspace_ = host.on(vc_api::workspace_set, [this](const void* p) {
        const auto* e = static_cast<const vc_api::WorkspaceSet*>(p);
        if (e->root == nullptr || *e->root == '\0') return;
        ws_live_ = e->root;   // 活值先行（槽可能还未创建）
        // env 显式覆盖最高，事件不覆写 agent_dir_（对齐窗口 env 的优先级语义）；
        // ws_root_ 无 env 覆盖面，恒跟随
        const char* dir_env = std::getenv("ITHACA_AGENT_DIR");
        const bool dir_locked = dir_env != nullptr && *dir_env != '\0';
        for (auto& [id, ctx] : ctxs_) {
            if (!dir_locked)
                ctx->agent_dir_ =
                    (std::filesystem::path(e->root) / ".ithaca" / "agent").string();
            ctx->ws_root_ = e->root;   // 摘要的路径呈现跟随真实工作区
        }
    });

    // 会话持久化：ITHACA_SESSION_FILE ⇒ 启动恢复 = 首个槽（load 与切换
    // 共用路径；activate 期无 webui 竞争）
    if (const char* v = std::getenv("ITHACA_SESSION_FILE"); v != nullptr && *v != '\0') {
        const std::string id = session_id_of_state(v);
        AgentCtx& ctx = ensure_ctx_(id);
        active_ = &ctx;
        ctx.load_session_state_(v);
    }

    host.log(2, "ithaca.agent: 就绪（多会话槽模式；已恢复 " +
                    std::to_string(ctxs_.size()) + " 个会话）");
    return NOSTOS_OK;
}

void AgentPlugin::deactivate() noexcept {
    sub_input_ = {};
    sub_decision_ = {};
    sub_cmd_ = {};
    sub_session_ = {};
    sub_mode_ = {};
    sub_llm_set_ = {};
    sub_workspace_ = {};
    ctxs_.clear();       // 槽订阅随 RAII 释放（slot_subs_ 在 ctx 内）
    active_ = nullptr;
    host_.log(2, "ithaca.agent: 已停用");
}

}  // namespace vc_agent

NOSTOS_PLUGIN(vc_agent::AgentPlugin)
