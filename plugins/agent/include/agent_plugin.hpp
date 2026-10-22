// plugin/agent —— turn 状态机 + 泛型工具桥 + 批准流 + 异步长任务归流。
//
// 翻译单元职责地图（按功能切片，AgentCtx/AgentPlugin 声明为本文件）：
//   src/plugin.cpp       —— AgentPlugin 薄壳（订阅分发到 ctx_）、manifest + NOSTOS_PLUGIN 入口
//   src/ctx_events.cpp   —— 事件处理器（输入/done/错误/裁决）
//   src/ctx_turn.cpp     —— turn 状态机（分发循环/收口/上下文压缩执行）
//   src/ctx_session.cpp  —— 会话持久化（state.json 读写）+ mission 任务目标卡
//   src/ctx_tools.cpp    —— 工具桥（discover/exec/undo/事件回发）+ 工具调用/结果人话摘要
//
// **死锁纪律（本插件的最高规则）**：事件处理器内禁止同步等待任何事件——提案后
// 即返回，decision 事件驱动状态机恢复；turn 进行中的输入入队。单线程事件环里
// 一处同步等待 = 整个宿主死锁回归用例专门覆盖）。
//
// 职责边界：编排/上下文/审批策略执行在本插件；不碰文件、不起进程、
// 不解析工具语义（工具自述经 list() 反射透传给 LLM）。
//
// 工具发现：env ITHACA_AGENT_TOOLS（svc 名清单，默认
// "vc.tool.workspace,vc.tool.runner"），每次 turn 开始按清单取表。
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
#include <map>
#include <nlohmann/json.hpp>
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

    void on_input(const char* text);
    void on_done(const vc_api::LlmDone& e);
    void on_llm_error(const vc_api::LlmError& e);
    void on_decision(const vc_api::TurnDecision& d);

    // ---- turn 状态机 ------------------------------------------------------------
    // （方法体在 src/ctx_turn.cpp）

    void begin_turn(const std::string& text);
    void send_chat();
    // 工具分发循环：跳过已完成项；确认类工具挂起等 decision；异步工具登记等归流；
    // 全部完成 ⇒ 结果按序回灌 ⇒ 下一轮 LLM。
    void dispatch_loop();
    // 全部工具调用完成 ⇒ 结果按序回灌 ⇒ 下一轮 LLM。
    // 上下文压缩阶段一：超内联上限（4KB）的成功/失败结果外置到 raw/，消息里只留
    // 确定性模板摘要 + ref（锚点逐字节保留；失败 2KB 内保留详细错误，ref 兜底全量）
    void append_results_and_continue();
    // 原始工具结果外置：raw/<turn>_<seq>_<tool>.json；失败 ⇒ 空 ref（调用方内联截断）
    std::string spool_raw(const std::string& tool, const std::string& result);
    // context.recall：ref 校验（纯文件名）→ 读归档 → provenance 包装
    std::string exec_recall(const json& args);
    // run.exit 到达后：所有分发完成且全部 run 归流 ⇒ 收口（正在等 decision 时不收口，
    // 收口延迟到 decision 恢复后的 dispatch_loop 的时机修正）
    void try_finish_tools();
    // 确认清单 = 权限模式映射：suggest 逐条问 / auto-edit 编辑免批 /
    // full-auto 全自动（沙箱兜底在 runner）
    void rebuild_confirm_();
    void finish_turn();
    // agent 内部失败的统一收口（没有工具表 / LLM 不可用 / LLM 未受理等）：发
    // llm_error——webui 由此清 in_flight + 落错误卡，transcript 有终态记录（重放
    // 安全）。request_id=0 是「agent 自发」哨兵：自己订阅的 on_llm_error 靠
    // request_id != inflight_ 忽略它，否则同步派发重入 cleanup 会把排队输入排空两次。
    void fail_turn(const std::string& why);
    // 状态机复位（不含对外事件——收口事件由调用方决定发不发）：持久化 + 回
    // Idle + 清工具现场 + 排队输入依次开新 turn（栈深有界）。
    void cleanup_turn_();

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
    void tool_result_msg(const std::string& call_id, const std::string& out);
    void emit_tool_call(const std::string& name, const json& args);
    void emit_tool_result(const std::string& name, bool ok, const std::string& brief);
    void notice(const std::string& text, int sys = 0);
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
    nostos::abi::Host::Subscription sub_input_{};
    nostos::abi::Host::Subscription sub_decision_{};
    nostos::abi::Host::Subscription sub_done_{};
    nostos::abi::Host::Subscription sub_error_{};
    nostos::abi::Host::Subscription sub_run_line_{};
    nostos::abi::Host::Subscription sub_run_exit_{};
    nostos::abi::Host::Subscription sub_session_{};
    nostos::abi::Host::Subscription sub_mode_{};

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
    std::string raw_dir_;  // 外置存储目录（会话文件旁 raw/；空 = 未启用）
    int raw_seq_ = 0;

    // 当前 assistant 消息的工具调用记账
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
};  // AgentCtx

// AgentPlugin：薄壳——订阅分发到 ctx_（M2 对象化后 v1 单槽；多会话槽/子代理
// 只是 AgentCtx 的多实例）
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
    nostos::abi::Host::Subscription sub_done_{};
    nostos::abi::Host::Subscription sub_error_{};
    nostos::abi::Host::Subscription sub_run_line_{};
    nostos::abi::Host::Subscription sub_run_exit_{};
    nostos::abi::Host::Subscription sub_session_{};
    nostos::abi::Host::Subscription sub_mode_{};
    nostos::abi::Host::Subscription sub_llm_set_{};   // 模型窗口事件（阶段二动态阈值）
};

}  // namespace vc_agent
