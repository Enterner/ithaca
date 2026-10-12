#pragma once
// 管道化子进程：CreateProcessW + stdout/stderr 匿名管道 + 行分割。
// 与 kit::process（生命周期）互补——这里专注"输出捕获"，即 proposal-kit §5.6
// 规划的行事件形态。runner 自建（第二份实现出现前暂不收编 kit，见观察文档）。
//
// 设计约束（经典坑，逐条处理）：
//   · 父进程的管道写端在 spawn 后**立即关闭**——否则读端永远等不到 EOF；
//   · 读端不可被子进程继承（SetHandleInformation 去继承位）；
//   · 子进程退出后读端到达 EOF，读取线程自然结束。
// 输出按字节流读取、经 LineSplitter 分行（网络分块/管道块边界不保证对齐）。
#include <nostos/kit/win.hpp>

#include <objbase.h>
#include <aclapi.h>
#include <userenv.h>

#include <cwctype>
#include <filesystem>
#include <string>
#include <utility>

#if defined(_WIN32)
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vcrun {

// 行分割器：任意分块喂入，按 \n 分行（剥 \r），残余冲刷。
class LineSplitter {
public:
    template <typename F>
    void feed(const char* data, std::size_t n, F&& on_line) {
        buf_.append(data, n);
        std::size_t pos = 0;
        for (;;) {
            const std::size_t eol = buf_.find('\n', pos);
            if (eol == std::string::npos) break;
            std::string line = buf_.substr(pos, eol - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            on_line(line);
            pos = eol + 1;
        }
        buf_.erase(0, pos);
    }

    template <typename F>
    void flush(F&& on_line) {
        if (buf_.empty()) return;
        std::string line = std::move(buf_);
        buf_.clear();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        on_line(line);
    }

private:
    std::string buf_;
};

struct PipePair {
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
};

inline bool make_pipe(PipePair& out, std::string& err) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&out.read_end, &out.write_end, &sa, 0)) {
        err = "CreatePipe 失败 (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    SetHandleInformation(out.read_end, HANDLE_FLAG_INHERIT, 0);  // 读端不继承
    return true;
}

struct ChildPipes {
    HANDLE process = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE job = nullptr;  // 进程树容器（KILL_ON_JOB_CLOSE）：句柄关闭即整树终止

    ChildPipes() = default;
    ChildPipes(const ChildPipes&) = delete;
    ChildPipes& operator=(const ChildPipes&) = delete;
    ~ChildPipes() {
        if (process != nullptr) CloseHandle(process);
        if (stdout_read != nullptr) CloseHandle(stdout_read);
        if (stderr_read != nullptr) CloseHandle(stderr_read);
        if (job != nullptr) CloseHandle(job);
    }
};

// ---- 沙箱（full-auto 模式：AppContainer 进程）--------------------------------
//   · 断网：容器默认无 INTERNET_CLIENT 能力 ⇒ 出站网络（含回环）内核级拒绝

//   · 注册表/用户配置：仅容器自己的 profile 可写（个别工具可能受限——文档明示）
//   · profile 进程级缓存一份（CreateAppContainerProfile 幂等开销大）

struct SandboxProfile {
    PSID sid = nullptr;         // LocalFree 归还（缓存持有，进程退出统一释放）
    std::wstring temp_dir;      // 容器可写临时目录（覆盖子进程 TEMP/TMP/LOCALAPPDATA）
};

inline std::string& sandbox_profile_err() {
    static std::string e;
    return e;
}

