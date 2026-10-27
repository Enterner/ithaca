// plugin/agent —— turn 状态机 + 泛型工具桥 + 批准流 + 异步长任务归流 + 子代理派发。
//
// 翻译单元职责地图（按功能切片，AgentCtx/AgentPlugin 声明为本文件）：
//   src/plugin.cpp       —— AgentPlugin 薄壳（订阅分发到 ctx_ 与子代理扇出）、manifest + NOSTOS_PLUGIN 入口
//   src/ctx_events.cpp   —— 事件处理器（输入/done/错误/裁决）
//   src/ctx_turn.cpp     —— turn 状态机（分发循环/收口/上下文压缩执行/子代理 spawn 与回灌）
//   src/ctx_session.cpp  —— 会话持久化（state.json 读写）+ mission 任务目标卡
//   src/ctx_tools.cpp    —— 工具桥（discover/exec/undo/事件回发）+ 工具调用/结果人话摘要
//   src/agent_types.cpp  —— 子代理类型注册表（.ithaca/agent/*.md 快照加载 + 内置 general）
//
// **死锁纪律（本插件的最高规则）**：事件处理器内禁止同步等待任何事件——提案后
// 即返回，decision 事件驱动状态机恢复；turn 进行中的输入入队。单线程事件环里
// 一处同步等待 = 整个宿主死锁（回归用例专门覆盖本条）。
//
// 职责边界：编排/上下文/审批策略执行在本插件；不碰文件、不起进程、
// 不解析工具语义（工具自述经 list() 反射透传给 LLM）。
//
// 工具发现：env ITHACA_AGENT_TOOLS（svc 名清单，默认
// "ithaca.tool.workspace,ithaca.tool.mcp,ithaca.tool.runner"），每次 turn 开始按清单取表。
// 审批策略：suggest 逐条问 / auto-edit 编辑免批、命令逐条问 /
// full-auto 全自动（沙箱由 runner 负责）。确认清单 = 模式映射（rebuild_confirm_），
// ITHACA_PERMISSION_MODE 设默认，vc.permission.mode 事件热切。
//
// 流（一个 turn）：turn.input → 组装 messages → chat_ex（带 tools）→ 流式 delta
// → done(完整 assistant 消息) → 有 tool_calls？
//   ├─ 需批准（confirm 清单内，默认 runner.run）：dry_run 试算 → emit 提案 → **返回**
//   ├─ 异步工具（结果含 run_id>0，如 runner.run）：登记 → 等 vc.run.exit 归流
//   └─ 同步工具（编辑类默认走这里）：直接执行——落盘 + applied 事件由 workspace 发
//   → 全部工具结果就位 ⇒ 按序回灌 → 下一轮 chat_ex → … → 无 tool_calls ⇒ turn 完成。

#pragma once

#include "../tools_bridge.hpp"  // vcagent：工具清单 CSV 拆分 / 自述 JSON 构造 / 工具集名映射

#include "../context.hpp"  // 上下文压缩阶段一（mission 卡/最小充分记录/raw 外置/迟滞压缩）
#include "../permission.hpp"  // 权限模式三档（归一/确认清单映射）

#include "vc_events.hpp"
#include "vc_llm.hpp"
#include "vc_tool.hpp"

