// 原生选目录对话框（plugin/webui 的 UI 原语切片，唯一实现翻译单元）；
// UTF-8 ↔ 宽字符转换用 vckit::win（全仓唯一实现）。
// 对话框定位状态与置顶钩子为本文件私有。/api/pick-folder 端点（独立线程弹对话
// 框 + 测试钩子）同在本文件——端点与对话框原语同域。

#include "webui_plugin.hpp"

#include <vckit/win.hpp>  // vckit::win：UTF-8 ↔ 宽字符（全仓唯一实现）

#include <objbase.h>   // CoInitializeEx/CoTaskMemFree：选目录线程的 COM 初始化
#include <shlobj.h>    // SHCreateItemFromParsingName：选目录对话框起始目录
#include <shobjidl.h>  // IFileDialog + FOS_PICKFOLDERS：现代选目录对话框

#include <cwchar>   // wcscmp：视图窗口类名比对
#include <cstdlib>  // std::getenv：测试钩子 ITHACA_TEST_PICK_PATH

namespace vc_webui {

// 机制与踩坑说明见头文件声明处注释（宿主无主对话框的四个系统默认行为纠正）。
std::string pick_folder_dialog(const std::string& title,
                               const std::string& initial_dir = {}) {
    IFileDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fd))) || !fd)
        return {};
    std::string result;
    DWORD opts = 0;
    if (SUCCEEDED(fd->GetOptions(&opts)))
        fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    fd->SetTitle(vckit::win::utf8_to_wide(title.empty() ? "选择工作文件夹" : title).c_str());
    fd->SetOkButtonLabel(L"选择文件夹");
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
        if (SUCCEEDED(SHCreateItemFromParsingName(vckit::win::utf8_to_wide(initial_dir).c_str(), nullptr,
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
                result = vckit::win::wide_to_utf8(ws);
                CoTaskMemFree(ws);
            }
            sel->Release();
        }
    }
    if (cbt) UnhookWindowsHookEx(cbt);
    fd->Release();
    return result;
}

/// POST /api/pick-folder → 原生目录对话框（独立线程弹，防模态阻塞），结果经
/// SSE folder_picked 事件返回。ITHACA_TEST_PICK_PATH 时跳过对话框直接返回
/// （零共享态，同步直答）。
http::Response WebuiPlugin::api_pick_folder_(const http::Request& req) {
    // UI 自动化测试钩子：设置 ITHACA_TEST_PICK_PATH ⇒ 跳过原生对话框，沿同一
    // folder_picked 事件路径返回（poster defer 广播）
    if (const char* v = std::getenv("ITHACA_TEST_PICK_PATH"); v != nullptr && *v != '\0') {
        vckit::poster_submit(poster_, [this, p = std::string(v)] {
            broadcast_({{"type", "folder_picked"}, {"path", p}});
        });
        return json_resp({{"ok", true}, {"path", std::string(v)}});
    }
    return defer_(req, [this](std::uint64_t conn) mutable {
        // workspace 初值在闭包内（宿主线程）读——defer_ 的 init-capture 在 select
        // 线程求值，而 config_ 写侧在宿主线程，提前到捕获点即数据竞争（唯一违例，
        // 其余端点数据面触碰全在闭包内）
        std::string initial = config_.value("workspace", "");
        // 停机纪律同 api_provider_models_：快照 server_ 值捕获，零触碰插件状态
        std::shared_ptr<http::Server> srv = server_;
        std::thread([srv, initial = std::move(initial)] {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool com = SUCCEEDED(hr);
        const std::string picked =
            com ? pick_folder_dialog("选择工作文件夹", initial)
                : std::string();
        if (com) CoUninitialize();
        if (!picked.empty() && srv)
            srv->broadcast(json{{"type", "folder_picked"}, {"path", picked}}.dump());
    }).detach();
        server_->respond(conn, json_resp({{"ok", true}}));
    });
}

}  // namespace vc_webui
