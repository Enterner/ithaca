#pragma once
// ithaca 契约 —— 事件（宿主与插件都编译这一份；唯一共享面的一部分）。
//
// 规矩与内核 app/contract.hpp 相同：C 布局、名字的 fnv1a-64、
// **指针/字符串只在本次回调期间有效，要留存必须拷贝**（ABI 纪律第五条）。
// 事件载荷经 raw 通道跨界：发方 emit(id, const void*)，收方按本头布局读取。
// 字段只在尾部追加。
//
// slot 路由约定（nostos on_slot/emit_slot，内核 58dc137 起）：ithaca 的 slot =
// 会话槽 turn_no（agent 插件全局发号器，跨主/子会话不撞）。多实例私有事件
// （子代理的 LLM 事件族、run.line/exit）按槽单播；主会话对外可见的事件留在
// 广播通道（slot=0）——子代理不可见性由"订阅才可达"保证，不再靠载荷标志。
#include <nostos/event.hpp>

#include <cstdint>
#include <string_view>

namespace vc_api {

// 用户在终端输入了一行（tui 输入线程 → poster → 宿主线程 emit）。
struct TurnInput {
    static constexpr std::string_view name = "vc.turn.input";
    const char* text = nullptr;  // UTF-8，调用期有效
};

// 用户对提案的裁决（tui → agent）。
// decision：0=approved（本提案通过） 1=denied 2=all（本会话后续免批） 3=undo（撤销上一 turn）
//           4=cancel_turn（取消当前 turn；turn_id 被忽略——M4 Ctrl+C / 取消语义）
// paths_json（尾部追加，M-W6 逐文件批准）：JSON 数组字符串，列出**接受**的文件路径；
//           null/空 = 全部文件（向后兼容：旧 tui 不设此字段）。
// msg（尾部追加，M-W10 拒绝反馈）：decision=1 时随拒绝带给 agent 的用户留言
//           （"拒绝并反馈"——ZCode 式：输入框文字即反馈）；null = 无留言。
struct TurnDecision {
    static constexpr std::string_view name = "vc.turn.decision";
    int turn_id = 0;
    int decision = 0;
    const char* paths_json = nullptr;  // 调用期有效，要留存必须拷贝
    const char* msg = nullptr;         // 调用期有效，要留存必须拷贝
};

// 工具插件后到接入的提示。MVP 的发现走 config 显式清单，
// 这只是"有新工具上线"的通知，载荷是 svc 名。
struct ToolRegister {
    static constexpr std::string_view name = "vc.tool.register";
    const char* svc_name = nullptr;
};

// LLM 流式增量。
struct LlmDelta {
    static constexpr std::string_view name = "vc.llm.delta";
    int request_id = 0;
    const char* text = nullptr;  // UTF-8，调用期有效
    // 已废弃（slot 路由替代，布局保留）：子代理请求的事件现按 slot=turn_no 单播，
    // 不进广播通道——本字段恒为 0，生产者不再回填，消费方不得读取。
    int subagent = 0;
};

// 深度思考增量（推理模型如 deepseek-reasoner 的 reasoning_content 流）。
// 在正文增量之前到达；非推理模型不产生本事件。text 调用期有效。
struct LlmReasoning {
    static constexpr std::string_view name = "vc.llm.reasoning";
    int request_id = 0;
    const char* text = nullptr;  // UTF-8，调用期有效
    // 已废弃（slot 路由替代，布局保留）：语义同 LlmDelta.subagent。
    int subagent = 0;
};

// 一轮对话正常结束。assistant_json = 完整的 assistant 消息（含可能的 tool_calls）：
//   {"role":"assistant","content":"...","tool_calls":[{"id":"..","type":"function",
//     "function":{"name":"workspace.read","arguments":"{...}"}}]}
// provider 负责把流式片段拼装成完整消息（content 与 tool_calls 的 arguments 碎片）。
// usage = 本轮 token 用量（JSON 文本，provider 尽力提供，可为 null）：
    //   {"prompt_tokens":N,"completion_tokens":N,"total_tokens":N
    //    [,"cache_tokens":N]}——缓存命中为可选归一口径（DeepSeek
    //    prompt_cache_hit_tokens / OpenAI prompt_tokens_details.cached_tokens）
// 均为调用期有效，要留存必须拷贝。
struct LlmDone {
    static constexpr std::string_view name = "vc.llm.done";
    int request_id = 0;
    const char* assistant_json = nullptr;
    const char* usage = nullptr;
    // 已废弃（slot 路由替代，布局保留）：语义同 LlmDelta.subagent。
    int subagent = 0;
};

// 一轮对话失败。code 沿用 nostos_status 值域；message 点名原因（调用期有效）。
struct LlmError {
    static constexpr std::string_view name = "vc.llm.error";
    int request_id = 0;
    int code = 0;
    const char* message = nullptr;
    // 已废弃（slot 路由替代，布局保留）：语义同 LlmDelta.subagent。
    int subagent = 0;
};

// 工具调用/结果的展示事件（agent → tui）。
struct ToolCall {
    static constexpr std::string_view name = "vc.tool.call";
    int turn_id = 0;
    const char* tool = nullptr;        // "workspace.apply" 形态（<工具表名>.<工具名>）
    const char* args_brief = nullptr;  // 参数摘要，调用期有效
};
struct ToolResult {
    static constexpr std::string_view name = "vc.tool.result";
    int turn_id = 0;
    const char* tool = nullptr;
    int ok = 0;
    const char* result_brief = nullptr;
};

// 编辑提案 / 应用 / 撤销。diff 由 workspace 生成，agent 只转述。
// 展示格式："── path（N 处编辑）" 文件头 + "@@ -a,n +b,m @@"
// hunk 集 + 行号行（"NNNN + 内容"）+ 统计尾行；tui/webui 按行首特征解析着色。
struct EditProposed {
    static constexpr std::string_view name = "vc.edit.proposed";
    int turn_id = 0;
    const char* diff_text = nullptr;  // 行级 diff 展示文本，调用期有效
    // 尾部追加（M-W7）：提案工具名（"runner.run" 等）——按工具区分批准卡形态用。
    // 当前消费方（webui/host）都不读它（批准卡形态靠 diff_text 首行解析）——
    // 契约面保留待用，生产者照常填充。
    const char* tool = nullptr;
};
struct EditApplied {
    static constexpr std::string_view name = "vc.edit.applied";
    int turn_id = 0;
    const char* paths_json = nullptr;  // ["a","b"]（UTF-8 相对路径），转述用，调用期有效
    // 尾部追加：hunk diff 文本；旧生产者组合下为 null——
    // 消费方以非空探测。调用期有效，要留存必须拷贝。
    const char* diff_text = nullptr;
    // 尾部追加：每文件结构化统计 [{"path","add","rem","is_new"}]（调用期有效，旧
    // 生产者组合下为 null，消费方以非空探测）。diff_text 是给人看的展示文本且可能
    // 被截断，统计以本字段为准——消费方不得从展示文本反解（截断砍掉的是文本尾行
    // 的统计，踩过：大 diff 的「本轮修改」卡恒 +0−0）。
    const char* files_json = nullptr;
};
struct EditUndone {
    static constexpr std::string_view name = "vc.edit.undone";
    int turn_id = 0;
    int snapshot_depth = 0;  // 撤销后剩余的快照组数
};

// 子进程执行（agent → runner；runner → agent/tui）。run_id 全链路关联。
// 【已废弃】本事件无生产者也无消费者——实际路径是工具表 execute 同步发起，
// 输出/退出经 vc.run.line/exit 事件回。契约冻结只标注不删声明。
struct RunRequest {
    static constexpr std::string_view name = "vc.run.request";
    int run_id = 0;
    int turn_id = 0;
    const char* command_line = nullptr;  // UTF-8 完整命令行，调用期有效
    const char* cwd = nullptr;           // jail 内相对路径；空 = jail 根
    int timeout_ms = 0;                  // 0 = 用 runner 默认
};
struct RunLine {
    static constexpr std::string_view name = "vc.run.line";
    int run_id = 0;
    int stream = 0;  // 0=stdout 1=stderr
    const char* text = nullptr;
    // 路由（slot 路由约定）：本事件按 slot = 发起 run 的会话槽 turn_id 单播——
    // 只有归属 ctx 收得到；主会话槽的行由 agent 重播到广播通道供 UI 消费。
    // 子代理槽的行不再出现在父会话 UI（slot 路由前本字段无隔离能力，踩过）。
};
struct RunExit {
    static constexpr std::string_view name = "vc.run.exit";
    int run_id = 0;
    unsigned int exit_code = 0;
    // 路由：同 RunLine（slot = 发起 run 的会话槽 turn_id）。
};

// 状态栏级提示（任意 → tui）。level：0=trace 1=debug 2=info 3=warn 4=error。
// sys（尾部追加，M-W10）：系统状态通告（如权限模式切换）——webui 渲染为居中
// 分隔样式、与对话内容区分；0/缺省 = 普通对话行。
struct Notice {
    static constexpr std::string_view name = "vc.notice";
    int level = 0;
    const char* text = nullptr;
    int sys = 0;
};

// 会话切换（webui → agent）：加载指定 state 文件（M6 格式：messages/turns/usage），
// 并把后续落盘重定向到该路径。仅在 Idle 时有效；turn 进行中 agent 拒绝并日志。
// path 调用期有效，要留存必须拷贝。
struct SessionLoad {
    static constexpr std::string_view name = "vc.session.load";
    const char* path = nullptr;
};

// 工作区热切（webui 设置 → 持有 jail/store 的插件）：重建边界与存储根。
// root 为绝对路径（UTF-8），调用期有效，要留存必须拷贝。webui 保证 Idle 时才切。
struct WorkspaceSet {
    static constexpr std::string_view name = "vc.workspace.set";
    const char* root = nullptr;
};

// 权限模式三档（M-W7，Codex 式；webui 设置面板/顶栏切换，宿主启动 env 可设默认）：
//   suggest    —— 逐条问：编辑与命令都走提案→批准（runner.run/apply 的 dry_run 试算）
//   auto-edit  —— 编辑免批（applied 携带 diff 事后展示），命令逐条问
//   full-auto  —— 全自动：命令在 AppContainer 沙箱内执行（文件系统限工作区、断网）
// agent 据此重算确认清单；runner 据此决定沙箱启动。mode 调用期有效，要留存必须拷贝。
struct PermissionMode {
    static constexpr std::string_view name = "vc.permission.mode";
    const char* mode = nullptr;
};

// provider 热切（M2 起；M-W8 尾部追加 base_url——多供应商下按「当前使用」供应商
// 热切 Key 与请求目标；base_url 为空 = 保持现值（兼容旧生产者）。
struct LlmSetActive {
    static constexpr std::string_view name = "vc.llm.set_active";
    const char* provider = nullptr;
    const char* key = nullptr;
    // 尾部追加（M-W8）：Base URL（UTF-8）；空 = 保持现值。调用期有效，要留存必须拷贝。
    const char* base_url = nullptr;
    // 尾部追加（上下文压缩阶段二）：
    // 当前模型上下文窗口（tokens；0 = 未知）——agent 据此做 0.9×W 动态压缩阈值。
    // 旧生产者组合下为 0（消费方回退兜底阈值）。
    int context_window = 0;
    // 尾部追加（子 agent）：子 agent 模型档案三元组——webui 从当前主模型的
    // sub_model 配置解析出目标供应商的模型/Key/Base URL 随本事件下发；provider
    // 对 body 带 "subagent":true 的请求改用这组凭据发起（svc 单槽互斥下跨
    // provider 派遣的唯一通路）。任一为 null/空 = 无子档案，子请求回落主模型。
    // 均调用期有效，要留存必须拷贝。
    const char* sub_model = nullptr;
    const char* sub_key = nullptr;
    const char* sub_base_url = nullptr;
};

inline constexpr std::uint64_t turn_input = nostos::event_id<TurnInput>;
inline constexpr std::uint64_t turn_decision = nostos::event_id<TurnDecision>;
inline constexpr std::uint64_t tool_register = nostos::event_id<ToolRegister>;
inline constexpr std::uint64_t llm_delta = nostos::event_id<LlmDelta>;
inline constexpr std::uint64_t llm_reasoning = nostos::event_id<LlmReasoning>;
inline constexpr std::uint64_t llm_done = nostos::event_id<LlmDone>;
inline constexpr std::uint64_t llm_error = nostos::event_id<LlmError>;
inline constexpr std::uint64_t tool_call = nostos::event_id<ToolCall>;
inline constexpr std::uint64_t tool_result = nostos::event_id<ToolResult>;
inline constexpr std::uint64_t edit_proposed = nostos::event_id<EditProposed>;
inline constexpr std::uint64_t edit_applied = nostos::event_id<EditApplied>;
inline constexpr std::uint64_t edit_undone = nostos::event_id<EditUndone>;
inline constexpr std::uint64_t run_request = nostos::event_id<RunRequest>;
inline constexpr std::uint64_t run_line = nostos::event_id<RunLine>;
inline constexpr std::uint64_t run_exit = nostos::event_id<RunExit>;
inline constexpr std::uint64_t notice = nostos::event_id<Notice>;
inline constexpr std::uint64_t session_load = nostos::event_id<SessionLoad>;
inline constexpr std::uint64_t workspace_set = nostos::event_id<WorkspaceSet>;
inline constexpr std::uint64_t permission_mode = nostos::event_id<PermissionMode>;
inline constexpr std::uint64_t llm_set_active = nostos::event_id<LlmSetActive>;

// MCP 配置已保存（webui 设置面板 → mcp 插件热重载）：mcp 插件收到后重读 config
// 的 mcp.servers 并重启全部 server 会话，完成后回发 vc.mcp.status。
struct McpReload {
    static constexpr std::string_view name = "vc.mcp.reload";
    int reserved = 0;
};

// MCP server 状态回发（mcp 插件 → 潜在订阅方）。当前 webui 不订阅本事件——
// 设置面板的状态行读 mcp-status.json 落盘文件（mcp 插件 emit_status_ 同步写）；
// 事件面为未来订阅方保留。json（UTF-8，调用期
// 有效，要留存必须拷贝）：
//   {"ok":true,"servers":[{"name":"fs","alive":true,
//                          "tools":["read_file",...],"diag":"仅失败时"}]}
struct McpStatus {
    static constexpr std::string_view name = "vc.mcp.status";
    const char* json = nullptr;
};

// 子代理活动（agent → webui，广播）：主会话的工作记录视图——落 transcript 供
// UI 展示与重放，但**不进任何 LLM 上下文**（父上下文只收 task 工具的最终结果，
// 子会话的 messages 隔离在子 AgentCtx）。消费方按 turn_id（= 子会话槽 turn_no）
// 聚合为单个子代理的卡片与过程视图；delta 只广播不落盘（与 assistant_delta
// 同款——实时性归流式，重放不需要逐字过程）。
// phase：0=spawn 1=delta 2=tool_call 3=tool_result 4=done 5=fail。
// tool：spawn=类型名 / tool_call|tool_result=工具全名；其余为 null。
// text：spawn=任务描述 / delta=流式增量 / tool_*=参数与结果人话摘要 /
//       done=最终报告（截断）/ fail=失败原因。均调用期有效，要留存必须拷贝。
struct SubagentEvent {
    static constexpr std::string_view name = "vc.subagent.event";
    int turn_id = 0;
    int phase = 0;
    const char* tool = nullptr;
    const char* text = nullptr;
};

inline constexpr std::uint64_t subagent_event = nostos::event_id<SubagentEvent>;

inline constexpr std::uint64_t mcp_reload = nostos::event_id<McpReload>;
inline constexpr std::uint64_t mcp_status = nostos::event_id<McpStatus>;

// ---- 插件 UI 贡献（声明式设置 schema，方案 A）--------------------------------
// 带设置 UI 的插件在 activate 时 emit register（webui 动态生成设置 tab）；webui
// 在自己 activate 末尾 emit query——已激活插件收到后**重发** register。两个方向
// 合起来消解装载时序：webui 先启动 ⇒ 插件 register 直接命中；webui 后启动 ⇒
// 靠 query 补拉。schema 的**值**不进事件——存 config.json 的 plugins.<plugin>
// 节（webui 设置面板读写，插件启动/reload 时读取），事件只传"界面长什么样"。
// 控件集 v1（schema_json 数组元素）：{"key","label","type":"text|number|bool|
// select|path","options"?:[...],"placeholder"?,"default"?}
struct UiRegister {
    static constexpr std::string_view name = "vc.ui.register";
    const char* plugin = nullptr;      // 插件标识（config 的 plugins 节名，如 "mcp"）
    const char* label = nullptr;       // 设置页节标题（如 "MCP 超时"）
    const char* schema_json = nullptr; // 控件描述数组（JSON 文本），调用期有效
};
struct UiQuery {
    static constexpr std::string_view name = "vc.ui.query";
    int reserved = 0;
};

inline constexpr std::uint64_t ui_register = nostos::event_id<UiRegister>;
inline constexpr std::uint64_t ui_query = nostos::event_id<UiQuery>;

// mode 归一助手（契约语义的一部分，两端共用）：未知取值回落 auto-edit
inline bool valid_permission_mode(const std::string& m) {
    return m == "suggest" || m == "auto-edit" || m == "full-auto";
}
inline std::string normalize_permission_mode(const std::string& m) {
    return valid_permission_mode(m) ? m : std::string("auto-edit");
}

}  // namespace vc_api
