// plugin/agent —— turn 状态机 + 泛型工具桥 + 批准流 + 异步长任务归流 + 子代理派发。
//
// 翻译单元职责地图（按功能切片，AgentCtx/AgentPlugin 声明为本文件）：
//   src/plugin.cpp       —— AgentPlugin 薄壳（订阅分发到 ctx_）、manifest + NOSTOS_PLUGIN 入口
//   src/ctx_events.cpp   —— 事件处理器（输入/done/错误/裁决）
//   src/ctx_turn.cpp     —— turn 状态机（分发循环/收口/异常兜底）
//   src/ctx_compress.cpp —— 上下文压缩与外置存储（raw 归档与 recall/阈值/判定/折叠/摘要）
//   src/ctx_child.cpp    —— 子代理派发与收口（task 工具运行时）
//   src/ctx_session.cpp  —— 会话持久化（state.json 读写）+ mission 任务目标卡
//   src/ctx_tools.cpp    —— 工具桥（discover/exec/undo/事件回发）+ 工具调用/结果人话摘要
//   src/ctx_strategy.cpp —— vc.ctx.v1 策略缝（表解析 + plan/ingest/recall/摘要槽包装）
//   src/agent_types.cpp  —— 子代理类型注册表（.ithaca/agent/*.md 快照加载 + 内置 general）
//
// **死锁纪律（本插件的最高规则）**：事件处理器内禁止同步等待任何事件——提案后
// 即返回，decision 事件驱动状态机恢复；turn 进行中的输入入队。单线程事件环里
// 一处同步等待 = 整个宿主死锁（回归用例专门覆盖本条）。
//
// 职责边界：编排/上下文/审批策略执行在本插件；不碰文件、不起进程、
// 不解析工具语义（工具自述经 list() 反射透传给 LLM）。
//
// 工具发现：env ITHACA_AGENT_TOOLS（svc 名清单，默认 workspace/office/mcp/runner
// 四族），每次 turn 开始按清单取表（缺表跳过并通告）。审批策略：suggest 逐条问 /
// auto-edit 编辑免批、命令逐条问 / full-auto 全自动（沙箱由 runner 负责）；
// ITHACA_PERMISSION_MODE 设默认，vc.permission.mode 事件热切。
//
// 流（一个 turn）：turn.input → 组装 messages → chat_ex（带 tools）→ 流式 delta
// → done(完整 assistant 消息) → 有 tool_calls ⇒ 逐条分发（需批准→提案挂起 /
// 异步→登记等归流 / 同步→直执）⇒ 全部就位按序回灌 → 下一轮 → … → 无
// tool_calls ⇒ turn 完成。

#pragma once

#include "../tools_bridge.hpp"  // vcagent：工具清单 CSV 拆分 / 自述 JSON 构造 / 工具集名映射

#include "vcagent/context.hpp"  // 上下文压缩阶段一（mission 卡/最小充分记录/raw 外置/迟滞压缩）
#include "../permission.hpp"  // 权限模式三档（归一/确认清单映射）

#include "vc_ctx.hpp"
#include "vc_events.hpp"
#include "vc_llm.hpp"
#include "vc_tool.hpp"

#include "vcws/text.hpp"  // vcws::trim_utf8：码点安全截断的收敛实现，勿再按字节切

