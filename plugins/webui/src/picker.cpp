// 原生选目录对话框（plugin/webui 的 UI 原语切片，唯一实现翻译单元）。
// 对外导出 utf8_to_wide / wide_to_utf8 / pick_folder_dialog（声明见
// include/webui_plugin.hpp）；对话框定位状态与两个钩子为本文件私有
// （匿名命名空间），其他翻译单元不可见。

#include "webui_plugin.hpp"

#include <objbase.h>   // CoTaskMemFree：释放 shell 返回的路径串
#include <shlobj.h>    // SHCreateItemFromParsingName：选目录对话框起始目录
#include <shobjidl.h>  // IFileDialog + FOS_PICKFOLDERS：现代选目录对话框

#include <cwchar>  // wcscmp：视图窗口类名比对

namespace vc_webui {

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring wide(need > 0 ? static_cast<std::size_t>(need) : 0, L'\0');
    if (!wide.empty())
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                            need);
    return wide;
}

std::string wide_to_utf8(const wchar_t* text) {
    if (text == nullptr) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};  // 只剩结尾 NUL = 空串
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), need, nullptr, nullptr);
    return out;
}

namespace {

// 对话框定位状态（in_flight 门卫保证同时最多一个；机制见 pick_folder_dialog 注释）
struct PickerLayout {
    HWND dlg = nullptr;              // 对话框顶层窗口（HCBT_ACTIVATE 时捕获）
    RECT work = {};                  // 目标显示器工作区（激活时的前台窗口所在屏）
    unsigned __int64 t_activate = 0; // 激活时刻——重居中只在此后短时限内生效
    bool shown = false;              // 已执行延迟显示
    bool view_seen = false;          // SHELLDLL_DefView（视图）已出现
};
PickerLayout g_picker;

// 居中到 g_picker.work；show=true 时同时置顶显示
void picker_center(HWND hwnd, bool show) {
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return;
    const int x = g_picker.work.left +
                  ((g_picker.work.right - g_picker.work.left) - (rc.right - rc.left)) / 2;
    const int y = g_picker.work.top +
                  ((g_picker.work.bottom - g_picker.work.top) - (rc.bottom - rc.top)) / 2;
    if (show)
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    else
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// WinEvent 兜底（机制见 pick_folder_dialog 注释）：等视图就绪显示 + 时限内重居中
void CALLBACK picker_win_event(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_obj,
                               LONG, DWORD, DWORD) {
    if (g_picker.dlg == nullptr || id_obj != OBJID_WINDOW) return;
    if (event == EVENT_OBJECT_SHOW) {
        if (!g_picker.view_seen && GetAncestor(hwnd, GA_ROOT) == g_picker.dlg) {
            wchar_t cls[64] = {};
            GetClassNameW(hwnd, cls, 64);
            g_picker.view_seen = wcscmp(cls, L"SHELLDLL_DefView") == 0;
        }
    } else if (event == EVENT_OBJECT_LOCATIONCHANGE && hwnd == g_picker.dlg &&
               GetTickCount64() - g_picker.t_activate <= 1500) {
        picker_center(hwnd, false);   // 已居中 ⇒ 无实际位移 ⇒ 事件不再触发，自然收敛
    }
    // 视图就绪或 2s 超时 ⇒ 居中显示
    if (!g_picker.shown && (g_picker.view_seen ||
                            GetTickCount64() - g_picker.t_activate > 2000)) {
        g_picker.shown = true;
        picker_center(g_picker.dlg, true);
    }
}

}  // namespace

// 机制与踩坑说明见头文件声明处注释（宿主无主对话框的四个系统默认行为纠正）。
std::string pick_folder_dialog(const std::string& title, const std::string& initial_dir) {
    IFileDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fd))) || fd == nullptr)
        return {};
    std::string result;
    HHOOK cbt = SetWindowsHookExW(
        WH_CBT,
        [](int code, WPARAM wp, LPARAM lp) -> LRESULT {
            if (code == HCBT_ACTIVATE) {
                const HWND hwnd = reinterpret_cast<HWND>(wp);
                // MonitorFromWindow 携 DEFAULTTONEAREST 永不返回空，无需兜底
                HMONITOR mon = MonitorFromWindow(GetForegroundWindow(),
                                                 MONITOR_DEFAULTTONEAREST);
                RECT rc{};
                MONITORINFO info{sizeof(info)};   // cbSize 必须先置（踩过：居中静默失效）
                if (GetMonitorInfoW(mon, &info) && GetWindowRect(hwnd, &rc)) {
                    g_picker.dlg = hwnd;
                    g_picker.work = info.rcWork;
                    g_picker.t_activate = GetTickCount64();
                    if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr)
                        picker_center(hwnd, true);   // 视图已就绪：居中显示
                    else {
                        ShowWindow(hwnd, SW_HIDE);   // 延迟显示：等视图出现（WinEvent 兜底）
                        picker_center(hwnd, false);
                    }
                } else {
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                }
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        },
        nullptr, GetCurrentThreadId());
    do {
        DWORD opts = 0;
        if (FAILED(fd->GetOptions(&opts))) break;
        fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        fd->SetTitle(utf8_to_wide(title.empty() ? std::string("选择工作文件夹") : title).c_str());
        fd->SetOkButtonLabel(L"选择文件夹");
        if (!initial_dir.empty()) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(utf8_to_wide(initial_dir).c_str(), nullptr,
                                                      IID_PPV_ARGS(&item))) &&
                item != nullptr) {
                fd->SetDefaultFolder(item);   // 起始目录（不预选，只定位）
                item->Release();
            }
        }
        // 记忆边界回写兜底 + 视图就绪延迟显示（见 picker_win_event）
        g_picker.shown = false;
        g_picker.view_seen = false;
        const HWINEVENTHOOK winevent = SetWinEventHook(
            EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
            picker_win_event, GetCurrentProcessId(), GetCurrentThreadId(),
            WINEVENT_OUTOFCONTEXT);
        const HRESULT shown = fd->Show(nullptr);
        if (winevent != nullptr) UnhookWinEvent(winevent);
        if (SUCCEEDED(shown)) {
            IShellItem* sel = nullptr;
            if (SUCCEEDED(fd->GetResult(&sel)) && sel != nullptr) {
                PWSTR ws = nullptr;
                if (SUCCEEDED(sel->GetDisplayName(SIGDN_FILESYSPATH, &ws))) {
                    result = wide_to_utf8(ws);
                    CoTaskMemFree(ws);
                }
                sel->Release();
            }
        }
    } while (false);
    if (cbt != nullptr) UnhookWindowsHookEx(cbt);
    fd->Release();
    g_picker.dlg = nullptr;   // 对话框已关，WinEvent 兜底随之失效
    return result;
}

}  // namespace vc_webui
