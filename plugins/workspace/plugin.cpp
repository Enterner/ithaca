// plugin/workspace —— 文件事务插件。
//
// 发布统一 ToolTable（契约 vc_tool.hpp；svc 名 vc.tool.workspace）：
//   read / list / search —— 只读工具，同步返回
//   apply                —— 编辑事务（dry_run 校验+diff；正式落盘并按 turn 分组记快照）
//   undo_turn            —— 整组弹回（两段式：先全组裁决再写回，任一冲突 ⇒ 零改动）
//
// jail 根：env ITHACA_WORKSPACE_DIR（UTF-8）> 当前目录。越界 / 保留设备名 /
// 段尾点一律拒绝，拒绝原因回灌给模型自我纠正。
//
// 线程纪律：无自有线程、无 poster 需求；表调用来自两个线程——agent 的宿主线程
// （事件回调链）与 webui 的 HTTP select 线程（文件树 /api/fs/list 直调），
// tool_execute 以全工具串行锁（tool_mu_）串行化两路调用。
// 状态边界：快照是纯内存态——插件卸载即弃，撤销能力随激活会话一起撤销（文档明示）。
#include "iface.hpp"

#include "vc_events.hpp"
#include "vc_tool.hpp"

#include "diff.hpp"
#include "edit.hpp"
#include "fs.hpp"
#include "jail.hpp"
#include "snapshots.hpp"

#include <nostos/abi/plugin.hpp>
#include <nostos/kit/regex.hpp>   // search 的正则（有界 NFA——std::regex 的回溯可被挂死）
#include <nostos/kit/win.hpp>

#include <algorithm>  // std::count/clamp/max：read 行统计与分页参数收敛
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

using nlohmann::json;

constexpr std::size_t kReadCap = 256 * 1024;   // read 的内容字节上限（切片前的整读护栏）
constexpr int kReadLinesCap = 2000;            // read 单次返回行数上限（默认值 = 参数上限）
constexpr int kReadLineCharCap = 2000;         // read 单行字符上限（超长行截断行尾）
constexpr std::size_t kListEntriesCap = 5000;   // list 条目护栏：浏览走默认聚合，flat 的
                                                // 定位是精确计数/全量过滤——上限只做遍历
                                                // 终止与体量护栏，不是浏览上限
constexpr int kListDepthCap = 6;               // list 递归深度上限
constexpr std::size_t kSearchFilesCap = 2000;  // search 扫描文件数上限
constexpr int kSearchDepthCap = 8;
constexpr std::size_t kSearchHitCap = 50;      // search 命中上限（默认值，参数可降）

// diff 文本尾行的统计 "（+A −R 行）" → "+A −R"（diff.hpp 生成该尾行；无 ⇒ 空串）
inline std::string diff_delta(const std::string& diff) {
    const std::size_t pos = diff.rfind("（+");
    if (pos == std::string::npos) return {};
    const std::size_t end = diff.find(" 行）", pos);
    if (end == std::string::npos) return {};
    return diff.substr(pos + 3, end - pos - 3);   // 跳过全角括号"（"（3B），保留 "+A −R" 的加号
}

// 文件名通配匹配（* 任意串、? 单字符；大小写不敏感——Windows 名空间惯例）。
// 迭代双指针 + 星号回溯，O(p+n)，无 locale 依赖（只折叠 ASCII 大写）
inline std::string ascii_lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

inline bool glob_match_ci(const std::string& pat, const std::string& name) {
    const std::string p = ascii_lower(pat), n = ascii_lower(name);
    std::size_t pi = 0, ni = 0, star = std::string::npos, mark = 0;
    while (ni < n.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == n[ni])) { ++pi; ++ni; }
        else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ni; }
        else if (star != std::string::npos) { pi = star + 1; ni = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}
constexpr std::size_t kDiffShowCap = 8 * 1024; // 单文件 diff 文本截断（结果体积护栏）

class WorkspacePlugin {
public:
    static constexpr std::string_view name = "vc.workspace";
    static constexpr std::uint32_t version = NOSTOS_PLUGIN_VERSION(0, 1, 0);

