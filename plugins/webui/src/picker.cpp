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

// 机制与踩坑说明见头文件声明处注释（宿主无主对话框的四个系统默认行为纠正）。
std::string pick_folder_dialog(const std::string& title, const std::string& initial_dir) {
    IFileDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fd))) || !fd)
        return {};
    std::string result;
    DWORD opts = 0;
    if (SUCCEEDED(fd->GetOptions(&opts)))
        fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    fd->SetTitle(utf8_to_wide(title.empty() ? "选择工作文件夹" : title).c_str());
    fd->SetOkButtonLabel(L"选择文件夹");
    // 文件选择框置顶显示钩子
    HHOOK cbt = SetWindowsHookExW(WH_CBT,
    [](int code, WPARAM wp, LPARAM lp) -> LRESULT {
        if (code == HCBT_ACTIVATE) {
            SetWindowPos(reinterpret_cast<HWND>(wp), HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    },
    nullptr, GetCurrentThreadId());

    if (!initial_dir.empty()) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(utf8_to_wide(initial_dir).c_str(), nullptr,
                                                  IID_PPV_ARGS(&item))) && item) {
            fd->SetDefaultFolder(item);
            item->Release();
        }
    }
    if (SUCCEEDED(fd->Show(nullptr))) {
        IShellItem* sel = nullptr;
        if (SUCCEEDED(fd->GetResult(&sel)) && sel) {
            PWSTR ws = nullptr;
            if (SUCCEEDED(sel->GetDisplayName(SIGDN_FILESYSPATH, &ws))) {
                result = wide_to_utf8(ws);
                CoTaskMemFree(ws);
            }
            sel->Release();
        }
    }
    if (cbt) UnhookWindowsHookEx(cbt);
    fd->Release();
    return result;
}

}  // namespace vc_webui