#include <nostos/abi/plugin.hpp>
#include "../scratch.hpp"  // Scratch：emit 载荷稳定缓冲（调用期有效）
#include <vckit/slot_call.hpp>  // 表槽 JSON 调用助手（出参生命周期口径单源）
#include <vckit/poster.hpp>   // poster_submit：dispatch 推迟一拍的投递原语

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include "nlohmann/json.hpp"
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace vc_agent {

// provider 未回报 usage 时的压缩判定信号粗估（中英混合经验系数，宁高勿低）：
// CJK ≈ 1 token/字；其余 3 字节 ≈ 1 token；另加 1/16 的 JSON 结构开销。只服务
// 压缩判定——估算值不进用量累计（计量以 provider usage 为准）。
inline long long estimate_prompt_tokens(const std::string& body) {
    std::size_t cjk = 0, other = 0;
    for (std::size_t i = 0; i < body.size();) {
        const unsigned char c = static_cast<unsigned char>(body[i]);
        if (c >= 0xE0) { ++cjk; i += 3; }          // 三字节起的多字节首字节（CJK 居多）
        else if (c >= 0xC0) { ++other; i += 2; }   // 两字节首字节（拉丁扩展等）
        else { ++other; i += 1; }
    }
    return static_cast<long long>(cjk + other / 3 + (cjk + other) / 16);
}

// state 文件路径 → 会话 id：<...>/<id>.state.json 的 <id>。事件尾部 session 标
// 与 AgentPlugin 槽键的派生单点——写两份必然漂移
inline std::string session_id_of_state(const std::string& path) {
    std::string name = fs::path(path).filename().string();
    const std::string suffix = ".state.json";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        name.erase(name.size() - suffix.size());
    return name;
}

using nlohmann::json;

// 工具级失败的标准回灌形态（ok:false + error——给模型的正常通路，非基础设施错误）
inline std::string err_result(const std::string& msg) {
    return json{{"ok", false}, {"error", msg}}.dump();
}

// 超窗强制折叠重发的次数上界。自然链 ≤2
// （多段折叠 → 单段折叠 → 无可折叠），上界防未知消息形态下「折叠不够 → 仍超窗」
// 无限打转。
inline constexpr int kMaxOverwindowResend = 2;

// 子代理护栏：并发上限远低于 provider 的并发额度（llm_cloud kMaxConcurrent=8，
// 父请求共享同一额度）；轮数上限防失控任务——子代理不做上下文压缩，长循环只会
// 单调膨胀。
inline constexpr int kMaxChildren = 4;
inline constexpr int kMaxChildRounds = 30;

// 子代理类型（一个 .ithaca/agent/<名>.md；内置 general 恒在，同名 md 覆盖之）。
// 类型只管提示词与工具面——子代理用哪个模型走配置链路（当前主模型的 sub_model
// 经 vc.llm.set_active 子档案下发），类型不设 model 字段。
struct AgentType {
    std::string name;
    std::vector<std::string> tools;  // 工具集白名单（去 ithaca.tool. 前缀的集合名）；空 = 继承父全量
    std::string prompt;              // 正文 = 子代理的 system prompt
};

// ---- AgentCtx：一个会话槽的全部状态与方法 ----------------------------------
// 把写死在插件单例里的会话状态机抽成可实例化结构。子代理（父 turn 内嵌套一个
// 受限 ctx 跑循环）与多会话槽只是多实例化它；方法体只触碰自身状态与 host_
// （activate 时灌入），天然可重入。
struct AgentCtx {
    enum class State { Idle, TurnActive, AwaitingDecision };

    // 全局唯一计数（多槽/子代理共用：turn_id 与 request_id 跨 ctx 不冲突——
    // 事件/快照分组按 id 反查的根基）。request_id=0 保留给 agent 自发收口事件。
    // NOTE: 发号与恢复抬升必须共用同一个计数器——抬到旁路计数器等于没抬，
    // 恢复会话后新 turn 仍从 1 重来，raw/ 归档按 turn 命名即跨会话互相覆盖
    // （踩过: adopt 曾抬在第二个 static 上，恢复即撞号）。
    static std::atomic<int>& turn_seq() { static std::atomic<int> seq{0}; return seq; }
    static int next_turn_no() { return ++turn_seq(); }
    static int next_chat_seq() { static std::atomic<int> seq{0}; return ++seq; }
    // 会话恢复时把全局计数抬到恢复值之上（防新 turn 与历史 turn_id 撞号）
    static int adopt_turn_floor(int restored) {
        std::atomic<int>& seq = turn_seq();
        int cur = seq.load();
        while (restored > cur && !seq.compare_exchange_weak(cur, restored)) {}
        return restored;
    }

    // 当前 assistant 消息的工具调用记账
    struct ToolCallCtx {
        std::string call_id;
        std::string full_name;
        std::string tool;
        json args;
        bool done = false;
        std::string result;
    };
    struct AsyncRun {
        std::size_t index = 0;       // 对应 calls_ 下标
        std::string output;          // 已收输出行
        bool exited = false;
        std::uint32_t exit_code = 0;
    };

    // ---- 事件处理器（全部在宿主线程；禁止同步等待）-----------------------------
    // 每个入口 try/catch 包体（*_impl）→ fail_unexpected_ 收口：turn 机器任何
    // 一段的意外异常都转成本 turn 的失败终态，绝不悬停 TurnActive（悬停无任何
    // 用户可见痕迹，重启后整段交换从历史蒸发）。
    void on_input(const std::string& text, const std::string& attachments_json);
    void on_done(const vc_api::LlmDone& e);
    void on_llm_error(const vc_api::LlmError& e);
    void on_decision(const vc_api::TurnDecision& d);
    void on_session_cmd(const vc_api::SessionCmd& c);
    void on_input_impl(const std::string& text, const std::string& attachments_json);
    void on_done_impl(const vc_api::LlmDone& e);
    void on_llm_error_impl(const vc_api::LlmError& e);
    void on_decision_impl(const vc_api::TurnDecision& d);
    void on_session_cmd_impl(const vc_api::SessionCmd& c);

    // ---- turn 状态机（方法体在 src/ctx_turn.cpp）----------------------------

    void begin_turn(const std::string& text, const std::string& attachments_json);
    void send_chat();
    void send_chat_impl();
    void dispatch_loop();
    // dispatch 的推迟一拍入口（on_done 专用；动机与安全边界见 ctx_turn.cpp 实现）。
    void dispatch_after_done_();
    // 单条调用分发（dispatch_loop 的 try 保护体）。返回 true = 挂起即返（提案
    // 等裁决 / 回灌已就地发车）——调用方必须跳过循环尾：挂起期跑尾部的
    // append_results_and_continue 会把无结果的轮次提前收口。
    bool dispatch_one_(ToolCallCtx& ctx);
    // 单条调用统一收尾：结果卡 emit + 分发游标推进（调用点先写好 ctx.result）
    void settle_(ToolCallCtx& ctx, bool ok);
    void append_results_and_continue();
    void append_results_and_continue_impl();
    void try_finish_tools();
    void try_finish_tools_impl();
    // 确认清单 = 权限模式映射（纯函数在 permission.hpp，单测直接覆盖它）
    void rebuild_confirm_();
    void finish_turn();
    // agent 内部失败的统一收口（哨兵语义与子代理静默分支见 ctx_turn.cpp 实现）
    void fail_turn(const std::string& why);
    // turn 机器统一异常兜底（事件处理器入口 catch 体 / 回灌与发送原语的 catch 体）：
    // 重掷当前异常取 what() 后转 fail_turn 终态（错误卡 + 状态机复位）。compress
    // 在途先清——异常语境下摘要请求不可信，悬着会让 cleanup_turn_ 早退继续悬停；
    // 迟到的摘要回包按 request_id 失配丢弃。noexcept：兜底绝不成为新故障源，
    // fail_turn 链自身再抛就强制回 Idle（host_.log 不抛，可安全留痕）。
    void fail_unexpected_() noexcept;
    // 状态机复位：持久化 + 清现场 + 压缩判定 + 回 Idle + flush 排队（收口事件归调用方）。
    // keep_partial = 主流式取消（用户裁决 d=4）：中断现场保留在上下文——未决调用注入
    // 「已被用户取消」合成结果使序列配对合法，已完成结果原样保留（模型知道停在哪，
    // 下轮可续）；默认 false = 截断悬挂回合（异常/失败收口的兜底，注入不可信的语境）
    void cleanup_turn_(bool keep_partial = false);
    // 取消现场配对（keep_partial 路径）：未决 tool_call 注入合成结果并回灌进历史
    void settle_cancelled_calls_();
    // 撤回中断回合（会话命令 cmd=2，中断卡 ↩ 按钮触发）：整轮回退到该轮之前
    //（模型上下文含 user 消息一并撤出）；webui 侧同令回退转录面。只认尾部现场
    void revoke_interrupt_();
    // 清理悬挂的工具回合尾巴（provider 400 防护，见 ctx_turn.cpp 实现）
    void truncate_dangling_tool_exchange_();

    // ---- 上下文压缩（方法体在 src/ctx_compress.cpp）---------------------------

    // 原始工具结果外置：raw/<turn>_<seq>_<tool>.json；失败 ⇒ 空 ref（调用方内联截断）
    std::string spool_raw(const std::string& tool, const std::string& result);
    // context.recall：ref 校验（纯文件名）→ 读归档 → provenance 包装
    std::string exec_recall(const json& args);

    // 阈值命中后的异步摘要请求：冻结折叠区间，发无 tools 的单轮 chat_ex；
    // 失败（服务缺失/未受理）就地回退机械应用。
    void request_digest_();
    // 折叠应用共用出口（LLM 摘要/机械 digest）：替换 + 持久化 + 回 Idle + flush 排队；
    // 置 compress_verify_ 退避校验。
    void apply_digest_(const json& digest_msg, bool via_llm);
    // 摘要回包处理（on_done/on_llm_error 按 compress_seq_ 分流到这）：校验/补
    // ref/截断后应用；摘要失败一律回退 pending_plan_.digest。
    void on_compress_done_(const vc_api::LlmDone& e);
    void on_compress_error_(const vc_api::LlmError& e);
    // 生效阈值重算：W 已知 ⇒ 按输出预留重算；未知 ⇒ 兜底阈值（compress_threshold_）。
    void set_context_window_(long long w);
    void recompute_threshold_();
    // 压缩判定共用入口（finish_turn 与 cleanup_turn_——失败/取消收口同样过判定，
    // 否则反复取消/失败的长会话上下文只增不减）：命中 ⇒ 产出折叠计划（LLM 摘要
    // 异步或机械），不可折叠 ⇒ 返回 false，调用方走原收口路径。
    bool maybe_compress_();
    // 折叠应用核心（LLM 摘要 / 机械 digest / 超窗强制三路共用）：区间替换 + 旧
    // digest 并入 + 持久化 + 通告。先插 digest 再跳过的顺序纪律见阶段一。
    void replace_with_digest_(const json& digest_msg, const char* via);
    // 超窗就地自救：应用冻结的机械折叠后重发当前请求（turn 保持在途，见实现）
    void forced_fold_resend_();
    // 排队输入 flush：队首开新 turn（栈深有界——一次一条，其余等该 turn 收口再取）。
    void flush_queued_inputs_();

    // 本槽事件订阅（begin_turn 开头调用，slot 路由约定见 contract/vc_events.hpp）：
    // 归属本槽的 run/LLM 事件单播直达本 ctx；主会话额外把 run 事件重播到广播
    // 通道（webui/selftest 的 UI 消费路径不变），子会话槽不重播——不可见性由
    // "订阅才可达"保证。
    void subscribe_turn_slot_();

    // ---- 上下文策略缝（vc.ctx.v1 单点解析；src/ctx_strategy.cpp）----------------

    // 轮边界解析策略表 + capabilities 自述缓存。null = 内置回退（不
    // inject——缺插件零行为变化）；轮边界生效，在途 turn 不换策略。
    void resolve_ctx_strategy_();
    // 压缩计划统一口：表接管 ⇒ messages/signals 打包调 plan 槽（判定门槛在
    // 策略内，action=none ⇒ 无效计划）；estimate ≥ 0 时并入 signals。无表返回
    // 无效计划——调用点自行走内置门槛 + plan_compress。
    vcagent::CompressPlan acquire_plan_(const char* reason, long long estimate = -1);
    // 策略缝取计划单点：表接管走 plan 槽，无表回落内置 plan_compress（raw 开关随取）
    vcagent::CompressPlan plan_now_(const char* reason, long long estimate = -1);
    // 「LLM 摘要可得」单点：表接管看 summary_prompt 槽，内置路径恒有摘要指令
    bool llm_summary_ready_() const;
    // mission 槽调用收敛（replace_with_digest_ / ensure_mission_ 共用）：表接管 =
    // dump messages → mission 槽 → parse object（null/失败/非对象 = false，处置
    // 归调用方）；无表 = 内置 build_mission（恒 true）
    bool mission_from_slot_or_builtin_(json& out);
    // 表接管的工具结果收编（槽失败 ⇒ 原文内联兜底，调用方保底通路）
    std::string strategy_ingest_(const std::string& full_name, const std::string& content,
                                 std::size_t seq);
    // 表接管的摘要请求素材（槽失败保持调用方传入的内置值不变）
    void strategy_summary_brief_(std::string& system, std::string& instructions);
    // 表接管的摘要回包后处理。返回 false = 槽失败，调用方机械 digest 兜底
    bool strategy_apply_summary_(const std::string& summary, json& out_digest);

    // ---- 子代理派发（运行时在 src/ctx_child.cpp；类型加载在 src/agent_types.cpp）------

    // task 内置工具：校验参数与护栏 → 起子 ctx 事件驱动跑循环 → 登记在途。
    // 返回 false = 拒绝（result 已写好，调用方 emit 后继续分发）；true = 已 spawn
    // 或已在同帧内经回调就地收口（begin_turn 同步失败的路径）。
    bool spawn_child_(std::size_t index, ToolCallCtx& call);
    // 子代理完成回调（宿主线程，可能在子事件处理栈内同步派发）：用量并入本会话、
    // 结果写回 calls_[index]、统一收口判定（门槛与 try_finish_tools 同一门）。
    void on_child_finished_(std::size_t index, AgentCtx* child, bool ok,
                            const std::string& report, int turns);
    // 类型清单重载（主会话每 turn 开始）：目录快照扫描 + 内置 general 兜底。
    void load_agent_types_();

    // ---- 会话持久化与 mission 卡（src/ctx_session.cpp）--------------------------

    // state 文件加载（activate 与会话切换共用；缺文件 = 全新会话）。
    // 撤销快照栈是内存态，恢复后从零开始（文档明示）
    void load_session_state_(const std::string& path);
    // M6 session 持久化：历史 + turn 计数 + 用量落盘（失败仅告警，不影响任务）
    void persist_session_();
    // mission 任务目标卡建档（仅缺卡时插到 system 之后；跨轮冻结——缓存前缀
    // 纪律，锚点在压缩折叠时吸收，折叠重建走 replace_with_digest_）
    void ensure_mission_();

    // ---- 工具桥（src/ctx_tools.cpp）-----------------------------------------------

    struct Bound {
        std::string svc_name;
        const vc_api::ToolTable* table = nullptr;
    };

    bool discover_tools();
    const Bound* find_bound(const std::string& full_name) const;
    std::string exec(const Bound& b, const std::string& tool, const json& args);
    void exec_undo();
    void emit_tool_call(const std::string& name, const json& args);
    void emit_tool_result(const std::string& name, bool ok, const std::string& brief);
    // 子代理活动广播（vc.subagent.event，主会话工作记录视图——落 transcript 不进
    // 上下文）：spawn/delta/tool/收口各点共用此出口。turn_id 必须传子的——UI 按
    // turn_id 分组，spawn 发生在父 ctx，带父号会让记录掉错组（踩过）
    void emit_subagent_(int turn_id, int phase, const char* tool, const std::string& text);
    void notice(const std::string& text);
    // 带机器路由元数据的通告（Notice 尾部追加字段）：kind 标类别，前端状态机按
    // 它路由（text 仅供人读）；run_id/count 按 kind 语义填充
    void notice_meta(const std::string& text, std::string_view kind,
                     long long run_id = 0, int count = 0);
    // 收口门槛：全部后台 run 已归流（三处判定共用一份口径）
    bool all_runs_exited_() const;
    // 异步长任务登记：run_id → 分发序号（结果等 vc.run.exit 归流后按序回卡）
    void register_run(int run_id, int index);
    static std::string trim(const std::string& s, std::size_t cap);
    // 调用参数 → 人话摘要（解析不出 ⇒ 空，回退原 JSON 截断）。static 保持纯函数
    // 可白盒测——工作区根由调用方传入（"." 形态的路径呈现需要它）
    static std::string summarize_call(const std::string& tool, const json& args,
                                      const std::string& ws_root);
    // apply 参数 → op 清单："试算 write a.txt；replace b.txt ×2"（解析不出 ⇒ 空）
    static std::string summarize_apply_ops(const json& args);
    // 工具结果 → 人话摘要；失败点名首条错误；认不出的形态回退原 JSON 截断
    static std::string summarize_result(const std::string& tool, const std::string& result);
    static int parse_run_id(const std::string& result);
    static bool tool_ok(const std::string& result);
    AsyncRun* find_run(int run_id);

    // ---- 状态 -----------------------------------------------------------------------

    nostos::abi::Host host_{};
    // dispatch 推迟投递用（activate 时领取；领取失败保持零值 ⇒ 回退同步分发）。
    nostos_poster poster_{};

    const vc_api::LlmTable* llm_ = nullptr;  // 每次 send_chat 前解析（provider 可被竞争顶掉）
    const vc_api::LlmTable* window_probed_llm_ = nullptr;  // 已向 info() 探过窗口的 provider 实例
    long long ctx_max_output_ = 0;      // provider 自述的输出上限（info()；消费方 = 发送前输出预留）
    // 上下文策略表（vc.ctx.v1）：轮边界解析、轮内持有；null = 内置同名逻辑。
    // NOTE: 不 manifest inject——inject 会让无插件时 LazyGate 永远 PENDING；
    // 本表是"可选依赖 + 内置回退"，缺插件零行为变化。
    const vc_api::CtxTable* ctx_tbl_ = nullptr;
    bool ctx_cap_recall_ = false;       // 策略自述 capabilities 含 recall ⇒ 暴露 context.recall

    State state_ = State::Idle;
    int turn_no_ = 0;
    int chat_seq_ = 0;
    int inflight_ = -1;
    bool auto_all_ = false;

    // M6：token 用量累计 + session 持久化
    long long usage_prompt_ = 0;
    long long usage_completion_ = 0;
    long long usage_total_ = 0;
    std::string session_path_;
    long long usage_cache_ = 0;         // 缓存命中 token 累计（provider 可选回报）

    // 上下文压缩
    long long last_prompt_tokens_ = 0;  // provider 回报的最近 prompt_tokens（判定信号）
    bool last_usage_real_ = false;      // 上次信号是真实 usage 回报（false = 粗估兜底
                                        // ——策略缝的退避判定只认真实回报，透传用）
    long long compress_threshold_ = 32000;  // W 未知时的兜底阈值（阶段一语义）
    long long ctx_window_ = 0;          // 当前模型上下文窗口（0 = 未知；env > set_active）
    bool ctx_window_env_ = false;       // env ITHACA_CONTEXT_WINDOW 已设 ⇒ set_active 事件不覆写
    long long session_threshold_ = 32000;  // 生效阈值（set_context_window_ 重算 / 退避抬升）
    bool summary_enabled_ = true;       // ITHACA_CONTEXT_SUMMARY=off ⇒ 永远机械折叠
    int compress_seq_ = -1;             // 摘要请求的 request_id（-1 = 无在途摘要）
    bool compress_inflight_ = false;    // 摘要在途：排队输入挂起，on_done/error 分流
    bool compress_verify_ = false;      // 压缩后首 turn 校验：仍超阈值 ⇒ 阈值 ×1.25 退避
    int compress_retries_ = 0;          // 本 turn 超窗强制重发计数（begin_turn 重置；
                                        // kMaxOverwindowResend 封顶，防折叠打转）
    vcagent::CompressPlan pending_plan_{};  // 冻结的折叠计划（机械 digest = 兜底产物）
    std::string tools_snapshot_;        // 上一轮工具表快照（字节级稳定观测）
    std::string raw_dir_;    // 外置存储目录（随 session_load 指向的会话文件走；空 = 未启用）
    std::string raw_prefix_; // 归档名前缀 = 会话文件 stem（多会话共享 raw/ 时防跨会话撞名）

    // 当前 assistant 消息的工具调用记账
    // NOTE: calls_/runs_/messages_ 等全部 turn 状态无常锁——单线程纪律：所有触碰
    // 它们的事件回调（llm_done/error 槽订阅、run 归流、子代理完成回调）都经
    // poster 收敛到宿主线程（llm_cloud/runner 的 emit 亦然），与 agent 状态机
    // 同一线程序列化；本类成员禁止任何线程加锁访问。此不变式若破（有事件源
    // 改为 worker 线程直发），全部容器访问面需重新审计
    std::vector<ToolCallCtx> calls_;
    std::size_t dispatched_ = 0;
    std::map<int, AsyncRun> runs_;   // run_id → run

    json messages_ = json::array();
    json tools_ = json::array();
    std::string body_buf_;
    std::string args_buf_;

    std::vector<std::string> tool_svcs_;
    std::vector<std::string> confirm_;
    std::string mode_ = "auto-edit";  // 权限模式
    std::vector<Bound> bound_;
    struct InputItem {                  // 排队输入：文本 + 多模态附件载荷（JSON 数组串）
        std::string text;
        std::string attachments_json;
    };
    std::deque<InputItem> queued_;

    // 等待批准的上下文
    struct Pending {
        std::size_t index = 0;
    } pending_;

    // ---- 子代理（状态全部随 ctx 走，方法只触自身与 host_）----------------------

    int depth_ = 0;              // 0 = 主会话；≥1 = 子代理（不注册 task，嵌套深度 1）
    bool subagent_ = false;      // 子代理形态：免批/不压缩/不持久化/quiet/请求体带 subagent 标志
    std::string system_prompt_;  // 非空 ⇒ begin_turn 以此替代默认 kSystemPrompt（子代理 = 类型 prompt）
    int rounds_ = 0;             // 本 turn 的 LLM 轮数（子代理以 kMaxChildRounds 封顶）
    // 子代理完成回调：spawn 时设置；finish_turn（成功）/fail_turn（失败）带回报告
    // 与轮数。回调在宿主线程同步执行，内部禁止等待（死锁纪律同事件处理器）。
    std::function<void(bool ok, const std::string& report, int turns)> on_finished_;
    std::vector<std::unique_ptr<AgentCtx>> children_;  // 仅主会话持有；随 turn 现场清亡（cleanup_turn_）
    std::set<std::size_t> child_pends_;  // 在途子代理的 calls_ 下标（回调回灌结果并摘除）
    std::vector<AgentType> types_;       // 子代理类型清单（主会话每 turn 开始重载）
    std::string agent_dir_;              // 类型目录（<工作区>/.ithaca/agent；env ITHACA_AGENT_DIR 可覆盖）
    std::string ws_root_;                // 当前工作区根（env 起源 + workspace_set 热切跟随；
                                         // 人话摘要的路径呈现用——"." 要译成真实目录）
    std::string session_id_;             // 本槽归属会话 id（state 文件名派生）：事件尾部
                                         // session 标的填充源（多会话并行的路由键）
    // 本 turn 的槽订阅（run_line/run_exit + LLM 事件族——多会话并行后主会话请求
    // 也按槽单播，槽订阅后补 session 标重播进广播通道供 UI 消费）——begin_turn
    // 换订（上 turn 槽已无事件源），ctx 析构随 RAII 释放
    std::vector<nostos::abi::Host::Subscription> slot_subs_;

    // 槽事件补标重播：布局拷贝槽载荷、覆写 session 标后转发广播。源字符串由
    // provider 的 Scratch 保活到本次派发结束，嵌套 emit 内仍有效
    template <typename E>
    void retag_emit_(std::uint64_t evt, const E* e) {
        vc_agent::Scratch s;
        E tag = *e;
        tag.session = s.hold(session_id_);
        host_.emit(evt, &tag);
    }
};  // AgentCtx

// AgentPlugin：薄壳——订阅分发到多会话槽与子代理扇出。多会话并行：每个已加载
// 会话一个 AgentCtx（map 按会话 id 键），turn 输入/裁决按事件尾部 session 标路由；
// 事件路由键的回退 = active_（最近一次会话加载的槽，单驱动场景如 selftest 兼容）。
// 会话工作区一致性由 webui 门卫保证（跨工作区并行切换被拒）——本插件不做校验。
class AgentPlugin {
public:
    static constexpr std::string_view name = "vc.agent";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 1, 0);

    // manifest：inject vc.llm.v1 —— provider 未就绪时宿主不激活本插件（LazyGate
    // PENDING，依赖出现后自动拉起——内核 M2 宿主策略的真实用例）。
    static const nostos_manifest* manifest;

    nostos_status activate(const nostos::abi::Host& host);
    void deactivate() noexcept;

private:
    // 会话槽创建：env 派生的初始态（工具清单/权限模式/窗口阈值/类型目录/系统
    // 提示词）在此统一灌入——原 activate 的单槽初始化体
    AgentCtx& ensure_ctx_(const std::string& id);
    // 事件 session 标 → 槽：null/空回落 active_（单驱动兼容）；未知会话返回空
    AgentCtx* route_(const char* session);

    std::map<std::string, std::unique_ptr<AgentCtx>> ctxs_;
    AgentCtx* active_ = nullptr;     // 最近一次会话加载的槽（回退路由目标）
    nostos_poster poster_{};         // 全槽共享的提交通道（poster 无状态，句柄可复制）
    nostos::abi::Host host_{};       // activate 期捕获：延迟创建的槽要灌 host 面
    // 全局活值：事件先于槽创建到达的时序兜底（webui activate 的模式/窗口/工作区
    // 回放先于首个 session_load——彼时 ctxs_ 为空，只记活值，ensure_ctx_ 套用）
    std::string mode_live_{};        // 权限模式事件值（空 = 未收到，env 兜底）
    long long window_live_ = 0;      // 模型上下文窗口事件值（0 = 未收到）
    std::string ws_live_{};          // 工作区根事件值（空 = 未收到，env 兜底）

    nostos::abi::Host::Subscription sub_input_{};
    nostos::abi::Host::Subscription sub_decision_{};
    nostos::abi::Host::Subscription sub_cmd_{};    // 会话级命令（vc.session.cmd）
    nostos::abi::Host::Subscription sub_session_{};
    nostos::abi::Host::Subscription sub_mode_{};
    nostos::abi::Host::Subscription sub_llm_set_{};     // 模型窗口事件（阶段二动态阈值）
    nostos::abi::Host::Subscription sub_workspace_{};   // 工作区热切（子代理类型目录跟随）
};

}  // namespace vc_agent
