// 上下文压缩与外置存储（plugin/agent 的压缩切片）。
// 外置：超内联上限的工具结果落 raw/ 归档（spool_raw），context.recall
// 分页回查（exec_recall）；折叠：阈值维护、收口判定（maybe_compress_）、异步
// LLM 摘要与机械 digest 兜底、折叠应用（replace_with_digest_/apply_digest_/
// forced_fold_resend_）。压缩纯函数单源在 shared/vcagent/context.hpp（ctx_basic
// 策略镜像共用）；本文件是 agent 内置管线的执行体，表接管路径经 ctx_strategy.cpp。

#include "agent_plugin.hpp"

#include <fstream>    // std::ofstream：raw 外置归档

namespace vc_agent {

// 原始工具结果外置：raw/<turn>_<seq>_<tool>.json；失败 ⇒ 空 ref（调用方内联截断）
std::string AgentCtx::spool_raw(const std::string& tool, const std::string& result) {
    if (raw_dir_.empty()) return {};
    // 序号进程级递增：主/子代理各有 turn_no_（都从 1 起）共享同一 raw/，实例内
    // 计数会撞名互相覆盖——全局计数 + 会话 stem 前缀双重隔离
    static std::atomic<unsigned long long> g_raw_seq{0};
    try {
        const std::string name = raw_prefix_ + "t" + std::to_string(turn_no_) + "_" +
                                 std::to_string(++g_raw_seq) + "_" +
                                 vcagent::tool_last(tool) + ".json";
        std::ofstream f(fs::path(raw_dir_) / name, std::ios::binary | std::ios::trunc);
        if (!f) return {};
        f << result;
        return name;
    } catch (const std::exception&) {
        return {};
    }
}

// context.recall：ref 校验（裸文件名）→ 读归档 → provenance 包装；分页：
// offset/limit 字节窗口（limit 缺省 2048、上限 4096 = 单条内联护栏），回包带
// total/next_offset/has_more——大归档分段读。管线单源 = vcagent 的 recall_* 纯
// 函数（ctx_basic 策略镜像共用，产出逐字节一致）。
std::string AgentCtx::exec_recall(const json& args) {
    // 策略缝：表接管时回查执行体在策略内（外置目录/分页/聚合过滤口径随策略）
    if (ctx_tbl_ != nullptr && ctx_tbl_->recall != nullptr) {
        const auto r = vckit::slot_call_str(args, [&](const char* s, const char** out) {
            return ctx_tbl_->recall(ctx_tbl_->self, s, out);
        });
        if (!r.ok) return err_result("context.recall（策略插件）执行失败");
        return r.body;
    }
    const std::string ref = args.value("ref", "");
    std::string content, err;
    if (!vcagent::recall_fetch(ref, raw_dir_, content, err))
        return err_result(err);
    const long long total = static_cast<long long>(content.size());
    const std::string filter = vcws::ascii_lower(args.value("filter", ""));
    // 结构化回查：归档是工具结果的 JSON dump——entries 形态（list 归档）支持
    // filter 与 summary 聚合，单页返回不切字节；其余形态回退字节窗口分页
    if (std::string summ = vcagent::recall_entries_summary(
            content, ref, args.value("summary", false), filter);
        !summ.empty())
        return vcagent::recall_result(ref, summ, total, 0, false, 0);
    return vcagent::recall_paged(content, args, ref, total);
}

// ---- 阈值维护与折叠应用 --------------

void AgentCtx::set_context_window_(long long w) {
    ctx_window_ = w;
    recompute_threshold_();   // 模型/供应商切换重置退避（窗口变了旧基数失效）
}

// 生效阈值：W 与 max_output 已知 ⇒ W − max_output（输出预留——给
// 长 diff/长测试输出留位，避免贴着 0.9W 的请求被大响应顶爆）；仅 W 已知 ⇒
// 0.9×W（历史语义）；都未知 ⇒ 兜底阈值。下限 W/4：max_output 元数据异常巨大时
// 仍保住 25% 的 prompt 空间。公式单源 = vcagent::effective_threshold。
void AgentCtx::recompute_threshold_() {
    session_threshold_ =
        vcagent::effective_threshold(ctx_window_, ctx_max_output_, compress_threshold_);
}

// LLM 精修可用：无策略表（内置管线）或策略表提供了 summary_prompt 槽。
bool AgentCtx::llm_summary_ready_() const {
    return ctx_tbl_ == nullptr || ctx_tbl_->summary_prompt != nullptr;
}

// 压缩判定共用入口：finish_turn 与 cleanup_turn_ 都走这里（失败/取消收口不漏
// 判定——否则反复取消/失败的长会话上下文只增不减）。计划不可折叠 ⇒ false，
// 调用方走原收口；若下一请求仍超窗，由 on_llm_error 的超窗自救接手。
bool AgentCtx::maybe_compress_() {
    if (subagent_) return false;  // 子代理总闸：单任务短命上下文，压缩语义（回 Idle/
                                  // 摘要异步收口/退避校验）全部不适用
    // 策略缝：表接管时判定门槛与计划都在策略内（turn_end 信号透传）；agent 只
    // 决定是否 LLM 精修（summary_enabled_ + 插件给不给 summary_prompt 槽）。
    if (ctx_tbl_ != nullptr && ctx_tbl_->plan != nullptr) {
        const vcagent::CompressPlan plan = acquire_plan_("turn_end");
        if (!plan.valid) return false;
        if (summary_enabled_ && llm_summary_ready_()) {
            pending_plan_ = plan;
            request_digest_();  // 异步收口：摘要回包/失败在 on_done/on_llm_error 分流
            return true;
        }
        pending_plan_ = plan;
        apply_digest_(plan.digest, false);  // apply_digest_ 内收口（回 Idle + flush 排队）
        return true;
    }
    const bool hard = vcagent::hard_window(last_prompt_tokens_, ctx_window_);
    if (!hard && !vcagent::should_compress(last_prompt_tokens_, session_threshold_))
        return false;
    const vcagent::CompressPlan plan = vcagent::plan_compress(messages_, !raw_dir_.empty());
    if (!plan.valid) return false;
    if (!hard && summary_enabled_ && llm_summary_ready_()) {
        pending_plan_ = plan;
        request_digest_();  // 异步收口：摘要回包/失败在 on_done/on_llm_error 分流
        return true;
    }
    // NOTE: 机械路径也必须先冻结计划——replace_with_digest_ 的 cut 区间/折叠统计
    // 读 pending_plan_（不设则全零：digest 插到 0 号位、原文一条不裁——踩过: 
    // 手动压缩曾因此只插不剪）
    pending_plan_ = plan;
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
    // 摘要素材与指令：表接管时由策略给（summary_prompt 槽），内置 = kSummarySystem
    // + 机械 digest 正文。发送权留 agent（request_id 出自 agent 发号器——插件
    // 自发会被 agent 当成自己的应答收进历史）。
    std::string system = std::string(vcagent::kSummarySystem);
    std::string material = pending_plan_.digest.value("content", "");
    if (ctx_tbl_ != nullptr && ctx_tbl_->summary_prompt != nullptr)
        strategy_summary_brief_(system, material);
    json body = {{"slot", turn_no_},
                   {"messages", json::array({
                                  json{{"role", "system"}, {"content", system}},
                                  json{{"role", "user"}, {"content", material}}})}};
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
    // 策略缝：表接管时摘要后处理（裁切/provenance/外置索引重建）在策略内；
    // 槽失败机械 digest 兜底（压缩永不卡死）。
    if (ctx_tbl_ != nullptr && ctx_tbl_->apply_summary != nullptr) {
        json out_digest;
        if (strategy_apply_summary_(s, out_digest))
            return apply_digest_(out_digest, true);
        return apply_digest_(pending_plan_.digest, false);
    }
    // 定稿组装（provenance 头 + 截断 + 索引重建）单源 = vcagent::summary_digest
    //（ctx_basic 策略镜像共用，产出逐字节一致）
    apply_digest_(vcagent::summary_digest(std::move(s), !raw_dir_.empty(),
                                          pending_plan_.refs),
                  true);
}

void AgentCtx::on_compress_error_(const vc_api::LlmError&) {
    apply_digest_(pending_plan_.digest, false);   // 摘要失败 ⇒ 机械兜底（计划里已备好）
}

// 折叠应用核心：区间替换 + 旧 digest 并入 + 任务卡锚点吸收 + 持久化 + 通告。
// 顺序纪律：先插 digest 再做跳过判断（踩过: 跳过分支先 continue 会漏掉插入，
// digest 整条丢失）。
void AgentCtx::replace_with_digest_(const json& digest_msg, const char* via) {
    // 任务卡折叠重建：在切割前的完整历史上合成——被折叠的 user 消息切割后即不在
    // messages_ 里，锚点全集只在此刻可见。折叠本身已重置缓存前缀，此刻重写卡
    // 零前缀代价（卡跨轮冻结的吸收点就在这）。表接管时 mission 槽收到同一份
    // 完整历史；null/失败 = 保留旧卡（锚点不全好过丢卡）。
    json mission_fresh;
    const bool mission_valid = mission_from_slot_or_builtin_(mission_fresh);
    // 折叠前原文归档：cut 区间全文写入 raw/，ref 附进 digest 正文——agent 视图
    // 丢掉的每一行都可 recall 回查
    json cut = json::array();
    for (std::size_t k = pending_plan_.cut_begin;
         k < pending_plan_.cut_end && k < messages_.size(); ++k)
        cut.push_back(messages_[k]);
    const std::string archive_ref = spool_raw("digest", cut.dump());
    json digest = digest_msg;
    if (!archive_ref.empty())
        digest["content"] = digest_msg.value("content", "") + "【折叠原文】" +
                            archive_ref + "（context_recall 可回查）\n";
    // 锚点保真在线观测：折叠区逐字锚点在折叠产物中的覆盖率——
    // 32KB 降级会丢最旧段的调用链锚点，比率落 host log 供评测采集
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
        // 跳过分支先 continue 会漏掉插入（踩过: digest 整条丢失）
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
    // 卡原位替换（卡不在切割区间：cut 从首个 user 起，卡在其前）——折叠重建点
    if (mission_valid)
        for (std::size_t k = 0; k < messages_.size(); ++k)
            if (vcagent::is_mission(messages_[k])) {
                messages_[k] = mission_fresh;
                break;
            }
    notice(std::string("上下文已压缩（") + via + "折叠：" +
           std::to_string(pending_plan_.segments_folded) + " 段历史，消息 " +
           std::to_string(before) + " → " + std::to_string(messages_.size()) +
           (raw_dir_.empty() ? "；本会话未启用外置归档" : "；原始结果在 raw/ 可 recall") +
           "）");
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

}  // namespace vc_agent