    nostos_status activate(const nostos::abi::Host& host) {
        host_ = host;
        const char* env = std::getenv("ITHACA_WORKSPACE_DIR");
        jail_ = std::make_unique<vcws::Jail>(nostos::kit::win::utf8_to_wide(
            env != nullptr && *env != '\0' ? env : "."));
        store_ = std::make_unique<vcws::DiskStore>();

        nostos::abi::init_table(table_);
        table_.self = this;
        table_.list = &WorkspacePlugin::list_thunk;
        table_.execute = &WorkspacePlugin::execute_thunk;
        if (host.publish(vcws::kToolSvc, &table_) != NOSTOS_OK) {
            host.log(4, "ithaca.workspace: 发布 vc.tool.workspace 失败（同名服务已存在？）");
            return NOSTOS_ERR_FAILED;
        }

        // 热接入提示（发现的主通道是 agent 的配置清单；这是给面板/日志看的）
        const vc_api::ToolRegister note{vcws::kToolSvcName};
        host_.emit(vc_api::tool_register, &note);
        host.log(2, "ithaca.workspace: 就绪，jail 根 = " +
                        nostos::kit::win::wide_to_utf8(jail_->root().c_str()));

        // 工作区热切——重建 Jail 边界，立即生效
        subs_.push_back(host_.on(vc_api::workspace_set, [this](const void* p) {
            const auto* e = static_cast<const vc_api::WorkspaceSet*>(p);
            if (e->root == nullptr || *e->root == '\0') return;
            std::error_code ec;
            fs::create_directories(fs::path(e->root), ec);
            jail_ = std::make_unique<vcws::Jail>(nostos::kit::win::utf8_to_wide(e->root));
            host_.log(2, "ithaca.workspace: 工作区已热切为 " + std::string(e->root));
        }));
        return NOSTOS_OK;
    }

    void deactivate() noexcept {
        subs_.clear();
        snaps_ = vcws::SnapshotStack{};  // 快照是内存态：随激活会话一起撤销
        host_.log(2, "ithaca.workspace: 已停用");
    }

private:
    // ---- ToolTable 槽位（noexcept thunk → 成员函数）----------------------------

    static nostos_status list_thunk(void* self, const char** out_json) noexcept {
        return static_cast<WorkspacePlugin*>(self)->tool_list(out_json);
    }
    static nostos_status execute_thunk(void* self, const char* tool, const char* args_json,
                                       int turn_id, const char** out_json) noexcept {
        return static_cast<WorkspacePlugin*>(self)->tool_execute(tool, args_json, turn_id,
                                                                 out_json);
    }

