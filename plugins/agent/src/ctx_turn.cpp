// turn 状态机（plugin/agent 的核心切片）。
// begin_turn 开轮 → dispatch_loop 分发工具（确认类挂起提案/异步登记归流/同步直执）
// → append_results_and_continue 按序回灌并进下一轮 → finish_turn 收口（迟滞执行
// 上下文压缩）。失败/取消的统一收口 fail_turn/cleanup_turn_ 也在本文件。

#include "agent_plugin.hpp"

#include <algorithm>  // std::clamp：recall 分页参数收敛
#include <fstream>    // std::ofstream：raw 外置归档

namespace vc_agent {

namespace {

constexpr char kSystemPrompt[] =
    "你是 Ithaca——一个运行在终端里的编程代理。用户用自然语言下达任务，"
    "你通过 tools 列出的工具读写工作区文件、执行构建。规则："
    "1) 编辑直接用 apply 提交（write=整文件 / replace=精确块，find 默认必须恰好"
    "命中一次，replace_all=true 全部替换）；系统会校验、落盘并把 diff 展示给用户，"
    "用户可随时撤销；"
    "2) 需要批准的操作（如运行命令）由宿主把关，被拒绝时把拒绝当作工具结果继续任务；"
    "3) 命令类工具是异步的：提交后先返回 run_id，构建/测试的输出与退出码随后"
    "作为工具结果回给你，读错误输出自我修复；"
    "4) 工具失败时读错误信息自我纠正；"
    "5) Python 依赖一律装进工作区内的虚拟环境（python -m venv .venv，之后用"
    " .venv 里的 python/-m pip 安装与运行）——完全访问模式下工作区外不可写，"
    "装到用户级目录会失败或不可见；"
    "6) 用中文简明汇报。";

}  // namespace

// ---- turn 状态机 ------------------------------------------------------------

void AgentCtx::begin_turn(const std::string& text) {
    turn_no_ = next_turn_no();
    state_ = State::TurnActive;
    runs_.clear();
    compress_retries_ = 0;
    if (!discover_tools())
        return fail_turn("没有可用的工具表（检查 ITHACA_AGENT_TOOLS）");
    // M6 session：恢复的会话保留历史（含 system），只在空会话时注入 system
    if (messages_.empty())
        messages_.push_back(json{{"role", "system"}, {"content", kSystemPrompt}});
    messages_.push_back(json{{"role", "user"}, {"content", text}});
    // mission 任务目标卡（上下文压缩阶段一）：turn 边界 = 重建点，整卡重写并
    // 固定在 system 之后——turn 内 append-only（缓存前缀纪律）
    rebuild_mission_();
    send_chat();
}

void AgentCtx::send_chat() {
    // provider 每轮重新解析：同名服务被竞争顶掉时自然切换，agent 无感
    llm_ = host_.service<vc_api::LlmTable>(vc_api::llm);
    // 窗口/输出上限自述兜底：env / set_active 没给全（窗口未知或 max_output 未知）
    // ⇒ 向 provider info() 探一次（每个 provider 实例至多一次）。优先级：
    // env > set_active > info（env 显式覆盖最高；info 只补缺口）。
    if ((!ctx_window_env_ && ctx_window_ == 0 || ctx_max_output_ == 0) &&
        llm_ != nullptr && llm_->info != nullptr && llm_ != window_probed_llm_) {
        window_probed_llm_ = llm_;
        const char* info_json = nullptr;
        if (llm_->info(llm_->self, &info_json) == NOSTOS_OK && info_json != nullptr) {
            const json info = json::parse(info_json, nullptr, false);
            if (info.is_object()) {
                if (ctx_window_ == 0 && !ctx_window_env_) {
                    if (const long long w = info.value("context_window", 0LL); w > 0)
                        ctx_window_ = w;
                }
                if (ctx_max_output_ == 0)
                    ctx_max_output_ = info.value("max_output_tokens", 0LL);
                recompute_threshold_();   // 元数据到位 ⇒ 阈值按输出预留重算
            }
        }
    }
    inflight_ = next_chat_seq();
    json body = {{"messages", messages_}};
    if (!tools_.empty()) body["tools"] = tools_;
    body_buf_ = body.dump();
    if (llm_ == nullptr || llm_->chat_ex == nullptr)
        return fail_turn("LLM 不可用");
    // 发送前预检（批次四-18/19）：请求体粗估超生效阈值 ⇒ 就地机械折叠后重建
    // 请求体再发——单 turn 几十轮工具往返在轮间主动瘦身（每轮 send_chat 都检），
    // 省一次 provider 400 往返；LLM 摘要仍只在 turn 边界。粗估宁高勿低：误折
    // 代价 = 上下文略瘦，可接受；折叠后旧信号置零待下次回报。
    if (session_threshold_ > 0 &&
        estimate_prompt_tokens(body_buf_) > session_threshold_) {
        if (const vcagent::CompressPlan plan = vcagent::plan_compress(messages_); plan.valid) {
            pending_plan_ = plan;
            replace_with_digest_(plan.digest, "发送前预检");
            pending_plan_ = {};
            last_prompt_tokens_ = 0;   // 旧信号已失效（上下文已折叠），待下次回报
            json nb = {{"messages", messages_}};
            if (!tools_.empty()) nb["tools"] = tools_;
            body_buf_ = nb.dump();
        }
    }
    if (const nostos_status st = llm_->chat_ex(llm_->self, inflight_, body_buf_.c_str());
        st != NOSTOS_OK)
        return fail_turn("LLM 未受理（status=" + std::to_string(static_cast<int>(st)) + "）");
}

// 工具分发循环：跳过已完成项；确认类工具挂起等 decision；异步工具登记等归流；
// 全部完成 ⇒ 结果按序回灌 ⇒ 下一轮 LLM。
void AgentCtx::dispatch_loop() {
    while (dispatched_ < calls_.size()) {
        ToolCallCtx& ctx = calls_[dispatched_];
        if (ctx.done) {
            ++dispatched_;
            continue;
        }
        emit_tool_call(ctx.full_name, ctx.args);
        // 内置工具：context.recall（外置存储回查）——不经工具表，直接处理
        if (ctx.full_name == vcagent::kRecallTool) {
            ctx.result = exec_recall(ctx.args);
            ctx.done = true;
            emit_tool_result(ctx.full_name, true, ctx.result);
            ++dispatched_;
            continue;
        }
        const Bound* b = find_bound(ctx.full_name);
        if (b == nullptr) {
            ctx.result = json{{"ok", false}, {"error", "未知工具: " + ctx.full_name}}.dump();
            ctx.done = true;
            emit_tool_result(ctx.full_name, false, ctx.result);
            ++dispatched_;
            continue;
        }
        if (vcagent::needs_confirm(ctx.full_name, confirm_) && !auto_all_) {
            // 泛化试算 + 提案（先置状态再 emit——嵌套派发会在本帧内回到
            // on_decision，状态没就位会被自己的守卫丢弃——M3 踩过）。
            json dry_args = ctx.args;
            dry_args["dry_run"] = true;
            const std::string dry_out = exec(*b, ctx.tool, dry_args);
            pending_.tool = ctx.full_name;
            pending_.index = dispatched_;
            pending_.args = ctx.args;
            state_ = State::AwaitingDecision;
            // diff_text 只装行级 diff（原始结果 JSON 不可读——webui 截图自审抓到）；
            // 同文件多 op 已在 workspace 折叠，edits>1 时标注「N 处编辑」
            std::string diff_text;
            json dry = json::parse(dry_out, nullptr, false);
            if (dry.is_object() && dry.contains("files") && dry["files"].is_array())
                for (const auto& f : dry["files"]) {
                    if (f.contains("path")) {
                        std::string head =
                            "── " + f.value("path", std::string());
                        if (f.contains("edits") && f["edits"].is_number_integer() &&
                            f["edits"].get<int>() > 1)
                            head += "（" + std::to_string(f["edits"].get<int>()) + " 处编辑）";
                        diff_text += head + "\n";
                    }
                    if (f.contains("diff") && f["diff"].is_string())
                        diff_text += f["diff"].get<std::string>();
                }
            // dry 失败（ok:false）不进提案：错误经 tool_result 回灌 LLM 自修
            if (dry.is_object() && dry.value("ok", true) == false) {
                ctx.result = dry_out;
                ctx.done = true;
                emit_tool_result(ctx.full_name, false, ctx.result);
                append_results_and_continue();
                return;
            }
            // runner 类提案：would_run 单行卡（命令批准卡形态）
            if (diff_text.empty() && dry.is_object() && dry.contains("would_run") &&
                dry["would_run"].is_string())
                diff_text = "── " + ctx.full_name + "（命令批准）\n" +
                            dry["would_run"].get<std::string>() + "\n";
            if (diff_text.empty()) diff_text = dry_out;
            nostos::kit::Scratch prop_scratch;
            const vc_api::EditProposed proposal{
                turn_no_, diff_text.c_str(), prop_scratch.hold(ctx.full_name)};
            host_.emit(vc_api::edit_proposed, &proposal);
            return;  // ← 死锁纪律：绝不同步等待
        }
        ctx.result = exec(*b, ctx.tool, ctx.args);
        ctx.done = true;
        emit_tool_result(ctx.full_name, tool_ok(ctx.result), ctx.result);
        ++dispatched_;
        // 异步长任务（runner.run）：登记 run_id——结果等 vc.run.exit 归流，
        // 本条消息的其余工具照常分发（runner 侧并发、run_id 归流）。
        if (const int rid = parse_run_id(ctx.result); rid > 0) {
            runs_[rid] = AsyncRun{dispatched_ - 1, "", false, 0};
            notice("后台任务 run#" + std::to_string(rid) + " 已启动");
        }
    }
    // 分发完但仍未收口 ⇒ 看异步任务：全部已退出（含 run_exit 在审批窗口内
    // 提前到达的场景 时机修正）就地收口；否则等归流
    if (runs_.empty()) return append_results_and_continue();
    bool all_exited = true;
    for (const auto& [rid, run] : runs_)
        if (!run.exited) all_exited = false;
    if (all_exited) return try_finish_tools();
    notice("等待 " + std::to_string(runs_.size()) + " 个后台任务完成…");
}

// 全部工具调用完成 ⇒ 结果按序回灌 ⇒ 下一轮 LLM。
// 上下文压缩阶段一：超内联上限（4KB）的成功/失败结果外置到 raw/，消息里只留
// 确定性模板摘要 + ref（锚点逐字节保留；失败 2KB 内保留详细错误，ref 兜底全量）
void AgentCtx::append_results_and_continue() {
    for (const auto& ctx : calls_) {
        std::string content = ctx.result;
        if (content.size() > vcagent::kInlineResultCap) {
            if (tool_last(ctx.full_name) == "recall") {
                // recall 的 content 本身就是外置归档：直接截断兜底即可
                content = vcws::trim_utf8(content, 2048) + "…（回查内容过长，已截断）";
            } else {
                const std::string ref = spool_raw(ctx.full_name, content);
                if (!ref.empty()) {
                    const json r = json::parse(content, nullptr, false);
                    content = vcagent::compact_result(ctx.full_name, r, ref);
                } else {
                    content = vcws::trim_utf8(content, 2048) +
                              "…（超出内联上限且外置失败）";
                }
            }
        }
        messages_.push_back(json{{"role", "tool"},
                                 {"tool_call_id", ctx.call_id},
                                 {"content", content}});
    }
    send_chat();
}

// 原始工具结果外置：raw/<turn>_<seq>_<tool>.json；失败 ⇒ 空 ref（调用方内联截断）
std::string AgentCtx::spool_raw(const std::string& tool, const std::string& result) {
    if (raw_dir_.empty()) return {};
    try {
        const std::string name = "t" + std::to_string(turn_no_) + "_" +
                                 std::to_string(++raw_seq_) + "_" +
                                 tool_last(tool) + ".json";
        std::ofstream f(fs::path(raw_dir_) / name, std::ios::binary | std::ios::trunc);
        if (!f) return {};
        f << result;
        return name;
    } catch (const std::exception&) {
        return {};
    }
}

// context.recall：ref 校验（纯文件名）→ 读归档 → provenance 包装；分页（批次三-8）：
// offset/limit 字节窗口（limit 缺省 2048、上限 4096 = 单条内联护栏），回包带
// total/next_offset/has_more——大归档分段读，不再"永远只看头 2KB"
std::string AgentCtx::exec_recall(const json& args) {
    const std::string ref = args.value("ref", "");
    if (ref.empty() || raw_dir_.empty())
        return json{{"ok", false},
                    {"error", "ref 为空或本会话未启用外置存储（无 ITHACA_SESSION_FILE）"}}
            .dump();
    if (ref.find('/') != std::string::npos || ref.find('\\') != std::string::npos ||
        ref.find("..") != std::string::npos)
        return json{{"ok", false}, {"error", "ref 必须是裸文件名"}}.dump();
    std::ifstream in(fs::path(raw_dir_) / ref, std::ios::binary);
    if (!in) return json{{"ok", false}, {"error", "归档不存在: " + ref}}.dump();
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    const long long total = static_cast<long long>(content.size());
    long long offset = args.value("offset", 0LL);
    const long long limit = std::clamp(args.value("limit", 2048LL), 1LL, 4096LL);
    offset = std::clamp(offset, 0LL, total);
    // 页边界不劈 UTF-8 码点：起点落在续字节上则后移到码点首字节
    while (offset < total && (static_cast<unsigned char>(
                                  content[static_cast<std::size_t>(offset)]) & 0xC0) == 0x80)
        ++offset;
    std::string page = content.substr(static_cast<std::size_t>(offset),
                                      static_cast<std::size_t>(limit));
    page = vcws::trim_utf8(page, static_cast<std::size_t>(limit));
    const bool has_more = offset + static_cast<long long>(page.size()) < total;
    return vcagent::recall_result(ref, page, total, offset, has_more,
                                  offset + static_cast<long long>(page.size()));
}

// run.exit 到达后：所有分发完成且全部 run 归流 ⇒ 收口（正在等 decision 时不收口，
// 收口延迟到 decision 恢复后的 dispatch_loop 的时机修正）
void AgentCtx::try_finish_tools() {
    if (state_ != State::TurnActive) return;
    if (dispatched_ != calls_.size()) return;
    if (runs_.empty()) return;  // 无异步任务：常规收口归 dispatch_loop，防双重 send_chat
    for (const auto& [rid, run] : runs_)
        if (!run.exited) return;
    for (const auto& [rid, run] : runs_) {
        ToolCallCtx& ctx = calls_[run.index];
        ctx.result = json{{"run_id", rid},
                          {"exit_code", run.exit_code},
                          {"output", run.output}}
                         .dump();
        ctx.done = true;
        emit_tool_result(ctx.full_name, run.exit_code == 0, ctx.result);
    }
    runs_.clear();
    // 工具消息必须按序回灌后再进下一轮——直接 send_chat 会留下"assistant 带
    // tool_calls 但无 tool 应答"的非法序列，真实 API 400
    append_results_and_continue();
}

// 确认清单 = 权限模式映射：suggest 逐条问 / auto-edit 编辑免批 /
// full-auto 全自动（沙箱兜底在 runner）
void AgentCtx::rebuild_confirm_() {
    if (mode_ == "suggest") confirm_ = {"workspace.apply", "runner.run"};
    else if (mode_ == "full-auto") confirm_.clear();
    else confirm_ = {"runner.run"};
}

void AgentCtx::finish_turn() {
    // turn 完成只发 host log，不发 notice——webui 对话流不需要；token 统计
    // 已随 assistant 记录回传
    host_.log(2, "ithaca.agent: turn #" + std::to_string(turn_no_) + " 完成（tokens total " +
                     std::to_string(usage_total_) + "）");
    persist_session_();
    // 上下文压缩（阶段二两态）：判定实时——硬保护 0.95×W 独立前置，
    // 或会话阈值（迟滞）；执行只在 turn 边界。折叠两态：LLM 自总结（异步摘要请求）
    // → 机械 digest 兜底；硬保护强制机械折叠（摘要输出量不可控，也不等它）。
    if (maybe_compress_()) return;  // 已收口（机械）或摘要在途（收口在回包/失败分流）
    state_ = State::Idle;
    flush_queued_inputs_();
}

// ---- 上下文压缩阶段二 --------------

void AgentCtx::set_context_window_(long long w) {
    ctx_window_ = w;
    recompute_threshold_();   // 模型/供应商切换重置退避（窗口变了旧基数失效）
}

// 生效阈值（批次四-19）：W 与 max_output 已知 ⇒ W − max_output（输出预留——给
// 长 diff/长测试输出留位，避免贴着 0.9W 的请求被大响应顶爆）；仅 W 已知 ⇒
// 0.9×W（历史语义）；都未知 ⇒ 兜底阈值。下限 W/4：max_output 元数据异常巨大时
// 仍保住 25% 的 prompt 空间。
void AgentCtx::recompute_threshold_() {
    if (ctx_window_ <= 0) {
        session_threshold_ = compress_threshold_;
        return;
    }
    const long long reserved =
        ctx_max_output_ > 0 ? ctx_max_output_ : ctx_window_ / 10;
    const long long th = ctx_window_ - reserved;
    const long long floor_th = ctx_window_ / 4;
    session_threshold_ = th > floor_th ? th : floor_th;
}

// ---- 上下文压缩批次一 --------------------------------

// 硬保护线：0.95×W，*20/19 整数式避免除法截断（宁早勿晚）。W 未知 ⇒ 不启用
// ——判定交给会话阈值/兜底阈值路径。
bool AgentCtx::hard_window_() const {
    return ctx_window_ > 0 && last_prompt_tokens_ * 20 >= ctx_window_ * 19;
}

// 压缩判定共用入口：finish_turn 与 cleanup_turn_ 都走这里（失败/取消收口不漏
// 判定——否则反复取消/失败的长会话上下文只增不减）。计划不可折叠 ⇒ false，
// 调用方走原收口；若下一请求仍超窗，由 on_llm_error 的超窗自救接手。
bool AgentCtx::maybe_compress_() {
    const bool hard = hard_window_();
    if (!hard && !vcagent::should_compress(last_prompt_tokens_, session_threshold_))
        return false;
    const vcagent::CompressPlan plan = vcagent::plan_compress(messages_);
    if (!plan.valid) return false;
    if (!hard && summary_enabled_) {
        pending_plan_ = plan;
        request_digest_();  // 异步收口：摘要回包/失败在 on_done/on_llm_error 分流
        return true;
    }
    apply_digest_(plan.digest, false);  // apply_digest_ 内收口（回 Idle + flush 排队）
    return true;
}

// 摘要请求：无 tools 的单轮 chat_ex；素材 = 机械 digest 渲染（request 逐字 +
// calls 最小记录 + 全部 ref）——与兜底产物同源，摘要失败可直接落它。
void AgentCtx::request_digest_() {
    llm_ = host_.service<vc_api::LlmTable>(vc_api::llm);
    if (llm_ == nullptr || llm_->chat_ex == nullptr)
        return apply_digest_(pending_plan_.digest, false);
    compress_seq_ = next_chat_seq();
    compress_inflight_ = true;
    json body = {{"messages", json::array({
                                  json{{"role", "system"},
                                       {"content", std::string(vcagent::kSummarySystem)}},
                                  json{{"role", "user"},
                                       {"content", pending_plan_.digest.value("content", "")}}})}};
    body_buf_ = body.dump();   // 调用点均在无在途请求时（turn 收口后/请求已失败或
                               // 取消）——复用缓冲安全
    if (llm_->chat_ex(llm_->self, compress_seq_, body_buf_.c_str()) != NOSTOS_OK)
        apply_digest_(pending_plan_.digest, false);   // 未受理 ⇒ 机械兜底
}

void AgentCtx::on_compress_done_(const vc_api::LlmDone& e) {
    json a = json::parse(e.assistant_json != nullptr ? e.assistant_json : "{}", nullptr,
                         /*allow_exceptions=*/false);
    std::string s = a.is_object() && a.contains("content") && a["content"].is_string()
                        ? a["content"].get<std::string>() : std::string();
    if (s.empty()) return apply_digest_(pending_plan_.digest, false);
    // 超长截断（伤散文不伤回查链——外置索引由本函数在截断之后确定性重建）
    if (s.size() > 4096) s = vcws::trim_utf8(s, 4096) + "…";
    json digest = json{
        {"role", "system"},
        {"content", std::string(vcagent::kDigestMarker) +
                        "（历史 turn 已压缩：LLM 自总结。" +
                        std::string(vcagent::kDigestProvenance) +
                        "工具原始结果外置于 raw/，可用 context.recall 回查）\n" + s}};
    // 外置索引确定性重建（批次三-11）：ref 清单来自折叠计划的结构化记录（calls
    // 提取，非摘要散文、非文本扫描）——摘要怎么改写都不断回查链；旧「补齐 tail」
    // 机制退役（其 ref 对下轮扫描不可见的失真随索引方案消失）
    const std::string index = vcagent::index_section(pending_plan_.refs);
    if (!index.empty())
        digest["content"] = digest["content"].get<std::string>() + "\n" + index;
    apply_digest_(digest, true);
}

void AgentCtx::on_compress_error_(const vc_api::LlmError&) {
    apply_digest_(pending_plan_.digest, false);   // 摘要失败 ⇒ 机械兜底（计划里已备好）
}

// 折叠应用核心：区间替换 + 旧 digest 并入 + 持久化 + 通告。先插 digest 再跳过
// 的顺序纪律见阶段一（踩过：跳过分支先 continue 会漏掉插入，digest 整条丢失）。
void AgentCtx::replace_with_digest_(const json& digest_msg, const char* via) {
    // 折叠前原文归档（批次三-17）：cut 区间的完整消息（user/assistant 全文）写入
    // raw/——agent 视图丢掉的每一行都可 recall 回查；归档 ref 附进 digest 正文，
    // 模型在压缩后仍保有"被折叠历史"的入口
    json cut = json::array();
    for (std::size_t k = pending_plan_.cut_begin;
         k < pending_plan_.cut_end && k < messages_.size(); ++k)
        cut.push_back(messages_[k]);
    const std::string archive_ref = spool_raw("digest", cut.dump());
    json digest = digest_msg;
    if (!archive_ref.empty())
        digest["content"] = digest_msg.value("content", "") + "【折叠原文】" +
                            archive_ref + "（context.recall 可回查）\n";
    // 锚点保真在线观测（批次五-20）：折叠区逐字锚点在折叠产物中的覆盖率——
    // 32KB 降级会丢最旧段的调用链锚点，比率落 host log 供评测采集（批次五-12）
    if (pending_plan_.anchors_total > 0)
        host_.log(2, "ithaca.agent: 折叠锚点保真 " +
                         std::to_string(pending_plan_.anchors_total -
                                        pending_plan_.anchors_dropped) +
                         "/" + std::to_string(pending_plan_.anchors_total));
    const std::size_t before = messages_.size();
    json nm = json::array();
    bool digest_placed = false;
    for (std::size_t k = 0; k < messages_.size(); ++k) {
        // 先插 digest（到末段起点处——紧贴 mission 之后），再做跳过判断：
        // 跳过分支先 continue 会漏掉插入（踩过：digest 整条丢失）
        if (!digest_placed && k == pending_plan_.cut_begin) {
            nm.push_back(digest);
            digest_placed = true;
        }
        if (k >= pending_plan_.cut_begin && k < pending_plan_.cut_end) continue;
        if (k < pending_plan_.cut_begin && k > 0 && vcagent::is_digest(messages_[k]))
            continue;  // 旧 digest 已并入新 digest
        nm.push_back(messages_[k]);
    }
    if (!digest_placed) nm.push_back(digest);
    messages_ = nm;
    notice(std::string("上下文已压缩（") + via + "折叠：" +
           std::to_string(pending_plan_.segments_folded) + " 段历史，消息 " +
           std::to_string(before) + " → " + std::to_string(messages_.size()) +
           "；原始结果在 raw/ 可 recall）");
    persist_session_();
}

// 折叠应用共用出口（LLM 摘要 / 机械 digest）：核心替换 + 置退避校验 + 回 Idle
// + flush 排队输入。
void AgentCtx::apply_digest_(const json& digest_msg, bool via_llm) {
    replace_with_digest_(digest_msg, via_llm ? "LLM 自总结" : "机械");
    compress_inflight_ = false;
    compress_seq_ = -1;
    pending_plan_ = {};
    compress_verify_ = true;   // 下一 turn done 时校验是否压到位（没压下 ⇒ 阈值退避）
    state_ = State::Idle;
    flush_queued_inputs_();
}

// 超窗就地自救：provider 报上下文超长 ⇒ 按冻结计划机械折叠后
// 重发当前请求。与 apply_digest_ 的分野：turn 仍在途——保持 state_/排队不动、
// 不置 compress_verify_（退避校验是 turn 边界折叠的语义）；重发次数由调用方
// （on_llm_error）以 kMaxOverwindowResend 封顶。
void AgentCtx::forced_fold_resend_() {
    replace_with_digest_(pending_plan_.digest, "超窗强制");
    pending_plan_ = {};
    send_chat();
}

// agent 内部失败的统一收口（没有工具表 / LLM 不可用 / LLM 未受理等）：发
// llm_error——webui 由此清 in_flight + 落错误卡，transcript 有终态记录（重放
// 安全）。request_id=0 是「agent 自发」哨兵：自己订阅的 on_llm_error 靠
// request_id != inflight_ 忽略它，否则同步派发重入 cleanup 会把排队输入排空两次。
void AgentCtx::fail_turn(const std::string& why) {
    const std::string msg = "turn #" + std::to_string(turn_no_) + " 失败: " + why;
    const vc_api::LlmError e{0, NOSTOS_ERR_FAILED, msg.c_str()};
    host_.emit(vc_api::llm_error, &e);
    cleanup_turn_();
}

// 状态机复位（不含对外事件——收口事件由调用方决定发不发）：持久化 + 清工具现场
// + 压缩判定（失败/取消收口同样过判定——上下文只增不减的反面）
// + 回 Idle + 排队输入 flush（栈深有界）。
void AgentCtx::cleanup_turn_() {
    persist_session_();
    calls_.clear();
    runs_.clear();
    // 摘要在途：折叠计划已冻结（pending_plan_），收口由摘要回包/失败分流完成
    // （on_compress_done_/on_compress_error_ → apply_digest_）——此处只清现场，
    // 不动 state_/排队。抢跑 flush 会让新 turn 的请求与摘要回包的区间替换交叠，
    // 破坏「摘要在途时 turn 请求不在途」的不变式（顺序纪律）。
    if (compress_inflight_) return;
    if (maybe_compress_()) return;  // 命中判定：机械折叠收口，或摘要在途（异步收口）
    state_ = State::Idle;
    flush_queued_inputs_();
}

// 排队输入 flush：队首开新 turn，其余等该 turn 收口时再取（栈深有界——不递归
// 弹空整队）。
void AgentCtx::flush_queued_inputs_() {
    if (queued_.empty()) return;
    const std::string next = queued_.front();
    queued_.pop_front();
    begin_turn(next);
}

}  // namespace vc_agent