inline bool sandbox_ensure_profile(SandboxProfile& out, std::string& err) {
    static SandboxProfile cached;
    static bool have = false;
    if (have) {
        out = cached;
        if (cached.sid == nullptr) err = sandbox_profile_err();
        return cached.sid != nullptr;
    }
    have = true;
    HRESULT hr = ::CreateAppContainerProfile(L"ithaca_sandbox", L"ithaca sandbox",
                                             L"ithaca full-auto sandbox", nullptr, 0,
                                             &cached.sid);
    if (FAILED(hr)) {
        // 残留 profile（上次异常退出/权限不一致）会挡住重建——删掉重试一次（自愈）
        ::DeleteAppContainerProfile(L"ithaca_sandbox");
        hr = ::CreateAppContainerProfile(L"ithaca_sandbox", L"ithaca sandbox",
                                         L"ithaca full-auto sandbox", nullptr, 0,
                                         &cached.sid);
    }
    if (FAILED(hr) || cached.sid == nullptr) {
        sandbox_profile_err() = "CreateAppContainerProfile 失败 (0x" + [] (HRESULT h) {
            char b[16];
            std::snprintf(b, sizeof b, "%lx", static_cast<unsigned long>(h));
            return std::string(b);
        }(hr) + ")";
        err = sandbox_profile_err();
        return false;
    }
    PWSTR path = nullptr;
    if (SUCCEEDED(::GetAppContainerFolderPath(L"ithaca_sandbox", &path)) && path != nullptr) {
        std::error_code ec;
        cached.temp_dir = std::wstring(path) + L"\\AC\\Temp";
        std::filesystem::create_directories(cached.temp_dir, ec);
        ::CoTaskMemFree(path);
    }
    out = cached;
    return true;
}

// 给目录授予容器 SID 的可继承 ACE（SetNamedSecurityInfo 触发自动继承传播到既有
// 子树；新文件随建随继承）。write=false = 只读+执行（PATH 工具目录用）。
// 幂等：已含该 SID 的 Allow ACE 且权限足够 ⇒ 跳过；权限不足（先只读、后要写）
// ⇒ 追加更宽 ACE——Windows 对同 SID 的多条 Allow 取并集，追加即升级。
inline bool sandbox_grant_dir(const std::wstring& dir, PSID sid, std::string& err,
                              bool write = true) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    if (::GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS ||
        dacl == nullptr) {
        err = "读取目录 DACL 失败: " + std::to_string(GetLastError());
        if (sd != nullptr) ::LocalFree(sd);
        return false;
    }
    ACL_SIZE_INFORMATION info{};
    if (!::GetAclInformation(dacl, &info, sizeof info, AclSizeInformation)) {
        err = "GetAclInformation 失败";
        ::LocalFree(sd);
        return false;
    }
    const DWORD want = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE |
                       (write ? (FILE_GENERIC_WRITE | DELETE) : 0);
    bool sufficient = false;
    for (DWORD i = 0; i < info.AceCount && !sufficient; ++i) {
        LPVOID ace = nullptr;
        if (!::GetAce(dacl, i, &ace)) break;
        const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(ace);
        if (allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
            ::EqualSid((PSID)&allowed->SidStart, sid))
            sufficient = !write || (allowed->Mask & FILE_GENERIC_WRITE) != 0;
    }
    ::LocalFree(sd);
    if (sufficient) return true;

    const DWORD ace_size = static_cast<DWORD>(sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) +
                                              ::GetLengthSid(sid));
    const DWORD new_acl_size = sizeof(ACL) + info.AclBytesInUse + ace_size;
    PACL new_acl = static_cast<PACL>(::LocalAlloc(LMEM_FIXED, new_acl_size));
    if (new_acl == nullptr || !::InitializeAcl(new_acl, new_acl_size, ACL_REVISION)) {
        if (new_acl) ::LocalFree(new_acl);
        err = "初始化新 DACL 失败";
        return false;
    }
    for (DWORD i = 0; i < info.AceCount; ++i) {
        LPVOID ace = nullptr;
        if (!::GetAce(dacl, i, &ace)) break;
        const auto* h = static_cast<const ACE_HEADER*>(ace);
        if (!::AddAce(new_acl, ACL_REVISION, MAXDWORD, ace, h->AceSize)) {
            ::LocalFree(new_acl);
            err = "复制原 ACE 失败";
            return false;
        }
    }
    if (!::AddAccessAllowedAceEx(new_acl, ACL_REVISION,
                                 OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                                 want, sid)) {
        ::LocalFree(new_acl);
        err = "AddAccessAllowedAceEx 失败";
        return false;
    }
    const DWORD set_rc = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(dir.c_str()),
                                                 SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                                 nullptr, nullptr, new_acl, nullptr);
    ::LocalFree(new_acl);
    if (set_rc != ERROR_SUCCESS) {
        err = "SetNamedSecurityInfo 失败 (" + std::to_string(set_rc) + ")";
        return false;
    }
    return true;
}

