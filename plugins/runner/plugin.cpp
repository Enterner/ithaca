// plugin/runner —— 命令执行插件。
//
// 发布统一 ToolTable（svc 名 vc.tool.runner），唯一工具 "run"：
//   execute("run", {"command_line":..., "cwd":?, "timeout_ms":?, "dry_run":?})
//   ⇒ 立即返回 {"ok":true,"run_id":N}；输出经 vc.run.line、结束经 vc.run.exit
//   事件回（都在宿主线程，携带 run_id 归流——并发 run 互不串线）。
//
// 执行口把关（安全边界在执行者，agent 只转发）：
//   · 权限把关：三档权限模式——suggest/auto-edit 由 agent 确认清单把关
//     （提案→decision），full-auto 在 AppContainer 沙箱内执行（读宽写窄：系统+
//     PATH 工具目录只读、工作区可写、断网——机制详见 proc.hpp 沙箱段）；
//   · cwd 限定 jail 内；dry_run=true 只解析并回显 would_run（批准门素材）；
//   · 超时（默认 600s，上限 1h）⇒ TerminateProcess + 截断通告。
//
// 线程：每条 run 三个自有线程（两路读取 + 一路等待/回收），事件全部经
// kit::poster_submit 回宿主线程；deactivate 终止全部子进程并 join——无悬空。
// 输出经 UTF-8 合法性清洗（子进程字节流不保证 UTF-8，JSON 序列化要求合法）。
#include "iface.hpp"

#include "proc.hpp"

#include "vc_events.hpp"
#include "vc_tool.hpp"

#include "../workspace/text.hpp"  // vcws::utf8_sanitize：子进程输出进 JSON 前的 UTF-8 净化（全仓唯一实现）

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/piped_process.hpp>
#include <nostos/kit/poster.hpp>
#include <nostos/kit/win.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

using nlohmann::json;

constexpr std::size_t kOutputCap = 256 * 1024;   // 每 run 输出字节上限（超出丢弃并通告）
constexpr std::size_t kLineCap = 4 * 1024;       // 单行截断
constexpr int kDefaultTimeoutMs = 600000;
constexpr int kMaxTimeoutMs = 3600000;

// runner 的 cwd 容纳检查（最小化）：规范化后必须仍落在工作区根内。
// 与 workspace 的 jail.hpp 同族但更小（只需要目录前缀校验，不做保留设备名等
// 文件级检查——那些是文件读写工具的职责）。
class CwdJail {
public:
    explicit CwdJail(std::wstring root_wide) {
        const DWORD need = ::GetFullPathNameW(root_wide.c_str(), 0, nullptr, nullptr);
        std::wstring out(need, L'\0');
        const DWORD copied = ::GetFullPathNameW(root_wide.c_str(), need, out.data(), nullptr);
        out.resize(copied);
        while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
        root_ = std::move(out);
    }

    const std::wstring& root() const noexcept { return root_; }

    bool resolve(const std::string& rel_utf8, std::wstring& out_full, std::string& err) const {
        if (rel_utf8.empty()) { out_full = root_; return true; }
        if (rel_utf8.size() > 512 || rel_utf8[0] == '/' || rel_utf8[0] == '\\' ||
            (rel_utf8.size() >= 2 && rel_utf8[1] == ':')) {
            err = "cwd 必须是工作区内相对路径";
            return false;
        }
        if (rel_utf8.find("..") != std::string::npos) { err = "cwd 含 '..'（越界尝试）"; return false; }
        const std::wstring joined = root_ + L"\\" + nostos::kit::win::utf8_to_wide(rel_utf8);
        const DWORD need = ::GetFullPathNameW(joined.c_str(), 0, nullptr, nullptr);
        std::wstring canon(need, L'\0');
        const DWORD copied = ::GetFullPathNameW(joined.c_str(), need, canon.data(), nullptr);
        canon.resize(copied);
        if (canon.rfind(root_, 0) != 0) { err = "cwd 规范化后越出工作区根"; return false; }
        out_full = canon;
        return true;
    }

private:
    std::wstring root_;
};