    nostos_status tool_list(const char** out) {
        // 工具自述：name + description + JSON schema——agent 据此拼 LLM 的 tools 数组，
        // 本插件不解释、agent 不硬编码（新增工具插件零 agent 改动的根基）。
        json schema_path = {{"type", "string"}, {"description", "工作区内相对路径"}};
        // NOTE: properties 必须显式建 json::object——花括号初始化会把 {"k",v} 对
        // 变成 [ ["k",v], ... ] 数组对，DeepSeek/OpenAI 的 JSON Schema 不认（踩过：400）
        auto object_with = [](std::initializer_list<json::value_type> props,
                              json required) {
            json properties = json::object();
            for (const auto& p : props) {
                if (p.is_array() && p.size() == 2 && p[0].is_string())
                    properties[p[0].get<std::string>()] = p[1];
            }
            return json{{"type", "object"}, {"properties", std::move(properties)},
                        {"required", std::move(required)}};
        };
        json tools = json::array(
            {json{{"name", "read"},
                  {"description",
                   "读取工作区内文本文件（拒绝二进制）。返回带行号（行首 = 文件真实"
                   "行号 + 制表符），标注出处直接引用行号即可。一次调用即可读满 "
                   "2000 行，无需手动分小块；更大的文件用 offset（起始行号，1 起）/"
                   "limit（行数）续读，单行超 2000 字符截断行尾。返回带 total_lines，"
                   "可据此判断是否读全"},
                  {"schema", object_with(
                                 {{"path", schema_path},
                                  {"offset", {{"type", "integer"},
                                              {"description", "起始行号（1 起，默认 1）"}}},
                                  {"limit", {{"type", "integer"},
                                             {"description", "本次读取行数（默认/上限 2000）"}}}},
                                 json::array({"path"}))}},
             json{{"name", "list"},
                  {"description",
                   "列出目录内容。默认 mode=aggregate 聚合视图：每目录一条汇总（文件数/子目录数/字节量），大目录一次看全貌；要看具体文件名再 mode=flat（可配 pattern/per_dir）下钻。可选："
                   "pattern=文件名通配（* ?，大小写不敏感，只过滤文件、目录保留）；"
                   "mode 默认 aggregate（聚合视图，每目录一条：文件数/子目录数/字节量）；flat=平铺看具体文件名；"
                   "per_dir=flat 模式单目录条目上限（超出折叠为 omitted 标注）。"
                   "返回含 listed（实际条数）与 truncated 标记"},
                  {"schema", object_with(
                                 {{"path", schema_path},
                                  {"depth", {{"type", "integer"}, {"description", "1-6"}}},
                                  {"pattern", {{"type", "string"}, {"description", "文件名通配，如 *.mp4"}}},
                                  {"mode", {{"type", "string"}, {"description", "aggregate（默认）| flat——大目录用默认，看具体文件名用 flat + pattern"}}},
                                  {"per_dir", {{"type", "integer"}, {"description", "flat 模式单目录文件条目上限，0=不限"}}}},
                                 json::array())}},
             json{{"name", "search"},
                  {"description",
                   "工作区内搜索文本：默认字面量子串（大小写敏感）；regex=true 用 "
                   "ECMAScript 正则，ignore_case=true 忽略大小写（两种模式均有效）。"
                   "默认上限 50 命中，单文件 >1MB 跳过"},
                  {"schema", object_with(
                                 {{"query", {{"type", "string"}}},
                                  {"max_hits", {{"type", "integer"}}},
                                  {"regex", {{"type", "boolean"}, {"description", "query 按正则解析"}}},
                                  {"ignore_case", {{"type", "boolean"}, {"description", "忽略大小写"}}}},
                                 json::array({"query"}))}},
             json{{"name", "apply"},
                  {"description",
                   "编辑事务：write（整文件）+ replace（精确块，默认必须恰好命中一次，"
                   "replace_all=true 全部替换）。大段修改优先拆成多个 replace 分块提交"
                   "（参数只携带变化块），write 留给新建文件或全文重写。"
                   "dry_run=true 只校验并生成 diff、不落盘；正式落盘按 turn 记入撤销快照，"
                   "diff 正文随结果回传（8KB/文件护栏）并经 applied 事件展示给用户"},
                  {"schema", object_with(
                                 {{"dry_run", {{"type", "boolean"}}},
                                  {"ops", {{"type", "array"}}}},
                                 json::array({"ops"}))}},
             json{{"name", "undo_turn"},
                  {"description",
                   "撤销一个 turn 的全部 workspace 编辑（默认最近一个）。范围仅限"
                   "编辑：runner 命令产生的文件/副作用不在撤销内；外部改动过的"
                   "文件会让整组拒绝"},
                  {"schema", object_with({{"turn_id", {{"type", "integer"}}}}, json::array())}}});
        return finish(json{{"ok", true}, {"tools", tools}}, out);
    }

    nostos_status tool_execute(const char* tool, const char* args_json, int turn_id,
                               const char** out) {
        // NOTE: 全工具串行锁——除 agent 外，webui 的 HTTP select 线程也直调本表
        // （文件树 /api/fs/list），而 list/search 用共享遍历缓冲、finish 写共享
        // 出参缓冲 out_buf_，双线程并发即竞态。锁粒度 = 单次工具调用，list 被
        // 500 条上限兜底、apply 有快照纪律，不会长持锁。
        std::lock_guard<std::mutex> lock(tool_mu_);
        json args = json::parse(args_json != nullptr ? args_json : "{}", nullptr,
                                /*allow_exceptions=*/false);
        if (args.is_discarded() || !args.is_object()) args = json::object();
        try {
            const std::string t = tool != nullptr ? tool : "";
            if (t == "read") return finish(read_tool(args), out);
            if (t == "list") return finish(list_tool(args), out);
            if (t == "search") return finish(search_tool(args), out);
            if (t == "apply") return finish(apply_tool(args, turn_id), out);
            if (t == "undo_turn") return finish(undo_tool(args), out);
            return finish(json{{"ok", false}, {"error", "未知工具: \"" + t + "\""}}, out);
        } catch (const std::exception& e) {
            return finish(json{{"ok", false}, {"error", std::string("内部错误: ") + e.what()}},
                          out);
        }
    }

