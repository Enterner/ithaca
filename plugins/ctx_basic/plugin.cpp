// plugin/ctx_basic —— 上下文管理策略插件（机械策略，发布 vc.ctx.v1）。
//
// 策略逻辑 = plugins/agent/context.hpp 纯函数层的组合镜像（include 复用实现，
// 不复制算法）：plan_compress 折叠计划 / compact_result 最小充分记录 /
// recall_result 回查包装 / mission_message 任务卡 / is_context_overflow_error
// 分类 / kSummarySystem 摘要指令。与 agent 内置回退的口径逐字节一致——本插件
// 活跃与缺席（内置接管）必须产出相同的上下文形态（A/B golden 逐字验收的根基）。
//
// 边界（契约 vc_ctx.hpp 头注）：信号与策略在本插件，时机与历史所有权在 agent
// ——plan 只回答"折不折、怎么折"，摘要请求的发送、消息历史替换、持久化都在
// agent。策略状态（阈值/退避）插件自理：不持久化、重启重置（代价小，避免策略
// 状态污染会话文件）。
//
// 外置目录：读 ITHACA_SESSION_FILE + 订阅 vc.session.load（派生规则与 agent 的
// 会话装载一致：<会话文件目录>/raw + 会话文件 stem 前缀）——两条外置写路径
//（本插件的工具结果归档与 agent 的折叠原文归档）落同一目录，ref 互认可回查。
//
// 选择开关：env ITHACA_CTX_ACTIVE（缺省/="basic" 活跃，其他值休眠）+
// vc.ctx.set_active 运行期热切（匹配 → publish / 不匹配 → unpublish，llm 两
// 供应商同构）。NOTE: 本插件运行期只 unpublish、不假设可被安全卸载（agent 不
// inject 本服务 ⇒ 内核 revoke 保护不到该依赖边，卸载安全不可言）。

#include "vc_ctx.hpp"
#include "vc_events.hpp"

#include "../agent/context.hpp"  // vcagent 纯函数层：plan_compress/compact_result/recall_result/…

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/active_gate.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using nlohmann::json;
using namespace vcagent;  // context.hpp 纯函数层（plan_compress/compact_result/…）

// 工具全名 → 末段（"office.read" → "read"）。与 agent 的分派口径一致：
// compact_result 与归档命名都按末段取形态。
std::string tool_last(const std::string& full) {
    const auto dot = full.rfind('.');
    return dot == std::string::npos ? full : full.substr(dot + 1);
}

class BasicCtxPlugin {
public:
    static constexpr std::string_view name = "vc.ctx.basic";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 1, 0);

    nostos_status activate(const nostos::abi::Host& host) {
        host_ = host;
        // 选择开关收编 kit::active_gate（竞争样板单源）：生效口径 env 缺省/="basic"
        // 活跃；显式选了别的策略 ⇒ 休眠不发布
        auto armed = gate_.arm(
            host, vc_api::ctx_set_active,
            [this](const void* p) {
                const auto* e = static_cast<const vc_api::CtxSetActive*>(p);
                if (e->active == nullptr)  // "" = 无偏好，保持现状
                    return nostos::kit::active_gate::prefer::keep;
                return std::string_view(e->active) == "basic"
                           ? nostos::kit::active_gate::prefer::mine
                           : nostos::kit::active_gate::prefer::other;
            },
            [this] { return publish_(); },
            [this] { host_.unpublish(vc_api::ctx); });
        if (!armed.ok) return NOSTOS_ERR_FAILED;
        subs_.push_back(std::move(armed.sub));
        // 外置目录：会话文件派生（activate 期 env 与会话切换事件同源）
        if (const char* v = std::getenv("ITHACA_SESSION_FILE"); v != nullptr && *v != '\0')
            rederive_raw_(v);
        // 兜底阈值 env（与 agent 内置同款旋钮同语义）：W 未知时的判定基线
        if (const char* v = std::getenv("ITHACA_CONTEXT_COMPRESS_TOKENS"))
            base_threshold_ = std::atoll(v);

        subs_.push_back(host_.on(vc_api::session_load, [this](const void* p) {
            const auto* e = static_cast<const vc_api::SessionLoad*>(p);
            if (e->path != nullptr && *e->path != '\0') rederive_raw_(e->path);
        }));
        return NOSTOS_OK;
    }

    void deactivate() noexcept {
        subs_.clear();
        if (gate_.published()) host_.unpublish(vc_api::ctx);
        host_.log(2, "ithaca.ctx.basic: 已停用");
    }

