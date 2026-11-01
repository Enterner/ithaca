#pragma once
// 预览转换引擎（plugin/office 的版式级预览源）：办公文档 → PDF（COM 自动化，
// late binding）+ 版本键缓存。webui 的 /api/fs/preview 经 PreviewTable
//（shared/vcoffice/iface.hpp）到这里；agent 工具面不经过本文件——预览是 UI
// 能力不是模型能力。
//
// 线程纪律（本插件的头两个自有线程，成对启停）：
//   · 转换线程（STA 常驻）：一次 CoInitializeEx(APARTMENTTHREADED)，FIFO 逐 job
//     转换——COM 自动化恒串行（并发双 Word 自动化互相打架，也省一份进程开销）。
//   · 看门狗线程：单守护循环。job 起飞记「本方 Office PID」，30s 未收口 ⇒
//     TerminateProcess 仅本方 PID ⇒ 阻塞中的 COM 调用随进程死立即失败返回，
//     转换线程自然收尾，无需强杀线程。PID 用快照差集定位（CoCreateInstance 恒
//     新起进程；GetActiveObject 才挂运行实例）。按映像名杀是禁手——会误伤用户
//     自己开着的 Word。差集为空（挂接异常）时不杀：宁可挂一个 job 也不冒险。
//   · 生命周期：stop() 置停机 ⇒ 看门狗杀在途 job 解锁转换线程 ⇒ 队列余量逐个
//     以取消回调清空 ⇒ 双 join。回调（消费方经 PreviewTable 提供）必须可随时
//     调用且恰好一次——四路收口（成功/失败/取消/杀进程后失败）同走 complete_。
//
// 宏安全（底线，缺一即可能执行文档宏或被模态框挂死）：
//   AutomationSecurity=3（msoAutomationSecurityForceDisable 宏禁用）+ 只读打开
//   + Visible=false（Word）/ WithWindow=false（PPT——它不吃 Visible=false，
//   置了反而抛错）+ DisplayAlerts=0。
//
// 缓存：%TEMP%/ithaca/preview/<fnv1a64(root|norm)>_<mtime>_<size>.pdf。系统
// 临时目录：不进工作区（fs watcher 零打扰）、多实例天然共享、可再生丢失无碍；
// 文件名纯数字（键是哈希+stat）⇒ 全 ASCII，绕开 fs::path 窄串构造的 ANSI 码页
// 陷阱。键盐含工作区根——不同工作区的同名相对路径不互撞；键含 mtime+size——
// 文件改过即自然失效。LRU：总量超上限按 mtime 最旧先删（转换收尾顺带做，无
// 独立线程；多实例并发清理时 remove 失败静默）。
//
// 依赖：COM 后期绑定（IDispatch）——#import 类型库需要构建机装 Office，不可
// 接受；官方只发类型库不发 C++ 头，late binding 是零依赖自动化的标准路。
#include "vcoffice/iface.hpp"  // vcoffice::PreviewDoneFn：回调签名单源

#include <nostos/kit/win.hpp>  // nostos::kit::win：UTF-8 ↔ 宽字符（全仓唯一实现）
#include <nostos/nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>  // CreateToolhelp32Snapshot：本方 Office 进程的快照差集

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace vcoffice {

using nlohmann::json;   // job 出参/错误 JSON（与 plugin.cpp 同一 vendored 版本）

class Preview {
public:
    struct Job {
        std::string root;      // 工作区根（UTF-8；缓存键盐）
        std::string norm;      // 相对路径（UTF-8；缓存键 + 错误信息）
        std::wstring full;     // 绝对路径（打开用）
        std::string ext;       // 折叠小写扩展名（.docx/.doc/.pptx/.ppt/.pdf）
        PreviewDoneFn done = nullptr;
        void* ud = nullptr;
    };

    Preview() = default;
    ~Preview() { stop(); }
    Preview(const Preview&) = delete;
    Preview& operator=(const Preview&) = delete;

    void start(std::function<void(const std::string&)> log) {
        log_ = std::move(log);
        wd_ = std::thread([this] { watchdog_loop_(); });
        worker_ = std::thread([this] { worker_loop_(); });
        if (log_) log_("ithaca.office: 预览转换线程已启动");
    }

    /** 停机（deactivate 调用）：在途 job 由看门狗杀进程解锁收尾，队列余量逐个
        以取消回调清空，双 join 后返回。回调期间消费方不得要求插件状态（其自身
        纪律）；本类内部状态自洽，重复调用安全（joinable 已空即跳过）。 */
    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        try {
            if (worker_.joinable()) worker_.join();
        } catch (...) {
        }
        try {
            if (wd_.joinable()) wd_.join();
        } catch (...) {
        }
    }

    /** 入队（调用线程 = webui 宿主排水闭包，天然串行）。返回 false = 未入队
        （out 填 ok:false JSON，on_done 不会来）；true = 已入队（out = pending，
        结果恰一次经 on_done 回调）。 */
    bool submit(Job&& job, std::string& out) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stopping_) {
                out = err_json_("预览服务已停止");
                return false;
            }
            if (q_.size() >= kQueueCap) {
                out = err_json_("预览转换排队已满，请稍后再试");
                return false;
            }
            q_.push_back(std::move(job));
        }
        // NOTE: 必须 notify_all——worker（队列谓词）与看门狗（在途谓词）同眠于
        // 这一把 cv，notify_one 抽中看门狗时其谓词为假、原样回睡，唤醒被白吃，
        // worker 永不醒（探针实测：入队+notify 后 job 在队列里躺到天荒地老）
        cv_.notify_all();
        out = (json{{"ok", true}, {"pending", true}}).dump();
        return true;
    }