    // 约定：NOSTOS_OK 只代表"调用成功"；工具级失败（文件不存在、命中不为 1、越界被拒…）
    // 以 ok:false + error 的 JSON 返回——这是给模型的正常回灌通路，不是基础设施错误。
    nostos_status finish(json result, const char** out) {
        out_buf_ = result.dump();
        *out = out_buf_.c_str();
        return NOSTOS_OK;
    }

    // ---- 工具实现 ---------------------------------------------------------------

    json read_tool(const json& args) {
        std::wstring full;
        std::string norm, err;
        if (!resolve_arg(args.value("path", ""), full, norm, err)) return fail(err);
        std::string content;
        if (!store_->read(full, content))
            return json{{"ok", false}, {"error", "文件不存在或不可读: " + norm}};
        if (content.find('\0') != std::string::npos)
            return json{{"ok", false},
                        {"error", "疑似二进制文件（含 NUL），拒绝文本读取: " + norm +
                                      "（docx/pptx 等容器格式可用 runner 调 Python 等"
                                      "脚本解析）"}};
        if (!vcws::utf8_valid(content))
            return json{{"ok", false}, {"error", "不是有效的 UTF-8 文本: " + norm}};
        // 按行分页（限制作用在「读」的动作上，结果内联一次回灌、无二次回查协议）。
        // 行按 \n 切、行内 \r 原样保留——重组与原文逐字节一致，apply 的精确块
        // 匹配不受分页展示影响。行首带行号（cat -n 形态，编号 = 文件真实行号，
        // 随 offset 对齐）：模型标注行号直接引用，不再需要逐符号 search 定位
        // （踩过：正文无行号 ⇒ 子代理为标行号发起几十次单符号搜索）
        const bool empty = content.empty();
        const int total_lines =
            empty ? 0
                  : static_cast<int>(std::count(content.begin(), content.end(), '\n')) +
                        (content.back() == '\n' ? 0 : 1);
        const int offset = std::max(args.value("offset", 1), 1);
        const int limit =
            std::clamp(args.value("limit", kReadLinesCap), 1, kReadLinesCap);
        std::string picked;
        bool line_truncated = false;
        if (!empty && offset <= total_lines) {
            std::size_t pos = 0;
            // 跳过前 offset-1 行
            for (int ln = 1; ln < offset && pos != std::string::npos; ++ln)
                pos = content.find('\n', pos) + 1;
            for (int ln = 0; ln < limit && pos < content.size(); ++ln) {
                const std::size_t end = content.find('\n', pos);
                const std::size_t stop = end == std::string::npos ? content.size() : end + 1;
                std::string line = content.substr(pos, stop - pos);
                if (line.size() > static_cast<std::size_t>(kReadLineCharCap) + 1) {
                    // 保留行尾 \n：截断到字符边界再补回（行内容仍以换行收尾）
                    line = vcws::trim_utf8(line, static_cast<std::size_t>(kReadLineCharCap));
                    line += "\n";
                    line_truncated = true;
                }
                picked += std::to_string(offset + ln) + "\t" + line;
                pos = stop;
            }
        }
        bool truncated = line_truncated || offset + limit <= total_lines;
        if (picked.size() > kReadCap) {
            picked = vcws::trim_utf8(picked, kReadCap);
            truncated = true;
        }
        return json{{"ok", true},
                    {"path", norm},
                    {"content", picked},
                    {"truncated", truncated},
                    {"total_lines", total_lines},
                    {"offset", offset},
                    {"limit", limit}};
    }

