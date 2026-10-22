// 文件树 watcher（plugin/webui 的 VSCode 式真监听切片）。
// 递归监听当前工作区根，变更经 SSE 广播 fs_changed，前端据此自动重拉文件树。

#include "webui_plugin.hpp"

#include <nostos/kit/win.hpp>  // nostos::kit::win：UTF-8 ↔ 宽字符（全仓唯一实现）

namespace vc_webui {

// ---- 文件树 watcher（VSCode 式真监听）--------------------------------------
// ReadDirectoryChangesW 递归监听当前工作区根，变更经 SSE 广播 fs_changed——
// 前端 fsRev++ 触发文件树自动重拉。黑名单目录不过滤（递归监听无法排除子树），
// 靠 350ms 去抖合并事件风暴，刷新本身有界（几次 HTTP list）。
// NOTE: broadcast_ 自本线程直发是安全的（handle 内禁的是 select 线程自锁——
// pick-folder 线程同款先例）；server_ 未起/已停时空操作。

void WebuiPlugin::start_fs_watcher_(const std::string& root) {
    stop_fs_watcher_();
    if (root.empty()) return;   // hero 态：无工作区不监听
    fs_watch_handle_ = CreateFileW(nostos::kit::win::utf8_to_wide(root).c_str(), FILE_LIST_DIRECTORY,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                   nullptr);
    if (fs_watch_handle_ == INVALID_HANDLE_VALUE || fs_watch_handle_ == nullptr) {
        fs_watch_handle_ = nullptr;
        // 网络盘/特殊路径监听不可靠——只降级不致命，树退化为手动刷新
        host_.log(3, "ithaca.webui: 文件监听打不开（" + root + "）——文件树退化为手动刷新");
        return;
    }
    fs_watch_running_.store(true);
    fs_watch_thread_ = std::thread([this] {
        std::vector<char> buf(64 * 1024);
        while (fs_watch_running_.load()) {
            DWORD n = 0;
            if (!ReadDirectoryChangesW(
                    fs_watch_handle_, buf.data(), static_cast<DWORD>(buf.size()), TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                    &n, nullptr, nullptr)) {
                // ERROR_OPERATION_ABORTED = 正常停止路径；其余记一次后退出（退化手动）
                if (const DWORD err = GetLastError(); err != ERROR_OPERATION_ABORTED)
                    host_.log(3, "ithaca.webui: 文件监听停止（错误 " + std::to_string(err) + "）");
                break;
            }
            if (!fs_watch_running_.load()) break;
            Sleep(350);   // 去抖：一次保存/构建是成簇通知，合并成一条
            if (!fs_watch_running_.load()) break;
            broadcast_({{"type", "fs_changed"}});
        }
    });
}

void WebuiPlugin::stop_fs_watcher_() {
    if (fs_watch_handle_ != nullptr && fs_watch_handle_ != INVALID_HANDLE_VALUE) {
        fs_watch_running_.store(false);
        CancelIoEx(fs_watch_handle_, nullptr);   // 唤醒阻塞中的同步读
    }
    if (fs_watch_thread_.joinable()) fs_watch_thread_.join();
    if (fs_watch_handle_ != nullptr && fs_watch_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(fs_watch_handle_);
        fs_watch_handle_ = nullptr;
    }
}

}  // namespace vc_webui