private:
    class Disp;   // 前置：word/ppt_export_ 的签名引用它（完整定义见 IDispatch 小节）
                  // ——成员函数体内可用后置嵌套类，形参类型不行（类在此处尚不完整）

    static constexpr std::size_t kQueueCap = 4;   // 队列上限（对齐 webui 挂起连接预算）
    static constexpr std::uintmax_t kCacheCapBytes = 200ull * 1024 * 1024;
    static constexpr auto kConvertTimeout = std::chrono::seconds{30};

    // ---- 线程主体 ---------------------------------------------------------------

    void worker_loop_() {
        // STA 常驻：Word/PPT 自动化对象要求 STA；初始化失败 = 进程级 COM 残废，
        // 之后所有 job 点名失败（不重试——环境坏了不会自愈）
        const HRESULT hr_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool com = SUCCEEDED(hr_init);
        if (!com && log_)
            log_("ithaca.office: 预览线程 COM 初始化失败 (0x" + hex_(hr_init) + ")");
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return stopping_ || !q_.empty(); });
                if (q_.empty()) break;   // stopping_ 且队空 ⇒ 收工
                job = std::move(q_.front());
                q_.pop_front();
            }
            if (stopping_) {
                complete_(job, {{"ok", false}, {"error", "预览已取消（插件停用中）"}});
                continue;
            }
            if (log_) log_("ithaca.office: 预览转换开始: " + job.norm);
            run_job_(job, com);
        }
        if (com) CoUninitialize();
    }

    void watchdog_loop_() {
        std::unique_lock<std::mutex> lk(mu_);
        for (;;) {
            cv_.wait(lk, [&] { return stopping_ || flight_pid_ != 0; });
            if (stopping_ && flight_pid_ == 0) return;
            // 在途：等收口（flight_pid_ 归零 = 完成）或超时/停机 ⇒ 杀进程解锁
            cv_.wait_for(lk, kConvertTimeout,
                         [&] { return stopping_ || flight_pid_ == 0; });
            if (flight_pid_ == 0) continue;
            const unsigned long pid = flight_pid_;
            lk.unlock();
            kill_pid_(pid);
            lk.lock();
            // 杀后 COM 调用应即失败返回、转换线程归零 flight_pid_；等它落地再续
            cv_.wait(lk, [&] { return flight_pid_ == 0; });
        }
    }

    // ---- 单 job 执行（job 级异常隔离：转换线程不许因单个 job 坠毁）---------------

    void run_job_(const Job& job, bool com) {
        std::string cache;   // 提到 try 外：异常收口要拿它清残骸
        try {
            // 缓存键在执行时刻现取 stat：提交到执行之间文件可能已变化，以执行
            // 时版本为准（键含 mtime+size ⇒ 改过的文件自然换键）
            auto [path, cerr] = cache_path_(job);
            cache = std::move(path);
            if (!cerr.empty()) {
                complete_(job, {{"ok", false}, {"error", cerr}});
                return;   // 缓存面不可用：无产物可清
            }
            std::error_code ec;
            if (fs::is_regular_file(cache, ec)) {
                complete_(job, {{"ok", true}, {"pdf", cache}});
                return;   // 命中：无新文件入缓存，LRU 无须重算
            }
            const std::string err = !com ? "预览线程 COM 初始化失败，无法转换"
                                         : job.ext == ".pdf" ? pdf_passthrough_(job, cache)
                                                             : office_convert_(job, cache);
            // 失败即清残骸（杀进程可留半截 PDF——不清会被同键误当缓存命中）
            if (!err.empty()) fs::remove(cache, ec);
            if (log_)
                log_("ithaca.office: 预览转换" +
                     std::string(err.empty() ? "完成: " : "失败: ") + job.norm +
                     (err.empty() ? "" : "（" + err + "）"));
            if (err.empty()) complete_(job, {{"ok", true}, {"pdf", cache}});
            else complete_(job, {{"ok", false}, {"error", err}});
        } catch (const std::exception& e) {
            job_fail_(job, cache, std::string("预览内部错误: ") + e.what());
        } catch (...) {
            job_fail_(job, cache, "预览内部错误（未知异常）");
        }
        lru_trim_();
    }

    // 异常收口：清在途 PID + 清半截产物（残骸会被同键误当缓存命中）+ 错误回调
    void job_fail_(const Job& job, const std::string& cache, std::string err) {
        clear_flight_();
        std::error_code ec;
        if (!cache.empty()) fs::remove(cache, ec);
        complete_(job, {{"ok", false}, {"error", err}});
    }

    // 缓存路径：<dir>/<fnv1a64(root|norm)>_<mtime>_<size>.pdf；目录创建/stat 失败
    // 经 second 点名（缓存面不可用时预览不可用，不静默回源盘）
    std::pair<std::string, std::string> cache_path_(const Job& job) {
        std::error_code ec;
        const fs::path dir = cache_dir_();
        fs::create_directories(dir, ec);
        if (ec) return {{}, "预览缓存目录创建失败: " + dir.string()};
        const auto mtime = fs::last_write_time(job.full, ec).time_since_epoch().count();
        if (ec) return {{}, "文档不可读（stat 失败）: " + job.norm};
        const auto sz = fs::file_size(job.full, ec);
        if (ec) return {{}, "文档不可读（stat 失败）: " + job.norm};
        const std::string name = std::to_string(fnv1a64_(job.root + "\n" + job.norm)) + "_" +
                                 std::to_string(mtime) + "_" + std::to_string(sz) + ".pdf";
        return {wide_to_utf8_((dir / name).wstring()), {}};
    }

    // .pdf 直通：源文件即产物（拷进缓存统一服务面——webui 只认缓存路径不回源盘，
    // jail 内外的字节边界因此单一）
    std::string pdf_passthrough_(const Job& job, const std::string& cache) {
        std::error_code ec;
        fs::copy_file(job.full, fs::path(cache), fs::copy_options::overwrite_existing, ec);
        return ec ? "PDF 复制失败: " + ec.message() : std::string{};
    }

    // ---- COM 转换（late binding，STA 线程上执行）---------------------------------

    std::string office_convert_(const Job& job, const std::string& cache) {
        const bool word = job.ext == ".docx" || job.ext == ".doc";
        const wchar_t* progid = word ? L"Word.Application" : L"PowerPoint.Application";
        const wchar_t* exe = word ? L"winword.exe" : L"powerpnt.exe";
        const char* appname = word ? "Word" : "PowerPoint";
        // 快照差集定位本方进程（必须在 CoCreateInstance 之前取存量）
        const std::vector<unsigned long> before = snapshot_pids_(exe);
        CLSID clsid{};
        HRESULT hr = CLSIDFromProgID(progid, &clsid);
        if (FAILED(hr))
            return std::string(appname) + " 不可用（COM 类未注册，本机大概率未安装 Office）";
        IDispatch* raw = nullptr;
        hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_IDispatch,
                              reinterpret_cast<void**>(&raw));
        if (FAILED(hr) || !raw)
            return std::string(appname) + " 启动失败 (0x" + hex_(hr) + ")";
        // 新 PID 上架看门狗（差集空 = 挂接异常：不杀不看门，宁可挂一个 job）
        {
            unsigned long pid = 0;
            for (unsigned long p : snapshot_pids_(exe))
                if (std::find(before.begin(), before.end(), p) == before.end()) pid = p;
            std::lock_guard<std::mutex> lk(mu_);
            flight_pid_ = pid;
            cv_.notify_all();
        }
        Disp app(raw);
        const std::string err =
            word ? word_export_(app, job, cache) : ppt_export_(app, job, cache);
        clear_flight_();   // 收口（成败都算）：看门狗放行
        return err;
    }

    // Word：Open(只读) → SaveAs2(pdf, wdFormatPDF=17)，老版本退 SaveAs 同参
    std::string word_export_(Disp& app, const Job& job, const std::string& cache) {
        app.put(L"DisplayAlerts", Vi_(0));          // wdAlertsNone
        app.put(L"AutomationSecurity", Vi_(3));     // msoAutomationSecurityForceDisable
        app.put(L"Visible", Vb_(false));
        const Disp docs = app.get(L"Documents");
        if (!docs) return "Word 自动化：取 Documents 失败";
        const Disp doc = docs.open_named(
            L"Open", {L"FileName", L"ReadOnly", L"AddToRecentFiles", L"Visible"},
            {Vs_(job.full.c_str()), Vb_(true), Vb_(false), Vb_(false)});
        if (!doc) return "Word 打开文档失败: " + job.norm;
        // call() 拥有并清空入参 ⇒ 重试路径每次重建 BSTR（复用即 use-after-free）
        const std::wstring wcache = utf8_to_wide_(cache);
        HRESULT hr = doc.call(L"SaveAs2", {Vs_(wcache.c_str()), Vi_(17)});
        if (FAILED(hr)) hr = doc.call(L"SaveAs", {Vs_(wcache.c_str()), Vi_(17)});
        std::string err;
        if (FAILED(hr))
            err = "Word 导出 PDF 失败 (0x" + hex_(hr) + ")";
        doc.call(L"Close", {Vi_(0)});   // wdDoNotSaveChanges：预览绝不写回源文件
        app.call(L"Quit", {Vi_(0)});
        return err;
    }

    // PowerPoint：Open(只读、无窗口) → SaveAs(pdf, ppSaveAsPDF=32)
    std::string ppt_export_(Disp& app, const Job& job, const std::string& cache) {
        app.put(L"DisplayAlerts", Vi_(2));          // ppAlertsNone
        app.put(L"AutomationSecurity", Vi_(3));     // msoAutomationSecurityForceDisable
        // NOTE: 不置 Visible=false——PowerPoint 对该属性抛错，无窗口由
        // WithWindow:=false 承担
        const Disp pres = app.get(L"Presentations");
        if (!pres) return "PowerPoint 自动化：取 Presentations 失败";
        const Disp doc = pres.open_named(
            L"Open", {L"FileName", L"ReadOnly", L"Untitled", L"WithWindow"},
            {Vs_(job.full.c_str()), Vb_(true), Vb_(true), Vb_(false)});
        if (!doc) return "PowerPoint 打开文档失败: " + job.norm;
        const HRESULT hr = doc.call(L"SaveAs", {Vs_(utf8_to_wide_(cache).c_str()), Vi_(32)});
        std::string err;
        if (FAILED(hr))
            err = "PowerPoint 导出 PDF 失败 (0x" + hex_(hr) + ")";
        doc.call(L"Close", {});
        app.call(L"Quit", {});
        return err;
    }

    // ---- 回调与缓存维护 -----------------------------------------------------------

    // 恰好一次回调（worker 线程）：result_json 只在回调期有效，消费方同步消费
    void complete_(const Job& job, json result) {
        if (!job.done) return;
        const std::string s = result.dump();
        job.done(job.ud, s.c_str());
    }

    void clear_flight_() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            flight_pid_ = 0;
        }
        cv_.notify_all();
    }

    // LRU：总量超上限按 mtime 最旧先删（单 worker 串行调用；多实例共享目录时
    // 并发 remove 失败静默——缓存是再生资源）
    void lru_trim_() {
        std::error_code ec;
        const fs::path dir = cache_dir_();
        struct Ent {
            fs::path p;
            std::uintmax_t sz;
            long long mt;
        };
        std::vector<Ent> ents;
        std::uintmax_t total = 0;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            const auto sz = it->file_size(e2);
            if (e2 || it->path().extension() != ".pdf") continue;
            const auto mt = it->last_write_time(e2).time_since_epoch().count();
            if (e2) continue;
            ents.push_back({it->path(), sz, static_cast<long long>(mt)});
            total += sz;
        }
        if (total <= kCacheCapBytes) return;
        std::sort(ents.begin(), ents.end(),
                  [](const Ent& a, const Ent& b) { return a.mt < b.mt; });
        for (const auto& e : ents) {
            if (total <= kCacheCapBytes) break;
            fs::remove(e.p, ec);
            total -= e.sz;
        }
    }

    static fs::path cache_dir_() {
        wchar_t tmp[MAX_PATH];
        // GetTempPathW 返回值含尾 null——fs::path(tmp) 按以零结尾读，不带进路径
        const DWORD n = GetTempPathW(MAX_PATH, tmp);
        return n > 0 && n < MAX_PATH ? fs::path(tmp) / "ithaca" / "preview"
                                     : fs::path("ithaca-preview");
    }

    // ---- 小件 ---------------------------------------------------------------------

    static std::string err_json_(const std::string& msg) {
        return (json{{"ok", false}, {"error", msg}}).dump();
    }

    static std::string hex_(long v) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%lX", static_cast<unsigned long>(v));
        return buf;
    }

    static std::uint64_t fnv1a64_(const std::string& s) {
        std::uint64_t h = 14695981039346656037ull;
        for (const unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }

    static std::string wide_to_utf8_(const std::wstring& w) {
        return nostos::kit::win::wide_to_utf8(w.c_str());
    }
    static std::wstring utf8_to_wide_(const std::string& s) {
        return nostos::kit::win::utf8_to_wide(s.c_str());
    }

    // 存量同名进程 PID 快照（工具链快照；映像名比较大小写不敏感——大小写随启动方）
    static std::vector<unsigned long> snapshot_pids_(const wchar_t* exe) {
        std::vector<unsigned long> pids;
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return pids;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (lstrcmpiW(pe.szExeFile, exe) == 0) pids.push_back(pe.th32ProcessID);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return pids;
    }

    static void kill_pid_(unsigned long pid) {
        const HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!h) return;   // 已自行退出 = 无事
        TerminateProcess(h, 1);
        CloseHandle(h);
    }

    static VARIANT Vi_(long v) {
        VARIANT r{};
        r.vt = VT_I4;
        r.lVal = v;
        return r;
    }
    static VARIANT Vb_(bool v) {
        VARIANT r{};
        r.vt = VT_BOOL;
        r.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
        return r;
    }
    static VARIANT Vs_(const wchar_t* s) {
        VARIANT r{};
        r.vt = VT_BSTR;
        r.bstrVal = SysAllocString(s);
        return r;
    }

    // ---- IDispatch 后期绑定小助手 -------------------------------------------------
    // 引用计数纪律：构造接管引用（CoCreateInstance/Open 的出参引用），析构 Release；
    // invoke 的入参 VARIANT 由调用点构造、本类不清——word/ppt_export_ 里 BSTR 参
    // 需跨两次调用复用（SaveAs2 退 SaveAs），故清点放调用侧，不藏进助手里。
    class Disp {
    public:
        Disp() = default;
        explicit Disp(IDispatch* d) : d_(d) {}
        ~Disp() {
            if (d_) d_->Release();
        }
        Disp(Disp&& o) noexcept : d_(o.d_) { o.d_ = nullptr; }
        Disp(const Disp&) = delete;
        Disp& operator=(const Disp&) = delete;
        explicit operator bool() const { return d_ != nullptr; }

        // 属性读（出参 DISPATCH 引用接管进 Disp；非对象/失败 = 空）
        Disp get(const wchar_t* name) const {
            HRESULT hr = S_OK;
            VARIANT v = invoke_(name, DISPATCH_PROPERTYGET, nullptr, 0, nullptr, 0, &hr);
            if (v.vt != VT_DISPATCH || !v.pdispVal) {
                VariantClear(&v);
                return Disp();
            }
            IDispatch* d = v.pdispVal;
            v.vt = VT_EMPTY;   // 所有权已移交，VariantClear 不再释放
            VariantClear(&v);
            return Disp(d);
        }
        // 属性写（PROPERTYPUT 必须带 DISPID_PROPERTYPUT 具名实参——Invoke 契约；
        // 失败静默：DISPLAYALERTS 之类的装饰属性不值得为它建错误路）
        void put(const wchar_t* name, VARIANT v) const {
            const DISPID did = resolve_(name);
            if (did != DISPID_UNKNOWN) {
                DISPID prop[] = {DISPID_PROPERTYPUT};
                DISPPARAMS dp{&v, prop, 1, 1};
                d_->Invoke(did, IID_NULL, 0, DISPATCH_PROPERTYPUT, &dp, nullptr, nullptr,
                           nullptr);
            }
            VariantClear(&v);
        }
        // 位置参数方法调用（args 自然序传入，内部翻转为 DISPPARAMS 的逆序；入参
        // 由本调用清——含 SaveAs 失败重试不复用入参的调用形态）
        HRESULT call(const wchar_t* name, std::vector<VARIANT> args) const {
            std::reverse(args.begin(), args.end());
            HRESULT hr = S_OK;
            VARIANT v = invoke_(name, DISPATCH_METHOD, args.data(),
                                static_cast<UINT>(args.size()), nullptr, 0, &hr);
            VariantClear(&v);
            for (VARIANT& a : args) VariantClear(&a);
            return hr;
        }
        // 具名实参方法调用（pnames[0] = 方法名，其余为参数名；具名实参的 rgvarg
        // 与 rgdispidNamedArgs 一一对应，不翻转）。出参为对象 ⇒ 接管进 Disp，
        // 否则返回空
        Disp open_named(const wchar_t* method, const std::vector<const wchar_t*>& pnames,
                        std::vector<VARIANT> vals) const {
            std::vector<DISPID> ids(pnames.size() + 1, DISPID_UNKNOWN);
            std::vector<LPOLESTR> names;
            names.push_back(const_cast<LPOLESTR>(method));
            for (const wchar_t* p : pnames) names.push_back(const_cast<LPOLESTR>(p));
            if (FAILED(d_->GetIDsOfNames(IID_NULL, names.data(),
                                         static_cast<UINT>(names.size()), 0, ids.data()))) {
                for (VARIANT& a : vals) VariantClear(&a);
                return Disp();
            }
            std::vector<DISPID> named(ids.begin() + 1, ids.end());
            DISPPARAMS dp{vals.data(), named.data(), static_cast<UINT>(vals.size()),
                          static_cast<UINT>(named.size())};
            VARIANT res{};
            const HRESULT hr =
                d_->Invoke(ids[0], IID_NULL, 0, DISPATCH_METHOD, &dp, &res, nullptr, nullptr);
            for (VARIANT& a : vals) VariantClear(&a);
            if (hr != S_OK || res.vt != VT_DISPATCH || !res.pdispVal) {
                VariantClear(&res);
                return Disp();
            }
            IDispatch* d = res.pdispVal;
            res.vt = VT_EMPTY;   // 所有权已移交，VariantClear 不再释放
            VariantClear(&res);
            return Disp(d);
        }

    private:
        DISPID resolve_(const wchar_t* name) const {
            DISPID did = DISPID_UNKNOWN;
            LPOLESTR n = const_cast<LPOLESTR>(name);
            if (d_ && FAILED(d_->GetIDsOfNames(IID_NULL, &n, 1, 0, &did)))
                return DISPID_UNKNOWN;
            return did;
        }
        VARIANT invoke_(const wchar_t* name, WORD flags, VARIANT* args, UINT cargs,
                        DISPID* named, UINT cnamed, HRESULT* hr_out) const {
            VARIANT res{};
            VariantInit(&res);
            const DISPID did = resolve_(name);
            HRESULT hr = did == DISPID_UNKNOWN ? DISP_E_UNKNOWNNAME : S_OK;
            if (SUCCEEDED(hr)) {
                DISPPARAMS dp{args, named, cargs, cnamed};
                hr = d_->Invoke(did, IID_NULL, 0, flags, &dp, &res, nullptr, nullptr);
            }
            if (hr_out) *hr_out = hr;
            return res;
        }
        IDispatch* d_ = nullptr;
    };

    // ---- 状态 ---------------------------------------------------------------------

    std::function<void(const std::string&)> log_;
    std::thread worker_;
    std::thread wd_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> q_;
    bool stopping_ = false;         // mu_ 守护：停机旗（deactivate 置位）
    unsigned long flight_pid_ = 0;  // mu_ 守护：在途 job 的本方 Office PID（0 = 无在途）
};

}  // namespace vcoffice