class RunnerPlugin {
public:
    static constexpr std::string_view name = "vc.runner";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 1, 0);

    nostos_status activate(const nostos::abi::Host& host) {
        host_ = host;
        const char* env = std::getenv("ITHACA_WORKSPACE_DIR");
        jail_ = std::make_unique<CwdJail>(nostos::kit::win::utf8_to_wide(
            env != nullptr && *env != '\0' ? env : "."));
        // 权限模式默认值：webui 事件可热切；ITHACA_RUN_ALLOW
        // 白名单机制已移除（把关责任上移到权限模式与沙箱）
        mode_ = vc_api::normalize_permission_mode(
            std::getenv("ITHACA_PERMISSION_MODE") != nullptr
                ? std::getenv("ITHACA_PERMISSION_MODE") : "");
        sandboxed_ = (mode_ == "full-auto");
        // 工作区热切——重建 CwdJail 边界，立即生效
        subs_.push_back(host_.on(vc_api::workspace_set, [this](const void* p) {
            const auto* e = static_cast<const vc_api::WorkspaceSet*>(p);
            if (e->root == nullptr || *e->root == '\0') return;
            std::error_code ec;
            fs::create_directories(fs::path(e->root), ec);
            jail_ = std::make_unique<CwdJail>(nostos::kit::win::utf8_to_wide(e->root));
            host_.log(2, "ithaca.runner: 工作区已热切为 " + std::string(e->root));
        }));
        // 权限模式：full-auto ⇒ 命令在 AppContainer 沙箱内执行；值未变化
        // ⇒ 静默（启动对齐不刷日志；suggest/auto-edit 的把关在 agent 确认清单）
        subs_.push_back(host_.on(vc_api::permission_mode, [this](const void* p) {
            const auto* e = static_cast<const vc_api::PermissionMode*>(p);
            const std::string next = vc_api::normalize_permission_mode(
                e->mode != nullptr ? e->mode : "");
            if (next == mode_) return;
            mode_ = next;
            sandboxed_ = (mode_ == "full-auto");
        }));
        nostos::abi::init_table(table_);
        table_.self = this;
        table_.list = &RunnerPlugin::list_thunk;
        table_.execute = &RunnerPlugin::execute_thunk;
        if (host.publish(vcrun::kToolSvc, &table_) != NOSTOS_OK) {
            host.log(4, "ithaca.runner: 发布 vc.tool.runner 失败（同名服务已存在？）");
            return NOSTOS_ERR_FAILED;
        }
        // 领取 poster的异步事件全部经它回流——漏掉 ⇒ post 空指针调用）
        if (host.acquire_poster(&poster_) != NOSTOS_OK) {
            host.log(4, "ithaca.runner: 领取 poster 失败（宿主 ABI 过旧或未实现）");
            return NOSTOS_ERR_FAILED;
        }
        const vc_api::ToolRegister note{vcrun::kToolSvcName};
        host_.emit(vc_api::tool_register, &note);
        host.log(2, "ithaca.runner: 就绪（权限模式 " + mode_ + "，jail 根 = " +
                        nostos::kit::win::wide_to_utf8(jail_->root().c_str()) + "）");
        return NOSTOS_OK;
    }

    void deactivate() noexcept {
        stopping_.store(true);
        {
            std::lock_guard<std::mutex> lock(runs_mu_);
            for (auto& [id, entry] : runs_)
                if (entry.child != nullptr && entry.child->process != nullptr)
                    TerminateProcess(entry.child->process, 0xFFFFFFFFu);
        }
        // join 全部线程（子进程被终止 ⇒ 读取端 EOF ⇒ 读取线程自然结束）；
        // 线程对象不持 runs_mu_，锁外 join 无死锁。
        std::lock_guard<std::mutex> lock(runs_mu_);
        for (auto& [id, entry] : runs_) {
            if (entry.rd_out.joinable()) entry.rd_out.join();
            if (entry.rd_err.joinable()) entry.rd_err.join();
            if (entry.poster.joinable()) entry.poster.join();
        }
        runs_.clear();
        host_.log(2, "ithaca.runner: 已停用");
    }

