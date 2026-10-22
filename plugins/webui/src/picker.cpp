// 原生选目录对话框（plugin/webui 的 UI 原语切片，唯一实现翻译单元）；
// UTF-8 ↔ 宽字符转换用 nostos::kit::win（全仓唯一实现）。
// 对话框定位状态与置顶钩子为本文件私有。

#include "webui_plugin.hpp"

#include <nostos/kit/win.hpp>  // nostos::kit::win：UTF-8 ↔ 宽字符（全仓唯一实现）

#include <objbase.h>   // CoTaskMemFree：释放 shell 返回的路径串
#include <shlobj.h>    // SHCreateItemFromParsingName：选目录对话框起始目录
#include <shobjidl.h>  // IFileDialog + FOS_PICKFOLDERS：现代选目录对话框

#include <cwchar>  // wcscmp：视图窗口类名比对

namespace vc_webui {

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
    fd->SetTitle(nostos::kit::win::utf8_to_wide(title.empty() ? "选择工作文件夹" : title).c_str());
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
        if (SUCCEEDED(SHCreateItemFromParsingName(nostos::kit::win::utf8_to_wide(initial_dir).c_str(), nullptr,
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
                result = nostos::kit::win::wide_to_utf8(ws);
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