#include "../../workspace/text.hpp"  // vcws::trim_utf8：码点安全截断的收敛实现，勿再按字节切

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/scratch.hpp>  // Scratch：emit 载荷稳定缓冲（调用期有效）

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <nostos/nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace vc_agent {

// provider 未回报 usage 时的压缩判定信号粗估（批次二-6/四-19；中英混合经验系数，
// 宁高勿低）：CJK 字 ≈ 1 token/字；其余按 3 字节 ≈ 1 token（英文实际 ≈4B/token，
// 分母取小即偏高）；再加 1/16 的 JSON 结构与转义开销。只服务压缩判定——估算值
// 不进用量累计（计量以 provider usage 为准）。
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

using nlohmann::json;

// 超窗强制折叠重发的次数上界。自然链 ≤2
// （多段折叠 → 单段折叠 → 无可折叠），上界防未知消息形态下「折叠不够 → 仍超窗」
// 无限打转。
inline constexpr int kMaxOverwindowResend = 2;

// 子代理护栏：并发上限远低于 provider 的并发额度（llm_cloud kMaxConcurrent=8，
// 父请求共享同一额度）；轮数上限防失控任务——子代理不做上下文压缩，长循环只会
// 单调膨胀。
inline constexpr int kMaxChildren = 4;
inline constexpr int kMaxChildRounds = 30;

// 内容型工具结果的分工具内联上限（对齐主流形态：限制收口在工具侧——read 按
// 行分页、task 报告即子代理的最终产出）。
// · read：96KB——正文是逐字工作素材（编辑依据），一次内联回灌，外置会让主/子
//   会话为拿到正文打几十次 recall，踩过。96KB 给行号前缀（每行 ≈7 字节）留
//   余量，代码类内容 ≈25K token。
// · task：1KB——报告是「结论消费型」内容，而实测会话（24 轮/92K tok）五份
//   报告 35.3KB 全文内联、折叠后模型仍以 4KB 满页 18 次整卷回读归档——内联 +
//   回读双倍开销。超限走「外置 + 结论头」（报告首段即结论摘要，compact_result
//   的 task 分支），细节按页 recall：内联恒小，回读只在真需要细节时发生。
// · 其他工具维持 4KB 通用闸（runner 输出等构建日志类本就该瘦着进上下文）。
inline constexpr std::size_t kContentInlineCap = 96 * 1024;
inline constexpr std::size_t kTaskInlineCap = 1024;

// 子代理类型（一个 .ithaca/agent/<名>.md；内置 general 恒在，同名 md 覆盖之）。
// 类型只管提示词与工具面——子代理用哪个模型走配置链路（当前主模型的 sub_model
// 经 vc.llm.set_active 子档案下发），类型不设 model 字段。
struct AgentType {
    std::string name;
    std::string description;         // 进 task 工具自述的类型说明
    std::vector<std::string> tools;  // 工具集白名单（去 ithaca.tool. 前缀的集合名）；空 = 继承父全量
    std::string prompt;              // 正文 = 子代理的 system prompt
};

// ---- AgentCtx：一个会话槽的全部状态与方法 ----------------------------------
// M2 对象化：把写死在插件单例里的会话状态机抽成可实例化结构。子代理（父 turn
// 内嵌套一个受限 ctx 跑循环）与多会话槽只是多实例化它；方法体只触碰自身状态
// 与 host_（activate 时灌入），天然可重入。
struct AgentCtx {
    enum class State { Idle, TurnActive, AwaitingDecision };

    // 全局唯一计数（多槽/子代理共用：turn_id 与 request_id 跨 ctx 不冲突——
    // 事件/快照分组按 id 反查的根基）。request_id=0 保留给 agent 自发收口事件。
    // NOTE: 发号与恢复抬升必须共用同一个计数器——抬到旁路计数器等于没抬，
    // 恢复会话后新 turn 仍从 1 重来，raw/ 归档按 turn 命名即跨会话互相覆盖
    // （踩过：adopt 曾抬在第二个 static 上，恢复即撞号）。
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
    // 用户可见痕迹，重启后整段交换从历史蒸发）。内核 ABI 边界已按订阅者隔离
    // 异常并落日志，本层兜底负责把异常变成可见的 turn 终态。
    void on_input(const char* text);
    void on_done(const vc_api::LlmDone& e);
    void on_llm_error(const vc_api::LlmError& e);
    void on_decision(const vc_api::TurnDecision& d);
    void on_input_impl(const char* text);
    void on_done_impl(const vc_api::LlmDone& e);
    void on_llm_error_impl(const vc_api::LlmError& e);
    void on_decision_impl(const vc_api::TurnDecision& d);

    // ---- turn 状态机 ------------------------------------------------------------
    // （方法体在 src/ctx_turn.cpp）

    void begin_turn(const std::string& text);
    void send_chat();
    void send_chat_impl();
    // 工具分发循环：跳过已完成项；确认类工具挂起等 decision；异步工具登记等归流；
    // 全部完成 ⇒ 结果按序回灌 ⇒ 下一轮 LLM。单条分发的异常由循环兜底转错误结果
    // （内核 ABI 边界已按订阅者隔离异常并落日志，但本 ctx 状态机仍会停在抛点
    // ⇒ send_chat 永不执行 ⇒ turn 悬停，见 dispatch_one_）。
    void dispatch_loop();
    // 单条调用分发（dispatch_loop 的 try 保护体）。返回 true = 挂起即返（提案
    // 等裁决 / 回灌已就地发车）——调用方必须跳过循环尾，语义对齐重构前这些
    // 路径的 return（尾部的 append_results_and_continue 在挂起期绝不能再跑）。
    bool dispatch_one_(ToolCallCtx& ctx);
    // 全部工具调用完成 ⇒ 结果按序回灌 ⇒ 下一轮 LLM。
    // 上下文压缩阶段一：超内联上限（4KB）的成功/失败结果外置到 raw/，消息里只留
    // 确定性模板摘要 + ref（锚点逐字节保留；失败 2KB 内保留详细错误，ref 兜底全量）
    void append_results_and_continue();
    void append_results_and_continue_impl();
    // 原始工具结果外置：raw/<turn>_<seq>_<tool>.json；失败 ⇒ 空 ref（调用方内联截断）
    std::string spool_raw(const std::string& tool, const std::string& result);
    // context.recall：ref 校验（纯文件名）→ 读归档 → provenance 包装
    std::string exec_recall(const json& args);
    // run.exit 到达后：所有分发完成且全部 run 归流 ⇒ 收口（正在等 decision 时不收口，
    // 收口延迟到 decision 恢复后的 dispatch_loop 的时机修正）
    void try_finish_tools();
    void try_finish_tools_impl();
    // 确认清单 = 权限模式映射：suggest 逐条问 / auto-edit 编辑免批 /
    // full-auto 全自动（沙箱兜底在 runner）
    void rebuild_confirm_();
    void finish_turn();
    // agent 内部失败的统一收口（没有工具表 / LLM 不可用 / LLM 未受理等）：发
    // llm_error——webui 由此清 in_flight + 落错误卡，transcript 有终态记录（重放
    // 安全）。request_id=0 是「agent 自发」哨兵：自己订阅的 on_llm_error 靠
    // request_id != inflight_ 忽略它，否则同步派发重入 cleanup 会把排队输入排空两次。
    void fail_turn(const std::string& why);
    // turn 机器统一异常兜底（事件处理器入口 catch 体 / 回灌与发送原语的 catch 体）：
    // 重掷当前异常取 what() 后转 fail_turn 终态（错误卡 + 状态机复位）。compress
    // 在途先清——异常语境下摘要请求不可信，悬着会让 cleanup_turn_ 早退继续悬停；
    // 迟到的摘要回包按 request_id 失配丢弃。noexcept：兜底绝不成为新故障源，
    // fail_turn 链自身再抛就强制回 Idle（host_.log 不抛，可安全留痕）。
    void fail_unexpected_() noexcept;
    // 状态机复位（不含对外事件——收口事件由调用方决定发不发）：持久化 + 回
    // Idle + 清工具现场 + 排队输入依次开新 turn（栈深有界）。
    void cleanup_turn_();
    // 清理悬挂的工具回合尾巴（cleanup_turn_ 开头调用）：异常/取消收口时
    // messages_ 可能停在分发中途——尾部是「assistant 带 tool_calls 却没有全部
    // tool 应答」，这种序列落盘后进下次请求即 provider 400 且每轮复现成错误
    // 循环。从尾部弹出未闭合的 exchange（tool 应答 → 其所属 assistant）。
    void truncate_dangling_tool_exchange_();

    // ---- 上下文压缩阶段二 ------------

    // 阈值命中后的异步摘要请求：冻结折叠区间（机械 digest 为兜底产物），发无
    // tools 的单轮 chat_ex；失败（服务缺失/未受理）就地回退机械应用。
    void request_digest_();
    // 折叠应用（LLM 摘要或机械 digest 共用出口）：区间替换 + 旧 digest 并入 +
    // 持久化 + 通告 + 回 Idle + flush 排队输入；置 compress_verify_ 退避校验。
    void apply_digest_(const json& digest_msg, bool via_llm);
    // 摘要回包处理（on_done/on_llm_error 按 compress_seq_ 分流到这）：校验/补
    // ref/截断后应用；摘要失败一律回退 pending_plan_.digest。
    void on_compress_done_(const vc_api::LlmDone& e);
    void on_compress_error_(const vc_api::LlmError& e);
    // 生效阈值重算：W 已知 ⇒ 按输出预留重算；未知 ⇒ 兜底阈值（compress_threshold_）。
    void set_context_window_(long long w);
    // 生效阈值（批次四-19）：W − max_output（输出预留），下限 W/4；仅 W ⇒ 0.9×W；
    // 都未知 ⇒ 兜底阈值。
    void recompute_threshold_();

    // ---- 上下文压缩批次一 ---------------------------

    // 硬保护判定：last_prompt_tokens_ ≥ 0.95×W（*20/19 整数式，宁早勿晚）。独立于
    // 会话阈值——退避封顶或阈值失灵时仍兜底；命中强制机械折叠（不等摘要）。
    bool hard_window_() const;
    // 压缩判定共用入口（finish_turn 与 cleanup_turn_——失败/取消收口同样过判定，
    // 否则反复取消/失败的长会话上下文只增不减）：硬保护或会话阈值命中 ⇒ 产出
    // 折叠计划——非硬保护且摘要开 → 异步 LLM 摘要（request_digest_，收口在摘要
    // 回包/失败分流）；否则机械折叠（apply_digest_ 内收口）。计划不可折叠（单段
    // 且轮数不足）⇒ 返回 false，调用方走原收口路径。
    bool maybe_compress_();
    // 折叠应用核心（LLM 摘要 / 机械 digest / 超窗强制三路共用）：区间替换 + 旧
    // digest 并入 + 持久化 + 通告。先插 digest 再跳过的顺序纪律见阶段一。
    void replace_with_digest_(const json& digest_msg, const char* via);
    // 超窗就地自救：应用 pending_plan_ 冻结的机械折叠后重发当前请求
    // ——turn 保持在途：不置 compress_verify_、不动排队、不回 Idle。
    void forced_fold_resend_();
    // 排队输入 flush：队首开新 turn（栈深有界——一次一条，其余等该 turn 收口再取）。
    void flush_queued_inputs_();

    // 本槽事件订阅（begin_turn 开头调用，slot 路由约定见 contract/vc_events.hpp）：
    // 归属本槽的 run/LLM 事件单播直达本 ctx——替代迁移前的总线广播 + request_id
    // 反查扇出。主会话额外把本槽 run 事件重播到广播通道（webui/selftest 的 UI
    // 消费路径不变）；子会话槽事件不重播——不可见性由"订阅才可达"保证。
    void subscribe_turn_slot_();

    // ---- 子代理派发（spawn/回灌在 ctx_turn.cpp；类型加载在 agent_types.cpp）------

    // task 内置工具：校验参数与护栏 → 起子 ctx 事件驱动跑循环 → 登记在途。
    // 返回 false = 拒绝（result 已写好，调用方 emit 后继续分发）；true = 已 spawn
    // 或已在同帧内经回调就地收口（begin_turn 同步失败的路径）。
    bool spawn_child_(std::size_t index, ToolCallCtx& call);
    // 子代理完成回调（宿主线程，可能在子事件处理栈内同步派发）：用量并入本会话、
    // 结果写回 calls_[index]、统一收口判定。收口门槛 = 全部 call 完成 + 无在途子
    // + 无在途 run——与 try_finish_tools 同一门。
    void on_child_finished_(std::size_t index, AgentCtx* child, bool ok,
                            const std::string& report, int turns);
    // 类型清单重载（主会话每 turn 开始）：目录快照扫描 + 内置 general 兜底。
    void load_agent_types_();

    // ---- 会话持久化与 mission 卡（src/ctx_session.cpp）--------------------------

    // M6 session 持久化：历史 + turn 计数 + 用量落盘（失败仅告警，不影响任务）
    // 注意：撤销快照栈是内存态，恢复后撤销栈从零开始（文档明示）。
    // state 文件加载（activate 与会话切换共用；缺文件 = 全新会话）
    void load_session_state_(const std::string& path);
    void persist_session_();
    // mission 任务目标卡重建（turn 边界 = 重建点）：已存在则原位重写内容，否则
    // 插到 system 之后（下标 1）。turn 内不改动——固定前缀纪律
    void rebuild_mission_();

    // ---- 工具桥（src/ctx_tools.cpp）-----------------------------------------------

    struct Bound {
        std::uint64_t svc = 0;
        std::string svc_name;
        const vc_api::ToolTable* table = nullptr;
    };

    bool discover_tools();
    const Bound* find_bound(const std::string& full_name) const;
    static std::string tool_last(const std::string& full_name);
    std::string exec(const Bound& b, const std::string& tool, const json& args);
    void exec_undo();
    void emit_tool_call(const std::string& name, const json& args);
    void emit_tool_result(const std::string& name, bool ok, const std::string& brief);
    // 子代理活动广播（vc.subagent.event，主会话工作记录视图——落 transcript 不进
    // 上下文）：spawn/delta/tool/收口各点共用此出口。turn_id 显式传入——UI 按
    // turn_id 把同一子代理的记录分组（spawn 卡点开右栏即按它过滤），必须与子
    // ctx 自身发出的记录同号：spawn 发生在父 ctx，须传子的 turn_no 而非父的
    void emit_subagent_(int turn_id, int phase, const char* tool, const std::string& text);
    void notice(const std::string& text, int sys = 0);
    // 带机器路由元数据的通告（vc_api::Notice 尾部追加字段）：kind 标类别，前端
    // 状态机按它路由（text 仅供人读）；run_id/count 按 kind 语义填充
    void notice_meta(const std::string& text, std::string_view kind,
                     long long run_id = 0, int count = 0);
    // 收口门槛：全部后台 run 已归流（exit 事件到达）——dispatch_loop 尾部、
    // 子代理收口、try_finish_tools_impl 三处共用一份口径
    bool all_runs_exited_() const;
    // 异步长任务登记：run_id → 分发序号（结果等 vc.run.exit 归流后按序回卡）
    void register_run(int run_id, int index);
    static std::string trim(const std::string& s, std::size_t cap);
    // apply 参数 → op 清单："试算 write a.txt；replace b.txt ×2"（解析不出 ⇒ 空，回退）
    // 调用参数 → 人话摘要（解析不出 ⇒ 空，回退原 JSON 截断）
    static std::string summarize_call(const std::string& tool, const json& args);
    static std::string summarize_apply_ops(const json& args);
    // diff 统计尾行 "（+A −R 行）" → "+A −R"（"（"=3 字节；无 ⇒ 空）
    static std::string diff_stats_short(const std::string& d);
    // 工具结果 → 人话摘要；失败点名首条错误；认不出的形态回退原 JSON 截断
    static std::string summarize_result(const std::string& tool, const std::string& result);
    static int parse_run_id(const std::string& result);
    static bool tool_ok(const std::string& result);
    AsyncRun* find_run(int run_id);

    // ---- 状态 -----------------------------------------------------------------------

    nostos::abi::Host host_{};

    const vc_api::LlmTable* llm_ = nullptr;  // 每次 send_chat 前解析（provider 可被竞争顶掉）
    const vc_api::LlmTable* window_probed_llm_ = nullptr;  // 已向 info() 探过窗口的 provider 实例
    long long ctx_max_output_ = 0;      // provider 自述的输出上限（info()；消费方 = 发送前输出预留）

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
    std::deque<std::string> queued_;

    // 等待批准的上下文
    struct Pending {
        std::string tool;
        std::size_t index = 0;
        json args;
    } pending_;

    // ---- 子代理（M2 预留的多实例化：状态全部随 ctx 走，方法只触自身与 host_）----

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
    // 本 turn 的槽订阅（run_line/run_exit；子会话另加 llm_done/llm_error）——
    // begin_turn 换订（上 turn 槽已无事件源），ctx 析构随 RAII 释放
    std::vector<nostos::abi::Host::Subscription> slot_subs_;
};  // AgentCtx

// AgentPlugin：薄壳——订阅分发到 ctx_ 与子代理扇出（M2 对象化后 v1 单主槽；
// 多会话槽只是 AgentPlugin 的多实例）
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
    AgentCtx ctx_;   // v1 单槽；多槽 = std::map<slot, AgentCtx>（方法已全部随 ctx 走）

    nostos::abi::Host::Subscription sub_input_{};
    nostos::abi::Host::Subscription sub_decision_{};
    nostos::abi::Host::Subscription sub_done_{};    // 主会话槽（广播）；子会话走各自槽订阅
    nostos::abi::Host::Subscription sub_error_{};   // 同上
    nostos::abi::Host::Subscription sub_session_{};
    nostos::abi::Host::Subscription sub_mode_{};
    nostos::abi::Host::Subscription sub_llm_set_{};     // 模型窗口事件（阶段二动态阈值）
    nostos::abi::Host::Subscription sub_workspace_{};   // 工作区热切（子代理类型目录跟随）
};

}  // namespace vc_agent