// PATH 工具目录批量授予容器 SID「只读+执行」ACE（首次拉起沙箱时调用一轮）。
// 授权面锚定宿主环境事实——PATH 按定义就是"装工具的地方"，agent 无从操纵
// （请求驱动的授权会被注入面利用：诱导授权任意目录）。个别条目失败（目录已
// 卸载/所有者非本用户改不了 DACL）静默跳过：无害且症状可观测——真要跑那里
// 的命令时 CreateProcess 失败 → 逃生舱（plugin.cpp 访问拒绝结构化回灌）。
inline void sandbox_grant_path_dirs(PSID sid) {
    static bool done = false;   // ACE 落盘持久存在（幂等检查在 grant_dir 内），进程内只做一轮
    if (done || sid == nullptr) return;
    done = true;
    const DWORD n = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (n == 0) return;
    std::wstring path(n, L'\0');
    const DWORD got = ::GetEnvironmentVariableW(L"PATH", path.data(), n);
    if (got == 0 || got >= n) return;
    path.resize(got);
    std::string unused;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find(L';', start);
        if (end == std::wstring::npos) end = path.size();
        std::wstring dir = path.substr(start, end - start);
        start = end + 1;
        // 条目允许带引号/夹空白——清边角
        const auto is_ws = [](wchar_t c) { return c == L' ' || c == L'\t'; };
        while (!dir.empty() && is_ws(dir.front())) dir.erase(dir.begin());
        while (!dir.empty() && is_ws(dir.back())) dir.pop_back();
        if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"')
            dir = dir.substr(1, dir.size() - 2);
        if (dir.empty()) continue;
        wchar_t full[MAX_PATH * 4];
        if (::GetFullPathNameW(dir.c_str(), MAX_PATH * 4, full, nullptr) == 0) continue;
        const DWORD attr = ::GetFileAttributesW(full);
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
            continue;   // 不存在/不是目录：PATH 里的正常现象，跳过
        sandbox_grant_dir(full, sid, unused, /*write=*/false);
    }
}

// 子进程环境块：继承宿主环境，TEMP/TMP/LOCALAPPDATA 重定向到容器可写目录
inline bool sandbox_env_block(const std::wstring& temp_dir, std::wstring& out,
                              std::string& err) {
    LPWCH envs = ::GetEnvironmentStringsW();
    if (envs == nullptr) {
        err = "GetEnvironmentStrings 失败";
        return false;
    }
    for (LPWCH p = envs; *p != L'\0';) {
        const std::wstring e = p;
        p += e.size() + 1;
        if (e.empty()) break;
        const auto eq = e.find(L'=');
        if (eq != std::wstring::npos && eq > 0) {
            std::wstring key = e.substr(0, eq);
            for (wchar_t& c : key) c = static_cast<wchar_t>(towupper(c));
            if (key == L"TEMP" || key == L"TMP" || key == L"LOCALAPPDATA") continue;
        }
        out += e;
        out += L'\0';
    }
    out += L"TEMP=" + temp_dir + L'\0';
    out += L"TMP=" + temp_dir + L'\0';
    out += L"LOCALAPPDATA=" + temp_dir + L'\0';
    out += L'\0';
    ::FreeEnvironmentStringsW(envs);
    return true;
}