private:
    bool publish_() {
        nostos::abi::init_table(table_);
        table_.self = this;
        table_.info = &BasicCtxPlugin::info_thunk;
        table_.mission = &BasicCtxPlugin::mission_thunk;
        table_.ingest_tool_result = &BasicCtxPlugin::ingest_thunk;
        table_.recall = &BasicCtxPlugin::recall_thunk;
        table_.plan = &BasicCtxPlugin::plan_thunk;
        table_.classify_error = &BasicCtxPlugin::classify_thunk;
        table_.summary_prompt = &BasicCtxPlugin::summary_prompt_thunk;
        table_.apply_summary = &BasicCtxPlugin::apply_summary_thunk;
        if (host_.publish(vc_api::ctx, &table_) != NOSTOS_OK) {
            host_.log(4, "ithaca.ctx.basic: 发布 vc.ctx.v1 失败（同名服务已存在？）");
            return false;
        }
        return true;
    }

    // 外置目录派生（与会话装载同源）：agent 的折叠原文归档同落此目录，ref 互认
    void rederive_raw_(const std::string& session_path) {
        const fs::path sp(session_path);
        raw_dir_ = (sp.parent_path() / "raw").string();
        const std::string fn = sp.filename().string();
        raw_prefix_ = fn.substr(0, fn.find('.')) + "_";
        std::error_code ec;
        fs::create_directories(raw_dir_, ec);
    }

    // ---- 策略状态：阈值与退避（不持久化，重启重置）-----------------------------

    // 生效阈值重算（与 agent 内置 recompute_threshold_ 同口径）：W + max_output
    // 已知 ⇒ W − max_output（输出预留）；仅 W ⇒ 0.9×W（reserved 缺省 = W/10）；
    // 都未知 ⇒ env 兜底基线。下限 W/4。
    void recompute_threshold_(long long window, long long max_output) {
        if (window <= 0) {
            session_threshold_ = base_threshold_;
            return;
        }
        const long long reserved = max_output > 0 ? max_output : window / 10;
        const long long th = window - reserved;
        const long long floor_th = window / 4;
        session_threshold_ = th > floor_th ? th : floor_th;
    }

    // 硬保护线：0.95×W（*20/19 整数式）。W 未知 ⇒ 不启用。
    static bool hard_window_(long long last, long long window) {
        return window > 0 && last * 20 >= window * 19;
    }

    // 压缩后首 turn 的退避校验：仍超生效阈值 ⇒ 阈值 ×1.25 抬升，封顶 0.95×W
    //（越过即由硬保护接管）。仅真实 usage 参与——粗估信号调阈值 = 用垃圾调阈值。
    // 通告与 agent 内置同文案同出口（vc.notice），transcript 表现一致。
    void verify_backoff_(const json& signals) {
        if (!verify_pending_) return;
        verify_pending_ = false;
        const bool usage_real = signals.value("usage_real", false);
        const long long last = signals.value("last_prompt_tokens", 0LL);
        const long long window = signals.value("window", 0LL);
        if (usage_real && session_threshold_ > 0 && last >= session_threshold_) {
            session_threshold_ = session_threshold_ / 4 * 5;
            if (window > 0) {
                const long long cap = window / 20 * 19;
                if (session_threshold_ > cap) session_threshold_ = cap;
            }
            // NOTE: 载荷用具名缓冲——emit 同步派发，语句级临时一悬垂就是 UAF
            const std::string text = "上下文仍紧张——压缩阈值已上调至 " +
                                     std::to_string(session_threshold_) +
                                     " tokens，建议开新会话";
            const vc_api::Notice n{2, text.c_str(), 0};
            host_.emit(vc_api::notice, &n);
        }
    }

    // plan 槽：判定门槛（阈值/硬保护/退避）+ plan_compress 计划产出。
    // reason 语义（契约）：precheck = 门槛信号用请求体粗估；turn_end = 真实信号
    // + 退避校验；overflow / manual = 无条件尝试计划（重试上界等时机护栏在 agent）。
    json plan_impl(const json& state) {
        const json& signals = state["signals"];
        const std::string reason = state.value("reason", "");
        const long long last = signals.value("last_prompt_tokens", 0LL);
        const long long window = signals.value("window", 0LL);
        // 只在窗口/输出上限变化时重算（首次调用 last_* = -1 必算）——无条件重算
        // 会把退避抬升的阈值立即压回 f(window)，"压缩后仍紧张 ⇒ 阈值上调"整条
        // 机制失效且每 turn 重复失实通告（对齐 agent 内置的调用时机）
        const long long max_output = signals.value("max_output", 0LL);
        if (window != last_window_ || max_output != last_max_output_) {
            last_window_ = window;
            last_max_output_ = max_output;
            recompute_threshold_(window, max_output);
        }
        if (reason == "precheck") {
            const long long est = signals.contains("estimate_prompt_tokens") &&
                                          signals["estimate_prompt_tokens"].is_number_integer()
                                      ? signals["estimate_prompt_tokens"].get<long long>()
                                      : last;
            if (!(session_threshold_ > 0 && est > session_threshold_)) return none_();
        } else if (reason == "turn_end") {
            verify_backoff_(signals);
            const bool hard = hard_window_(last, window);
            const bool soft = session_threshold_ > 0 && last > session_threshold_;
            if (!hard && !soft) return none_();
        }
        // overflow / manual：无门槛（agent 持有重试上界与 Idle 护栏）
        if (!state.contains("messages") || !state["messages"].is_array()) return none_();
        const vcagent::CompressPlan plan =
            vcagent::plan_compress(state["messages"], !raw_dir_.empty());
        if (!plan.valid) return none_();
        json out{{"action", "fold"},
                 {"digest_message", plan.digest},
                 {"cut_begin", plan.cut_begin},
                 {"cut_end", plan.cut_end},
                 {"refs", plan.refs},
                 {"segments_folded", plan.segments_folded},
                 {"anchors_total", plan.anchors_total},
                 {"anchors_dropped", plan.anchors_dropped}};
        // 武装退避校验：turn_end 与 manual 的折叠都经 agent 的 apply_digest_ 出口
        //（内置语义），预检/超窗强制不经——口径与内置一致
        if (reason == "turn_end" || reason == "manual") verify_pending_ = true;
        return out;
    }

    static json none_() { return json{{"action", "none"}}; }

    // ingest 槽：外置 + 最小充分记录一步完成（与 agent 内置回灌管线同口径镜像）。
    json ingest_impl(const json& args) {
        std::string content = args.value("body", std::string());
        const std::string tool = args.value("tool", "");
        const std::string last = tool_last(tool);
        const bool content_inline =
            last == "read" ? content.size() <= vcagent::kContentInlineCap
          : last == "task" ? content.size() <= vcagent::kTaskInlineCap
          : false;
        if (content.size() > vcagent::kInlineResultCap && !content_inline) {
            if (last == "recall") {
                // recall 回包本身是 JSON——在 content 值内截断后重打包保持合法
                //（内置回灌管线同款注释：直接 trim 劈 JSON 串 = 非法 tool 消息）
                const json rj = json::parse(content, nullptr, false);
                if (rj.is_object() && rj.contains("content") && rj["content"].is_string()) {
                    json fixed = rj;
                    fixed["content"] =
                        vcws::trim_utf8(rj["content"].get<std::string>(), 1024) +
                        "…（回查内容过长，已截断）";
                    content = fixed.dump();
                } else {
                    content = vcws::trim_utf8(content, 2048) + "…（回查内容过长，已截断）";
                }
            } else {
                const std::string ref = spool_(args.value("turn", 0),
                                               static_cast<int>(args.value("seq", 0)), last,
                                               content);
                if (!ref.empty()) {
                    const json r = json::parse(content, nullptr, false);
                    content = vcagent::compact_result(tool, r, ref);
                } else {
                    content = vcws::trim_utf8(content, 2048) + "…（超出内联上限且外置失败）";
                }
            }
        }
        return json{{"content", content}};
    }

    // 工具结果归档：raw/<前缀>t<turn>_<seq>_<tool>.json。NOTE: agent 的折叠原文
    // 归档共用本目录但工具段恒为 "digest"——除名为 digest 的工具外无撞名面。
    std::string spool_(int turn, int seq, const std::string& tool, const std::string& body) {
        if (raw_dir_.empty()) return {};
        try {
            const std::string name = raw_prefix_ + "t" + std::to_string(turn) + "_" +
                                     std::to_string(seq) + "_" + tool + ".json";
            std::ofstream f(fs::path(raw_dir_) / name, std::ios::binary | std::ios::trunc);
            if (!f) return {};
            f << body;
            return name;
        } catch (const std::exception&) {
            return {};
        }
    }

    // recall 槽：ref 校验（裸文件名）→ 读归档 → entries 形态的结构化聚合
    //（summary/filter）→ 字节窗口分页兜底。与 agent 内置 exec_recall 同口径。
    json recall_impl(const json& args) {
        const std::string ref = args.value("ref", "");
        if (ref.empty() || raw_dir_.empty())
            return json{{"ok", false},
                        {"error", "ref 为空，或本会话未启用外置归档（折叠历史不可回查）"}};
        if (ref.find('/') != std::string::npos || ref.find('\\') != std::string::npos ||
            ref.find("..") != std::string::npos)
            return json{{"ok", false}, {"error", "ref 必须是裸文件名"}};
        std::ifstream in(fs::path(raw_dir_) / ref, std::ios::binary);
        if (!in) return json{{"ok", false}, {"error", "归档不存在: " + ref}};
        const std::string content((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        const long long total = static_cast<long long>(content.size());
        const bool summary = args.value("summary", false);
        const std::string filter = vcws::ascii_lower(args.value("filter", ""));
        if ((summary || !filter.empty()) && total > 0 && total < (8LL << 20)) {
            const json rj = json::parse(content, nullptr, false);
            if (rj.is_object() && rj.contains("entries") && rj["entries"].is_array()) {
                json filtered = json::array();
                std::size_t matched = 0;
                std::map<std::string, std::pair<long long, long long>> by_dir;
                for (const auto& e : rj["entries"]) {
                    if (!e.is_object()) continue;
                    const std::string p = e.value("path", "");
                    if (!filter.empty() &&
                        vcws::ascii_lower(p).find(filter) == std::string::npos)
                        continue;
                    ++matched;
                    const std::size_t slash = p.rfind('/');
                    const std::string bucket =
                        slash == std::string::npos ? "(根)" : p.substr(0, slash);
                    const std::string type = e.value("type", "file");
                    if (type == "dir") ++by_dir[bucket].second;
                    else if (type == "file") ++by_dir[bucket].first;
                    if (filtered.size() < 50) filtered.push_back(e);
                }
                json dirs = json::array();
                for (const auto& [d, c] : by_dir)
                    dirs.push_back(json{{"dir", d}, {"files", c.first}, {"subdirs", c.second}});
                json summ = json{{"summary_of", ref},
                                 {"total_entries", rj["entries"].size()},
                                 {"matched", matched}};
                if (!filter.empty()) summ["matching"] = filtered;
                if (summary) summ["dirs"] = dirs;
                return json::parse(vcagent::recall_result(ref, summ.dump(), total, 0, false, 0),
                                   nullptr, false);
            }
        }
        long long offset = args.value("offset", 0LL);
        const long long limit = std::clamp(args.value("limit", 2048LL), 1LL, 4096LL);
        offset = std::clamp(offset, 0LL, total);
        std::size_t off = static_cast<std::size_t>(offset);
        const std::string page =
            vcws::utf8_window(content, off, static_cast<std::size_t>(limit));
        const bool has_more = off + page.size() < static_cast<std::size_t>(total);
        return json::parse(vcagent::recall_result(ref, page, total,
                                                  static_cast<long long>(off), has_more,
                                                  static_cast<long long>(off + page.size())),
                           nullptr, false);
    }

    // 摘要回包后处理：provenance 头 + 超长截断 + 外置索引确定性重建（refs 来自
    // 折叠计划的结构化记录——与 agent 内置 on_compress_done_ 尾段同口径）。
    json apply_summary_impl(const json& args) {
        std::string s = args.value("summary", std::string());
        if (s.empty()) return {};   // 空回包 = 让 agent 走机械兜底
        std::vector<std::string> refs;
        if (args.contains("refs") && args["refs"].is_array())
            for (const auto& r : args["refs"])
                if (r.is_string()) refs.push_back(r.get<std::string>());
        if (s.size() > 4096) s = vcws::trim_utf8(s, 4096) + "…";
        json digest = json{
            {"role", "system"},
            {"content", std::string(vcagent::kDigestMarker) +
                            "（历史 turn 已压缩：LLM 自总结。" +
                            std::string(vcagent::kDigestProvenance) +
                            (raw_dir_.empty()
                                 ? "本会话未启用外置归档，被折叠原文不可回查）\n"
                                 : "工具原始结果外置于 raw/，可用 context_recall 回查）\n") +
                            s}};
        const std::string index = vcagent::index_section(refs);
        if (!index.empty())
            digest["content"] = digest["content"].get<std::string>() + "\n" + index;
        return digest;
    }

    // ---- C 槽位（noexcept thunk → 成员；出参缓冲调用期有效）----------------------

    static nostos_status info_thunk(void* self, const char** out) noexcept {
        static const std::string kInfo =
            json{{"name", "basic"},
                 {"capabilities", json::array({"spool", "recall", "llm_summary"})}}
                .dump();
        *out = kInfo.c_str();
        return NOSTOS_OK;
    }
    static nostos_status mission_thunk(void* self, const char* messages_json,
                                       const char** out) noexcept {
        auto& p = *static_cast<BasicCtxPlugin*>(self);
        json msgs = json::parse(messages_json != nullptr ? messages_json : "[]", nullptr,
                                false);
        if (!msgs.is_array()) return NOSTOS_ERR_INVALID_ARG;
        p.mission_buf_ = vcagent::mission_message(vcagent::collect_tasks(msgs)).dump();
        *out = p.mission_buf_.c_str();
        return NOSTOS_OK;
    }
    static nostos_status ingest_thunk(void* self, const char* args_json,
                                      const char** out) noexcept {
        auto& p = *static_cast<BasicCtxPlugin*>(self);
        json args = json::parse(args_json != nullptr ? args_json : "{}", nullptr, false);
        if (!args.is_object()) return NOSTOS_ERR_INVALID_ARG;
        try {
            p.out_buf_ = p.ingest_impl(args).dump();
        } catch (const std::exception& e) {
            p.out_buf_ = json{{"content",
                               json{{"ok", false}, {"error", std::string("策略收编内部错误: ") +
                                                                  e.what()}}
                                   .dump()}}
                          .dump();
        }
        *out = p.out_buf_.c_str();
        return NOSTOS_OK;
    }
    static nostos_status recall_thunk(void* self, const char* args_json,
                                      const char** out) noexcept {
        auto& p = *static_cast<BasicCtxPlugin*>(self);
        json args = json::parse(args_json != nullptr ? args_json : "{}", nullptr, false);
        if (!args.is_object()) return NOSTOS_ERR_INVALID_ARG;
        try {
            p.out_buf_ = p.recall_impl(args).dump();
        } catch (const std::exception& e) {
            p.out_buf_ = json{{"ok", false}, {"error", std::string("策略回查内部错误: ") +
                                                              e.what()}}
                        .dump();
        }
        *out = p.out_buf_.c_str();
        return NOSTOS_OK;
    }
    static nostos_status plan_thunk(void* self, const char* state_json,
                                    const char** out) noexcept {
        auto& p = *static_cast<BasicCtxPlugin*>(self);
        json state = json::parse(state_json != nullptr ? state_json : "{}", nullptr, false);
        if (!state.is_object() || !state.contains("signals") ||
            !state["signals"].is_object())
            return NOSTOS_ERR_INVALID_ARG;
        try {
            p.out_buf_ = p.plan_impl(state).dump();
        } catch (const std::exception& e) {
            p.out_buf_ = json{{"action", "none"}}.dump();
            p.host_.log(3, std::string("ithaca.ctx.basic: plan 内部异常（按不折叠处理）: ") +
                               e.what());
        }
        *out = p.out_buf_.c_str();
        return NOSTOS_OK;
    }
    static nostos_status classify_thunk(void* self, const char* error_text,
                                        const char** out) noexcept {
        auto& p = *static_cast<BasicCtxPlugin*>(self);
        p.out_buf_ = json{{"overflow", vcagent::is_context_overflow_error(
                                           error_text != nullptr ? error_text : "")}}
                     .dump();
        *out = p.out_buf_.c_str();
        return NOSTOS_OK;
    }
    static nostos_status summary_prompt_thunk(void* self, const char* digest_json,
                                              const char** out) noexcept {
        auto& p = *static_cast<BasicCtxPlugin*>(self);
        json args = json::parse(digest_json != nullptr ? digest_json : "{}", nullptr, false);
        if (!args.is_object() || !args.contains("digest") || !args["digest"].is_object())
            return NOSTOS_ERR_INVALID_ARG;
        p.out_buf_ = json{{"system", std::string(vcagent::kSummarySystem)},
                          {"instructions", args["digest"].value("content", "")}}
                     .dump();
        *out = p.out_buf_.c_str();
        return NOSTOS_OK;
    }
    static nostos_status apply_summary_thunk(void* self, const char* args_json,
                                             const char** out) noexcept {
        auto& p = *static_cast<BasicCtxPlugin*>(self);
        json args = json::parse(args_json != nullptr ? args_json : "{}", nullptr, false);
        if (!args.is_object()) return NOSTOS_ERR_INVALID_ARG;
        try {
            const json digest = p.apply_summary_impl(args);
            if (!digest.is_object()) return NOSTOS_ERR_INVALID_ARG;   // 空回包 ⇒ agent 机械兜底
            p.out_buf_ = digest.dump();
        } catch (const std::exception&) {
            return NOSTOS_ERR_FAILED;
        }
        *out = p.out_buf_.c_str();
        return NOSTOS_OK;
    }

    // ---- 状态 ---------------------------------------------------------------------

    nostos::abi::Host host_{};
    vc_api::CtxTable table_{};
    std::vector<nostos::abi::Host::Subscription> subs_;
    // 竞争发布门：选择开关（env 缺省/="basic" 活跃 + vc.ctx.set_active 热切，
    // 与 llm 两供应商同构）的机制托管（kit 单源）
    nostos::kit::active_gate gate_{"ithaca.ctx.basic", "ITHACA_CTX_ACTIVE", "basic",
                                   /*publish_when_unset=*/true};

    long long base_threshold_ = 32000;    // env 兜底基线（W 未知时的判定基线）
    long long session_threshold_ = 32000; // 生效阈值（窗口/输出上限变化时重算）
    long long last_window_ = -1;          // 上次重算所用的窗口（变化检测——每次 plan
                                          // 无条件重算会把 verify_backoff_ 的 ×1.25
                                          // 抬升立即抹平，退避机制整条失效；对齐
                                          // agent 内置只在元数据到位/切窗时重算）
    long long last_max_output_ = -1;
    bool verify_pending_ = false;         // turn_end 折叠已应用 ⇒ 下 turn 退避校验
    std::string raw_dir_;                 // 外置归档目录（随会话文件走）
    std::string raw_prefix_;              // 归档名前缀 = 会话 stem（跨会话防撞）

    std::string out_buf_;       // 出参存活缓冲（槽调用期有效）
    std::string mission_buf_;   // mission 槽出参缓冲
};

}  // namespace

NOSTOS_PLUGIN(BasicCtxPlugin)