private:
    struct RunEntry {
        std::unique_ptr<vcrun::ChildPipes> child;
        std::thread rd_out;    // stdout 读取线程：只读 + 缓冲（不碰 poster）
        std::thread rd_err;    // stderr 读取线程：只读 + 缓冲（不碰 poster）
        std::thread poster;    // 唯一的投递线程：轮询缓冲 → poster 事件 + exit
        std::mutex mu;
        std::vector<std::pair<int, std::string>> buffered;  // (stream, line)
        bool eof[2] = {false, false};  // 读取端 EOF（[0]=stdout [1]=stderr，mu 保护）
        std::atomic<std::size_t> bytes{0};
        std::atomic<bool> truncated{false};
        bool truncation_announced = false;
    };

    // ---- ToolTable 槽位 ----------------------------------------------------------

    static nostos_status list_thunk(void* self, const char** out_json) noexcept {
        auto* p = static_cast<RunnerPlugin*>(self);
        const json tools = json::array({json{
            {"name", "run"},
            {"description",
             "在工作区内执行命令（异步：立即返回 run_id，输出/退出码经事件回；"
             "执行权限由三档权限模式管理）。"
             "dry_run=true 只解析不执行"},
            {"schema", json{{"type", "object"},
                            {"properties",
                             json::object({{"command_line", {{"type", "string"}}},
                                            {"cwd", {{"type", "string"}}},
                                            {"timeout_ms", {{"type", "integer"}}},
                                            {"dry_run", {{"type", "boolean"}}}})},
                            {"required", json::array({"command_line"})}}}}});
        p->out_buf_ = json{{"ok", true}, {"tools", tools}}.dump();
        *out_json = p->out_buf_.c_str();
        return NOSTOS_OK;
    }

    static nostos_status execute_thunk(void* self, const char* tool, const char* args_json,
                                       int turn_id, const char** out_json) noexcept {
        return static_cast<RunnerPlugin*>(self)->tool_execute(tool, args_json, out_json);
    }

    nostos_status tool_execute(const char* tool, const char* args_json, const char** out) {
        json args = json::parse(args_json != nullptr ? args_json : "{}", nullptr, false);
        if (args.is_discarded() || !args.is_object()) args = json::object();
        try {
            const std::string t = tool != nullptr ? tool : "";
            if (t == "run") return finish(run_tool(args), out);
            return finish(json{{"ok", false}, {"error", "未知工具: \"" + t + "\""}}, out);
        } catch (const std::exception& e) {
            return finish(json{{"ok", false}, {"error", std::string("内部错误: ") + e.what()}},
                          out);
        }
    }

    nostos_status finish(json result, const char** out) {
        out_buf_ = result.dump();
        *out = out_buf_.c_str();
        return NOSTOS_OK;  // 工具级失败 = ok:false JSON，回灌模型
    }

    // ---- run 工具 ------------------------------------------------------------------

    json run_tool(const json& args) {
        const std::string cmd = args.value("command_line", std::string{});
        if (cmd.empty()) return json{{"ok", false}, {"error", "command_line 为空"}};

        // cwd：jail 内相对路径（空 = 根）
        std::wstring cwd_wide = jail_->root();
        if (const std::string rel = args.value("cwd", std::string{}); !rel.empty()) {
            std::string err;
            std::wstring full;
            if (!jail_->resolve(rel, full, err))
                return json{{"ok", false}, {"error", "cwd 被拒: " + err}};
            cwd_wide = full;
        }

        // dry_run：只解析 cwd，不执行——批准门（suggest/auto-edit 模式）的提案素材
        if (args.value("dry_run", false))
            return json{{"ok", true},
                        {"dry_run", true},
                        {"would_run", cmd},
                        {"cwd", nostos::kit::win::wide_to_utf8(cwd_wide.c_str())}};

        const int timeout_ms =
            std::min(std::max(args.value("timeout_ms", kDefaultTimeoutMs), 1000), kMaxTimeoutMs);

        // spawn（宿主线程；读线程/等待线程接管后续）
        auto child = std::make_unique<vcrun::ChildPipes>();
        std::string err;
        // full-auto：AppContainer 沙箱（读宽写窄 + 断网）；其余模式由
        // agent 确认清单把关（建议/自动编辑模式逐条批准）
        bool degraded = false;
        if (!vcrun::spawn(nostos::kit::win::utf8_to_wide(cmd), cwd_wide, *child, err,
                          sandboxed_, &degraded)) {
            // 逃生舱（Claude Code unsandboxed-retry 的降级版）：沙箱路径任何失败
            // （profile/ACL/属性装配/创建）都不回死错误——结构化提示回灌给 agent
            // 自我纠正（请示用户切模式/改用工作区内工具），与读错误自修纪律同构。
            if (sandboxed_)
                err += "（沙箱路径失败。可向用户说明并建议切换 suggest/auto-edit 模式"
                       "重跑，或改用工作区内的工具/venv）";
            return json{{"ok", false}, {"error", err}};
        }
        if (degraded && !degrade_warned_) {
            degrade_warned_ = true;   // 每进程一次：日志 + 对话流通告——听得见的降级
            host_.log(3, "ithaca.runner: full-auto 沙箱在本机不可用（SECURITY_CAPABILITIES "
                         "被拒——Windows build 26200 回归）——本次及后续命令降级为非沙箱执行");
            nostos::kit::poster_submit(poster_, [this] {
                const vc_api::Notice e{
                    3, "⚠ full-auto 沙箱在本机 Windows 上不可用（AppContainer 进程创建被"
                       "拒）——已降级为非沙箱执行；如需命令审批请切换 suggest/auto-edit", 1};
                host_.emit(vc_api::notice, &e);
            });
        }

        const int run_id = ++next_run_id_;
        RunEntry* entry = nullptr;
        {
            std::lock_guard<std::mutex> lock(runs_mu_);
            entry = &runs_[run_id];
            entry->child = std::move(child);
        }

        const auto reader = [this, entry](HANDLE h, int stream) {
            // 读取线程：只读 + 缓冲（mu 保护）——poster 投递集中在 poster 线程，
            // 消除并发投递竞态崩溃根因：两个读取线程并发过 poster 边界）。
            nostos::kit::LineSplitter splitter;
            for (;;) {
                std::string chunk;
                if (!nostos::kit::PipedProcess::read_chunk(h, chunk)) break;
                splitter.feed(chunk.data(), chunk.size(), [&](const std::string& line) {
                    std::lock_guard<std::mutex> lock(entry->mu);
                    entry->buffered.emplace_back(stream, line);
                });
            }
            splitter.flush([&](const std::string& line) {
                std::lock_guard<std::mutex> lock(entry->mu);
                entry->buffered.emplace_back(stream, line);
            });
            {
                std::lock_guard<std::mutex> lock(entry->mu);
                entry->eof[stream] = true;  // 本流 EOF；双流皆 EOF ⇒ 输出终止
            }
        };
        if (!std::getenv("ITHACA_M5_NORD")) {  // 二分实验开关（保留诊断）
            entry->rd_out = std::thread(reader, entry->child->stdout_read, 0);
            entry->rd_err = std::thread(reader, entry->child->stderr_read, 1);
        }
        entry->poster =
            std::thread([this, run_id, entry, timeout_ms] { pump_and_finish_(run_id, entry, timeout_ms); });

        return json{{"ok", true}, {"run_id", run_id}};
    }

    // 投递线程（每 run 唯一的 poster 调用者）：轮询缓冲 → RunLine 事件；
    // 进程确认终止且缓冲清空 ⇒ 终冲刷 → RunExit 事件。
    // 读取线程只缓冲（mu 保护），poster 投递集中在本线程——消除并发投递竞态。
    void pump_and_finish_(int run_id, RunEntry* entry, int timeout_ms) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        // 单行投递（主循环与收口终冲刷共用同一份净化/截断/超限纪律）
        const auto emit_line = [this, run_id, entry](int stream, const std::string& raw) {
            std::string line = vcws::utf8_sanitize(raw);
            if (line.size() > kLineCap) line = line.substr(0, kLineCap) + "…";
            if (entry->bytes.fetch_add(line.size() + 1) > kOutputCap) {
                if (!entry->truncated.exchange(true)) {
                    nostos::kit::poster_submit(poster_, [this, run_id] {
                        const vc_api::RunLine e{
                            run_id, 0, "（输出超过上限，后续内容已丢弃）"};
                        host_.emit(vc_api::run_line, &e);
                    });
                }
                return;
            }
            nostos::kit::poster_submit(poster_, [this, run_id, stream, line = std::move(line)] {
                const vc_api::RunLine e{run_id, stream, line.c_str()};
                host_.emit(vc_api::run_line, &e);
            });
        };
        bool killed = false;
        bool proc_gone_seen = false;
        std::chrono::steady_clock::time_point proc_gone_at{};
        for (;;) {
            if (stopping_.load(std::memory_order_relaxed) && !killed) {
                TerminateProcess(entry->child->process, 0xFFFFFFFFu);
                killed = true;
            }
            std::vector<std::pair<int, std::string>> take;
            bool both_eof = false;
            {
                std::lock_guard<std::mutex> lock(entry->mu);
                take.swap(entry->buffered);
                both_eof = entry->eof[0] && entry->eof[1];
            }
            if (!proc_gone_seen &&
                ::WaitForSingleObject(entry->child->process, 0) == WAIT_OBJECT_0) {
                proc_gone_seen = true;
                proc_gone_at = std::chrono::steady_clock::now();
            }
            for (const auto& [stream, raw] : take) emit_line(stream, raw);
            // NOTE: 收口判据是双流 EOF，不是进程退出——子进程死后管道里可能还压着
            // 未读数据（echo 后即退是最常见形态），读取端 EOF 才是"输出终止"的
            // 可靠信号；进程退出只作兜底：流永不 EOF（孙进程继承句柄/无读取线程）
            // 时经 2s 宽限再收口。两路都到齐才读退出码，否则 GetExitCodeProcess
            // 会把 STILL_ACTIVE(259) 当真退出码（踩过：单 bool 任一流 EOF 即收口）。
            if (both_eof && take.empty()) {
                if (entry->rd_out.joinable()) entry->rd_out.join();
                if (entry->rd_err.joinable()) entry->rd_err.join();
                std::vector<std::pair<int, std::string>> tail;
                {
                    std::lock_guard<std::mutex> lock(entry->mu);
                    tail.swap(entry->buffered);
                }
                for (const auto& [stream, raw] : tail) emit_line(stream, raw);
                if (!proc_gone_seen)
                    proc_gone_seen = ::WaitForSingleObject(
                        entry->child->process, 2000) == WAIT_OBJECT_0;
                DWORD code = 0xFFFFFFFFu;
                if (proc_gone_seen) GetExitCodeProcess(entry->child->process, &code);
                nostos::kit::poster_submit(poster_, [this, run_id, code] {
                    const vc_api::RunExit e{run_id, code};
                    host_.emit(vc_api::run_exit, &e);
                });
                break;
            }
            if (proc_gone_seen && !both_eof && take.empty() &&
                std::chrono::steady_clock::now() - proc_gone_at >
                    std::chrono::seconds(2)) {
                // 宽限期到仍无 EOF：句柄被孙进程继承等病理场景——放弃残余输出收口
                if (entry->rd_out.joinable()) entry->rd_out.detach();
                if (entry->rd_err.joinable()) entry->rd_err.detach();
                DWORD code = 0xFFFFFFFFu;
                GetExitCodeProcess(entry->child->process, &code);  // proc_gone_seen ⇒ 已退出
                nostos::kit::poster_submit(poster_, [this, run_id, code] {
                    const vc_api::RunExit e{run_id, code};
                    host_.emit(vc_api::run_exit, &e);
                });
                break;
            }
            if (!killed && std::chrono::steady_clock::now() >= deadline) {
                TerminateProcess(entry->child->process, 0xFFFFFFFFu);
                killed = true;
                nostos::kit::poster_submit(poster_, [this, run_id] {
                    const vc_api::RunLine e{run_id, 0, "（超时，已强制终止）"};
                    host_.emit(vc_api::run_line, &e);
                });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // ---- 状态 -----------------------------------------------------------------------

    nostos::abi::Host host_{};
    nostos_poster poster_{};
    std::vector<nostos::abi::Host::Subscription> subs_;  // 工作区热切订阅
    vc_api::ToolTable table_{};
    std::string out_buf_;

    std::unique_ptr<CwdJail> jail_;
    std::string mode_ = "auto-edit";  // 权限模式
    bool sandboxed_ = false;          // full-auto ⇒ AppContainer 沙箱 spawn
    bool degrade_warned_ = false;     // 沙箱降级通告只发一次（进程级）
    std::mutex runs_mu_;
    std::map<int, RunEntry> runs_;
    std::atomic<int> next_run_id_{0};
    std::atomic<bool> stopping_{false};
};

}  // namespace

NOSTOS_PLUGIN(RunnerPlugin)