// 沙箱 v2：令牌先行——NtCreateLowBoxToken 直造低盒令牌（AppContainer 令牌本体），
// 后续 CreateProcessAsUserW 用它拉起，完全绕开 CreateProcessW 内部的
// SECURITY_CAPABILITIES 属性装配（该路径在本机 Insider 26200 上回归，见探针记录）。
// 零能力（capability 数 0）= 读宽写窄策略的断网 + 默认拒绝由令牌天然保证；工作区/
// PATH 的 ACE 授权仍按容器 SID 落盘，与此令牌同一 SID，全部照用。
// NtCreateLowBoxToken 是 ntdll 非 SDK 符号（Win8 起稳定存在，Chrome 系沙箱同款
// 依赖）：GetProcAddress 动态加载，缺口 ⇒ false，由 spawn 走 v1 属性路径/降级。
inline bool sandbox_lowbox_token(PSID sid, HANDLE& out, std::string& err) {
    // 参数形态照 ntdll 原型，但全用 void*——OBJECT_ATTRIBUTES 等内核类型不在
    // SDK 公开头（winternl.h 才有），指针宽度兼容即可安全调用
    using NtCreateLowBoxToken_fn = LONG(NTAPI*)(void**, void*, unsigned long, void*,
                                                void*, unsigned long, void*, unsigned long,
                                                void*);
    static NtCreateLowBoxToken_fn fn = []() -> NtCreateLowBoxToken_fn {
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        return ntdll != nullptr
                   ? reinterpret_cast<NtCreateLowBoxToken_fn>(
                         ::GetProcAddress(ntdll, "NtCreateLowBoxToken"))
                   : nullptr;
    }();
    if (fn == nullptr) {
        err = "NtCreateLowBoxToken 不可用（ntdll 缺口）";
        return false;
    }
    HANDLE tok = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &tok)) {
        err = "OpenProcessToken 失败 (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    HANDLE primary = nullptr;
    const bool dup = ::DuplicateTokenEx(tok, MAXIMUM_ALLOWED, nullptr,
                                        SecurityImpersonation, TokenPrimary, &primary);
    ::CloseHandle(tok);
    if (!dup) {
        err = "DuplicateTokenEx 失败 (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    // NT_SUCCESS = >= 0；capability 与继承句柄参数均为 0/空（读宽写窄 + 管道继承）
    const LONG rc = fn(&out, primary, MAXIMUM_ALLOWED, nullptr, sid, 0, nullptr, 0, nullptr);
    ::CloseHandle(primary);
    if (rc < 0) {
        char b[16];
        std::snprintf(b, sizeof b, "%lx", static_cast<unsigned long>(rc));
        err = "NtCreateLowBoxToken 失败 (0x" + std::string(b) + ")";
        return false;
    }
    return true;
}

inline bool spawn(const std::wstring& cmdline, const std::wstring& cwd, ChildPipes& out,
                  std::string& err, bool sandboxed = false, bool* degraded = nullptr) {
    PipePair so, se;
    if (!make_pipe(so, err) || !make_pipe(se, err)) return false;

    bool degraded_now = false;   // 两层沙箱路径都不可用 ⇒ 本次非沙箱（调用方告警）
    SandboxProfile prof;
    std::wstring env_block;
    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof(siex);
    siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    siex.StartupInfo.hStdOutput = so.write_end;
    siex.StartupInfo.hStdError = se.write_end;
    siex.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    LPPROC_THREAD_ATTRIBUTE_LIST attr_list = nullptr;
    SIZE_T attr_size = 0;
    DWORD flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
    HANDLE lowbox = nullptr;     // v2 低盒令牌（子进程持副本后父侧即关）
    bool have_lowbox = false;

    std::wstring cwd_w = cwd;
    if (sandboxed) {
        if (!sandbox_ensure_profile(prof, err)) return false;
        sandbox_grant_path_dirs(prof.sid);   // PATH 工具目录只读授权（进程内一轮，幂等）
        if (!sandbox_grant_dir(cwd.empty() ? std::filesystem::current_path().wstring() : cwd,
                               prof.sid, err))
            return false;
        if (!sandbox_env_block(prof.temp_dir, env_block, err)) return false;

        // ---- v2 令牌先行：绕开 SECURITY_CAPABILITIES 属性路径 ----
        std::string tok_err;
        if (sandbox_lowbox_token(prof.sid, lowbox, tok_err)) {
            have_lowbox = true;
            flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;   // 无属性列表
        } else {
            // v2 缺口静默——v1 属性路径接棒（探针记录两层的可用面不同）
            // ---- v1 属性路径：SECURITY_CAPABILITIES（本机 Insider 26200 上为 87）----
            ::InitializeProcThreadAttributeList(nullptr, 2, 0, &attr_size);
            attr_list = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                ::HeapAlloc(::GetProcessHeap(), 0, attr_size));
            if (attr_list == nullptr ||
                !::InitializeProcThreadAttributeList(attr_list, 2, 0, &attr_size)) {
                if (attr_list) ::HeapFree(::GetProcessHeap(), 0, attr_list);
                err = "InitializeProcThreadAttributeList 失败";
                return false;
            }
            SID_AND_ATTRIBUTES sa{prof.sid, 0};
            if (!::UpdateProcThreadAttribute(attr_list, 0,
                                             PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                             &sa, sizeof(sa), nullptr, nullptr)) {
                const DWORD gle = ::GetLastError();
                ::DeleteProcThreadAttributeList(attr_list);
                ::HeapFree(::GetProcessHeap(), 0, attr_list);
                attr_list = nullptr;
                // 探针实测：build 26200 任意 SID 形态/属性数均 87——沙箱不可用 ≠
                // 任务不可跑：降级非沙箱，degraded 交调用方显式告警
                if (gle == ERROR_INVALID_PARAMETER && degraded != nullptr) {
                    *degraded = true;
                    degraded_now = true;
                    siex.lpAttributeList = nullptr;
                } else {
                    err = "UpdateProcThreadAttribute 失败 (" + std::to_string(gle) + ")";
                    return false;
                }
            } else {
                flags |= CREATE_UNICODE_ENVIRONMENT;
                siex.lpAttributeList = attr_list;
            }
        }
    }

    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;  // CreateProcess* 可能改写缓冲 ⇒ 传可写副本
    const BOOL ok =
        have_lowbox
            ? ::CreateProcessAsUserW(lowbox, nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                     flags, env_block.data(),
                                     cwd_w.empty() ? nullptr : cwd_w.c_str(),
                                     &siex.StartupInfo, &pi)
            : ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags,
                               (sandboxed && !degraded_now) ? env_block.data() : nullptr,
                               cwd_w.empty() ? nullptr : cwd_w.c_str(),
                               &siex.StartupInfo, &pi);
    if (lowbox != nullptr) ::CloseHandle(lowbox);  // 子进程持令牌副本，父侧即关
    CloseHandle(so.write_end);  // 父进程写端用完即关（EOF 的前提）
    CloseHandle(se.write_end);
    if (!ok) {
        CloseHandle(so.read_end);
        CloseHandle(se.read_end);
        if (attr_list) {
            ::DeleteProcThreadAttributeList(attr_list);
            ::HeapFree(::GetProcessHeap(), 0, attr_list);
        }
        err = "CreateProcess 失败 (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    if (attr_list) {
        ::DeleteProcThreadAttributeList(attr_list);
        ::HeapFree(::GetProcessHeap(), 0, attr_list);
    }
    out.process = pi.hProcess;
    out.stdout_read = so.read_end;
    out.stderr_read = se.read_end;
    // Job Object（进程树容器）：KILL_ON_JOB_CLOSE ⇒ 句柄关闭即整树终止（超时/
    // deactivate 的树级兜底；嵌套作业 Windows 8+ 支持）
    out.job = ::CreateJobObjectW(nullptr, nullptr);
    if (out.job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jli{};
        jli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(out.job, JobObjectExtendedLimitInformation, &jli,
                                  sizeof jli);
        ::AssignProcessToJobObject(out.job, pi.hProcess);
    }
    return true;
}

// 终止进程树（超时/停用路径）：Job 句柄在则 TerminateJobObject（含孙进程），
// 否则退回 TerminateProcess 直接子进程。
inline void terminate_tree(ChildPipes& out) {
    if (out.job != nullptr) {
        ::TerminateJobObject(out.job, 1);
        return;
    }
    if (out.process != nullptr) ::TerminateProcess(out.process, 1);
}

// 阻塞读一段输出（EOF/错误 ⇒ false）。供读取线程循环调用。
inline bool read_chunk(HANDLE read_end, std::string& out) {
    char buf[4096];
    DWORD got = 0;
    if (!ReadFile(read_end, buf, sizeof buf, &got, nullptr) || got == 0) return false;
    out.append(buf, got);
    return true;
}

}  // namespace vcrun