    json list_tool(const json& args) {
        const std::string rel = args.value("path", "");
        std::wstring full;
        std::string norm, err;
        if (rel.empty()) {
            full = jail_->root();
        } else if (!resolve_arg(rel, full, norm, err)) {
            return fail(err);
        }
        std::error_code ec;
        if (!fs::exists(full, ec) || !fs::is_directory(full, ec))
            return json{{"ok", false}, {"error", "目录不存在: " + (norm.empty() ? "." : norm)}};
        const int depth = std::min(args.value("depth", 3), kListDepthCap);
        pattern_ = ascii_lower(args.value("pattern", ""));
        aggregate_ = args.value("mode", "aggregate") != "flat";   // 默认聚合（显式 flat 才平铺）
        per_dir_ = std::max(args.value("per_dir", 0), 0);
        entries_ = json::array();
        listed_ = 0;
        truncated_ = false;
        if (aggregate_) walk_agg(full, norm, depth);
        else walk(full, norm, depth, 0);
        // listed 量化反馈：配合 truncated，模型能算出「看到了多少 / 还有多少量级」
        return json{{"ok", true},
                    {"entries", entries_},
                    {"listed", listed_},
                    {"truncated", truncated_}};
    }

    void walk(const fs::path& dir, const std::string& rel_prefix, int depth, int dir_files) {
        if (depth <= 0 || listed_ >= kListEntriesCap) {
            truncated_ = truncated_ || listed_ >= kListEntriesCap;
            return;
        }
        int omitted = 0;   // 本目录因 per_dir 被折叠的文件数
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end && !truncated_; it.increment(ec)) {
            if (ec) break;
            std::error_code se;
            const bool is_dir = it->is_directory(se) && !se;
            if (it->is_symlink(se) && !se) continue;  // 重解析点不入遍历（防环、防出 jail）
            const std::string name =
                nostos::kit::win::wide_to_utf8(it->path().filename().c_str());
            const std::string child_rel =
                rel_prefix.empty() ? name : rel_prefix + "/" + name;
            // pattern 只过滤文件（目录始终保留——结构信息不因过滤丢失）；不匹配
            // 的文件不占 500 配额
            if (!is_dir && !pattern_.empty() && !glob_match_ci(pattern_, name)) continue;
            if (is_dir) {
                entries_.push_back(json{{"path", child_rel}, {"type", "dir"}, {"size", 0}});
                ++listed_;
                if (listed_ >= kListEntriesCap) { truncated_ = true; return; }
                walk(it->path(), child_rel, depth - 1, 0);
                continue;
            }
            ++dir_files;
            if (per_dir_ > 0 && dir_files > per_dir_) { ++omitted; continue; }
            std::uint64_t size = 0;
            std::error_code ze;
            size = static_cast<std::uint64_t>(it->file_size(ze));
            entries_.push_back(
                json{{"path", child_rel}, {"type", "file"}, {"size", size}});
            ++listed_;
            if (listed_ >= kListEntriesCap) { truncated_ = true; return; }
        }
        if (ec) truncated_ = true;
        if (omitted > 0)   // 每目录超限标注：省略数可量化（比裸 truncated 布尔可读）
            entries_.push_back(json{{"path", rel_prefix.empty() ? "./*" : rel_prefix + "/*"},
                                    {"type", "note"},
                                    {"omitted", omitted}});
    }

    // aggregate 模式：每个目录一个条目（文件数/子目录数/字节量）——数据集类目录
    // 的骨架视图：大量同构文件折叠成计数，结构一目了然
    void walk_agg(const fs::path& dir, const std::string& rel_prefix, int depth) {
        if (depth <= 0 || listed_ >= kListEntriesCap) {
            truncated_ = truncated_ || listed_ >= kListEntriesCap;
            return;
        }
        long long files = 0, bytes = 0;
        int subdirs = 0;
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            std::error_code se;
            if (it->is_symlink(se) && !se) continue;
            const bool is_dir = it->is_directory(se) && !se;
            const std::string name =
                nostos::kit::win::wide_to_utf8(it->path().filename().c_str());
            const std::string child_rel =
                rel_prefix.empty() ? name : rel_prefix + "/" + name;
            if (is_dir) {
                ++subdirs;
                walk_agg(it->path(), child_rel, depth - 1);
                continue;
            }
            if (!pattern_.empty() && !glob_match_ci(pattern_, name)) continue;
            std::error_code ze;
            bytes += static_cast<long long>(it->file_size(ze));
            ++files;
        }
        if (ec) truncated_ = true;
        entries_.push_back(json{{"path", rel_prefix.empty() ? "." : rel_prefix},
                                {"type", "dir"},
                                {"files", files},
                                {"subdirs", subdirs},
                                {"bytes", bytes}});
        ++listed_;
        if (listed_ >= kListEntriesCap) truncated_ = true;
    }

    json search_tool(const json& args) {
        const std::string query = args.value("query", "");
        if (query.empty()) return fail("query 为空");
        const bool use_re = args.value("regex", false);
        const bool icase = args.value("ignore_case", false);
        nostos::kit::Regex re;
        std::string lit = query;
        if (use_re) {
            std::string re_err;
            if (!re.compile(query, {.icase = icase}, &re_err))
                return fail("正则非法: " + re_err);
        } else if (icase) {
            lit = ascii_lower(query);   // 小写副本上查找——与原串 1:1 映射，位置直用
        }
        const int max_hits =
            std::min(std::max(args.value("max_hits", static_cast<int>(kSearchHitCap)), 1), 200);
        hits_ = json::array();
        scanned_ = 0;
        hit_count_ = 0;
        truncated_ = false;
        stop_ = false;
        walk_search(jail_->root(), "", static_cast<int>(kSearchDepthCap), max_hits,
                    use_re, &re, lit, icase && !use_re);
        return json{{"ok", true},
                    {"hits", hits_},
                    {"scanned", scanned_},
                    {"truncated", truncated_}};
    }

    // 命中迭代：regex（有界 NFA，icase 内建于编译选项）或字面子串（icase 走小写
    // 副本）。cb 返回 false = 停止迭代（命中上限/停止旗）
    void each_hit(const std::string& content, bool use_re, const nostos::kit::Regex* re,
                  const std::string& lit, const std::function<bool(std::size_t)>& cb) const {
        if (use_re) {
            re->find_all(content,
                         [&](std::size_t b, std::size_t) { return cb(b); });
            return;
        }
        for (std::size_t pos = content.find(lit); pos != std::string::npos;
             pos = content.find(lit, pos + lit.size()))
            if (!cb(pos)) break;
    }

    void walk_search(const fs::path& dir, const std::string& rel_prefix, int depth,
                     int max_hits, bool use_re, const nostos::kit::Regex* re,
                     const std::string& lit, bool lower_hay) {
        if (stop_ || depth <= 0) {
            if (depth <= 0) truncated_ = true;
            return;
        }
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end && !stop_; it.increment(ec)) {
            if (ec) break;
            std::error_code se;
            if (it->is_symlink(se) && !se) continue;
            const bool is_dir = it->is_directory(se) && !se;
            const std::string child_rel =
                rel_prefix.empty() ? nostos::kit::win::wide_to_utf8(it->path().filename().c_str())
                                   : rel_prefix + "/" +
                                         nostos::kit::win::wide_to_utf8(it->path().filename().c_str());
            if (is_dir) {
                walk_search(it->path(), child_rel, depth - 1, max_hits, use_re, re,
                            lit, lower_hay);
                continue;
            }
            if (++scanned_ > kSearchFilesCap) {
                truncated_ = true;
                stop_ = true;
                return;
            }
            std::error_code ze;
            if (it->file_size(ze) > 1024 * 1024 || ze) continue;  // 大文件跳过
            std::string content;
            if (!store_->read(it->path().wstring(), content)) continue;
            if (content.find('\0') != std::string::npos) continue;  // 二进制跳过
            // icase 字面匹配在「小写副本」上做——副本与原串 1:1 字符映射，命中
            // 位置可直接用于原串取行；regex 的 icase 内建于标志，匹配原串
            std::string lower_buf;
            const std::string& hay =
                lower_hay ? (lower_buf = ascii_lower(content)) : content;
            each_hit(hay, use_re, re, lit, [&](std::size_t pos) {
                std::size_t line_start = content.rfind('\n', pos);
                line_start = line_start == std::string::npos ? 0 : line_start + 1;
                std::size_t line_end = content.find('\n', pos);
                if (line_end == std::string::npos) line_end = content.size();
                std::string line_text = content.substr(line_start, line_end - line_start);
                if (!line_text.empty() && line_text.back() == '\r') line_text.pop_back();
                const int line_no =
                    1 + static_cast<int>(std::count(content.begin(), content.begin() + static_cast<long>(pos), '\n'));
                hits_.push_back(json{{"path", child_rel},
                                     {"line", line_no},
                                     {"text", vcws::trim_utf8(line_text, 200)}});
                ++hit_count_;
                return hit_count_ < max_hits;
            });
            if (hit_count_ >= max_hits) {
                stop_ = true;      // 命中满即停
                truncated_ = true; // "可能还有更多"以 truncated 标注
            }
        }
        if (ec) truncated_ = true;
    }

    json apply_tool(const json& args, int turn_id) {
        std::vector<vcws::EditOp> ops;
        std::vector<vcws::OpError> parse_errors;
        vcws::parse_ops(args.value("ops", json::array()), ops, parse_errors);
        if (!parse_errors.empty()) return fail_json("op 解析失败", parse_errors);

        const bool dry = args.value("dry_run", false);
        const vcws::ApplyResult r = vcws::apply_ops(*jail_, *store_, ops, dry);

        json files = json::array();
        for (const auto& e : r.edits) {
            json f{{"path", e.path},
                   {"is_new", e.is_new},
                   {"edits", e.ops_count}};
            // diff 双通道：dry_run 是决策依据，落盘回执是操作自述（高分辨率）——
            // 两条路都回传 hunk，模型无需回读或 dry_run 二趟即可核对落点与内容。
            // "模型已知自己写了什么"不足以省略回执：find 落点、复合编辑的逐 op
            // 叠加结果都可能与意图有偏差。8KB 护栏防单文件体积失控；diff 同时经
            // applied 事件展示给用户
            std::string diff = e.diff;
            if (diff.size() > kDiffShowCap) diff = vcws::trim_utf8(diff, kDiffShowCap) + "\n（diff 截断）";
            f["diff"] = diff;
            if (!dry) f["delta"] = diff_delta(e.diff);   // 落盘量级（+A −B）——[tool] 卡与模型的锚点
            files.push_back(std::move(f));
        }
        json j{{"ok", r.ok}, {"dry_run", dry}, {"rolled_back", r.rolled_back}, {"turn_id", turn_id}};
        j["files"] = files;
        if (r.ok && !dry)
            j["note"] = "已落盘；diff 正文随结果回传，applied 事件同步展示给用户";
        if (!r.errors.empty()) j["errors"] = fail_json_errors(r.errors);
        if (r.ok && !dry) {
            std::vector<vcws::FileSnap> snaps;
            snaps.reserve(r.edits.size());
            for (const auto& e : r.edits)
                snaps.push_back(vcws::FileSnap{e.path, e.full, e.before, e.after, e.is_new});
            snaps_.add(turn_id, std::move(snaps));
            j["snapshot_depth"] = snaps_.depth();
            // vc.edit.applied：真实落盘后广播（修改即事实，diff 是报告）。
            // diff_text 拼装含 "── path（N 处编辑）" 文件头 + hunk 集；载荷必须用**具名
            // 缓冲**——emit 同步派发，paths.dump() 这类语句级临时一悬垂就是 UAF
            json paths = json::array();
            json files_meta = json::array();   // 每文件结构化统计（随事件下发，前端免反解展示文本）
            std::string diff_text;
            for (const auto& e : r.edits) {
                paths.push_back(e.path);  // UTF-8 相对路径；push e.full（wstring）会变码元数组
                files_meta.push_back(json{{"path", e.path},
                                          {"add", e.add},
                                          {"rem", e.rem},
                                          {"is_new", e.is_new}});
                std::string d = e.diff;
                if (d.size() > kDiffShowCap) d = vcws::trim_utf8(d, kDiffShowCap) + "\n（diff 截断）";
                diff_text += "── " + e.path;
                if (e.ops_count > 1) diff_text += "（" + std::to_string(e.ops_count) + " 处编辑）";
                diff_text += "\n" + d;
            }
            const std::string paths_json = paths.dump();
            const std::string files_json = files_meta.dump();
            const vc_api::EditApplied ap{turn_id, paths_json.c_str(), diff_text.c_str(),
                                         files_json.c_str()};
            host_.emit(vc_api::edit_applied, &ap);
        }
        return j;
    }

    json undo_tool(const json& args) {
        const bool has_turn = args.contains("turn_id");
        const vcws::TurnGroup* group =
            has_turn ? snaps_.find(args.value("turn_id", 0)) : snaps_.peek_latest();
        if (group == nullptr)
            return json{{"ok", false},
                        {"error", "没有可撤销的 turn（栈深 " + std::to_string(snaps_.depth()) + "）"}};
        const int turn_id = group->turn_id;  // 先拷贝：pop 之后 group 指针失效（ASan 捕获过）

        // 两段式：先全组只读裁决——任一冲突（外部改过 / 被删）⇒ 整组零改动并点名。
        json conflicts = json::array();
        for (const auto& f : group->files) {
            std::string err;
            if (!vcws::undo_check(*store_, f, err)) conflicts.push_back(err);
        }
        if (!conflicts.empty())
            return json{{"ok", false},
                        {"error", "撤销被拒：turn #" + std::to_string(turn_id) +
                                      " 存在外部改动（陈旧检测）"},
                        {"conflicts", conflicts}};

        json restored = json::array();
        for (auto it = group->files.rbegin(); it != group->files.rend(); ++it) {
            std::string err;
            if (!vcws::undo_write(*store_, *it, err))
                return json{{"ok", false}, {"error", err}};  // 裁决通过后失败极罕见；如实上报
            restored.push_back(it->path);
        }
        if (has_turn) snaps_.pop_turn(turn_id);
        else snaps_.pop_latest();
        // 撤销结果必须广播——EditUndone 是 tui/webui「已撤销」卡的唯一事件源
        // （踩过：契约事件曾长期无生产者，撤销后 UI 无终态）
        const vc_api::EditUndone ev{turn_id, static_cast<int>(snaps_.depth())};
        host_.emit(vc_api::edit_undone, &ev);
        return json{{"ok", true},
                    {"turn_id", turn_id},
                    {"restored", restored},
                    {"snapshot_depth", snaps_.depth()}};
    }

    // ---- 小件 -------------------------------------------------------------------

    bool resolve_arg(const std::string& rel, std::wstring& full, std::string& norm,
                     std::string& err) {
        if (!jail_->resolve(rel, full, norm, err)) {
            err = "路径被拒: " + err;
            return false;
        }
        return true;
    }

    static json fail(const std::string& error) { return json{{"ok", false}, {"error", error}}; }

    static json fail_json(const std::string& error, const std::vector<vcws::OpError>& errors) {
        json j{{"ok", false}, {"error", error}};
        json arr = json::array();
        for (const auto& e : errors)
            arr.push_back(json{{"index", e.index},
                               {"path", e.path},
                               {"message", e.message}});
        j["errors"] = arr;
        return j;
    }

    static json fail_json_errors(const std::vector<vcws::OpError>& errors) {
        json arr = json::array();
        for (const auto& e : errors)
            arr.push_back(json{{"index", e.index}, {"path", e.path}, {"message", e.message}});
        return arr;
    }

    // ---- 状态 ---------------------------------------------------------------------

    nostos::abi::Host host_{};
    std::unique_ptr<vcws::Jail> jail_;
    std::unique_ptr<vcws::TextStore> store_;  // DiskStore；单测走 MemoryStore 的纯逻辑头
    vcws::SnapshotStack snaps_;
    std::vector<nostos::abi::Host::Subscription> subs_;  // 工作区热切订阅
    vc_api::ToolTable table_{};
    std::mutex tool_mu_;   // 全工具串行锁（webui HTTP 线程与 agent 双线程直调，见 tool_execute）
    std::string out_buf_;  // execute 出参的存活缓冲（调用期有效纪律）

    // 遍历的产出缓冲（list/search 共用模式：成员缓冲 + 标志）
    json entries_ = json::array();
    json hits_ = json::array();
    std::size_t listed_ = 0;
    std::size_t scanned_ = 0;
    std::size_t hit_count_ = 0;
    bool truncated_ = false;
    bool stop_ = false;
    // list 的过滤/形态参数（list_tool 每次调用重置）
    std::string pattern_;      // 文件名通配（已小写；空 = 不过滤）
    bool aggregate_ = false;   // true = 目录聚合视图（默认；mode=flat 显式平铺）
    int per_dir_ = 0;          // 单目录文件条目上限（0 = 不限）
};

}  // namespace

NOSTOS_PLUGIN(WorkspacePlugin)
