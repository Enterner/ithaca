// 兄弟实例健康探测（plugin/webui 的多实例聚合切片）。
// 后台线程每 2s 对 manifest 各实例实际端口做 /api/health 探测并缓存 alive
// 真值；/api/shell/sessions 聚合端点同在本文件（数据全部来自磁盘 + 探测缓存）。

#include "webui_plugin.hpp"

#include <nostos/kit/win_http.hpp>  // 一发 HTTP 探测小件（兄弟实例健康检查）

#include <sstream>  // std::ostringstream：整读兄弟实例 index.json

namespace vc_webui {

// ---- 兄弟实例健康探测（多实例聚合 alive 真值）------------------------------
// 背景：聚合端点原以 port 文件存在判 alive——宿主死亡后文件残留 ⇒ 死会话被标
// 存活，前端整页导航到死端口（踩过：切换会话 nav FAIL 循环）。本线程每 2s 对
// manifest 各 key 的实际端口做 /api/health 探测并缓存真值；聚合端点读缓存。
// 仅壳模式（ITHACA_SHELL_MANIFEST 已设）运行。
void WebuiPlugin::start_prober_() {
    const char* mf = std::getenv("ITHACA_SHELL_MANIFEST");
    if (mf == nullptr || *mf == '\0') return;
    manifest_path_ = mf;
    prober_run_.store(true);
    prober_ = std::thread([this] {
        while (prober_run_.load()) {
            std::ifstream in(manifest_path_, std::ios::binary);
            std::string line;
            std::map<std::string, std::pair<bool, int>> fresh;
            while (std::getline(in, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.empty()) continue;
                const size_t t1 = line.find('\t');
                if (t1 == std::string::npos) continue;
                const std::string key = line.substr(0, t1);
                if (key == "ACTIVE") continue;
                std::ifstream pin(manifest_path_.parent_path() / "hosts" / key / "port",
                                  std::ios::binary);
                int port = 0;
                if (pin >> port) {
                    const bool alive = win::http_fetch(
                        std::wstring(L"http://127.0.0.1:") + std::to_wstring(port) +
                            L"/api/health",
                        {.timeouts = {800, 800, 800, 800}}).status == 200;
                    fresh[key] = {alive, port};
                }
            }
            {
                std::lock_guard<std::mutex> lock(probe_mu_);
                probe_cache_ = std::move(fresh);
            }
            for (int i = 0; i < 20 && prober_run_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void WebuiPlugin::stop_prober_() {
    prober_run_.store(false);
    if (prober_.joinable()) prober_.join();
}

// 实例瞬态状态落盘（聚合侧栏的徽标数据源：壳/兄弟实例只读磁盘，不互发 HTTP）
void WebuiPlugin::persist_status_() {
    bool pending = false;
    {
        std::lock_guard<std::recursive_mutex> lock(data_mu_);
        pending = pending_turn_ >= 0;
    }
    std::ofstream f(home_ / "status.json", std::ios::binary | std::ios::trunc);
    if (f) f << json{{"working", in_flight_.load()}, {"pending", pending}}.dump();
}

/// GET /api/shell/sessions → 聚合会话清单（多实例壳模式）。数据全部来自磁盘：
/// 壳 manifest（ITHACA_SHELL_MANIFEST env，未设 = 非壳模式返回 shell:false）+
/// 各实例 home 的 index.json（标题/时间/真实会话 id）、port（存活）、status.json
/// （working/pending 徽标）。本实例自己的行会照常列出——前端以 /api/session 的
/// id 判定哪一行是"当前页"。文件缺失/损坏的实例按尽力而为字段返回。
http::Response WebuiPlugin::api_shell_sessions_() {
    const char* mf = std::getenv("ITHACA_SHELL_MANIFEST");
    if (mf == nullptr || *mf == '\0')
        return json_resp({{"ok", true}, {"shell", false}});
    const fs::path manifest(mf);
    const fs::path hosts_root = manifest.parent_path() / "hosts";
    json rows = json::array();
    std::ifstream in(manifest, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        const std::string key = line.substr(0, t1);
        if (key == "ACTIVE") continue;
        const std::string ws = line.substr(t1 + 1);
        const fs::path home = hosts_root / key;
        json row = {{"key", key},
                    {"id", key},
                    {"title", ""},
                    {"updated", 0},
                    {"workspace", ws},
                    {"alive", false},
                    {"port", 0},
                    {"working", false},
                    {"pending", false}};
        std::error_code ec;
        const fs::path idx_f = home / "index.json";
        std::string idx_text;
        if (fs::exists(idx_f, ec)) {
            std::ifstream idx_in(idx_f, std::ios::binary);
            std::ostringstream buf;
            buf << idx_in.rdbuf();
            idx_text = buf.str();
        } else {
            // 未迁移（从没被点开过）：从中央库条目兜底取标题/时间/工作区
            const fs::path central = manifest.parent_path() / "index.json";
            if (fs::exists(central, ec)) {
                std::ifstream cin2(central, std::ios::binary);
                std::ostringstream buf;
                buf << cin2.rdbuf();
                const std::string ctext = buf.str();
                const std::string idkey = "\"id\":\"" + key + "\"";
                const size_t k = ctext.find(idkey);
                if (k != std::string::npos) {
                    const size_t b = ctext.rfind('{', k);
                    const size_t e = ctext.find('}', k);
                    if (b != std::string::npos && e != std::string::npos)
                        idx_text = ctext.substr(b, e - b + 1);
                }
            }
        }
        if (!idx_text.empty()) {
            json idx = json::parse(idx_text, nullptr, false);
            if (idx.is_object()) {
                // 两种形态：实例 index（{active, sessions:[…]}) / 中央条目块
                //（直接就是会话对象）。取"活会话"条目填标题/时间/工作区
                const json* sess = nullptr;
                if (idx.contains("sessions") && idx["sessions"].is_array()) {
                    const std::string active_id = idx.value("active", "");
                    row["active_id"] = active_id;
                    for (const auto& e : idx["sessions"]) {
                        if (e.is_object() && e.value("id", "") == active_id) {
                            sess = &e;
                            break;
                        }
                    }
                } else if (idx.value("id", "") == key) {
                    sess = &idx;
                }
                if (sess != nullptr) {
                    row["id"] = sess->value("id", key);
                    row["title"] = sess->value("title", "");
                    row["updated"] = sess->value("updated", 0LL);
                    const std::string w = sess->value("workspace", "");
                    if (!w.empty()) row["workspace"] = w;
                }
            }
        }
        // alive 真值 = 探测缓存（实际健康检查）；缓存未覆盖（首轮）回退 port 文件
        {
            int port = 0;
            bool alive = false;
            {
                std::lock_guard<std::mutex> lock(probe_mu_);
                const auto it = probe_cache_.find(key);
                if (it != probe_cache_.end()) {
                    alive = it->second.first;
                    port = it->second.second;
                }
            }
            if (port == 0) {
                const fs::path port_f = home / "port";
                std::error_code pec;
                if (fs::exists(port_f, pec)) {
                    std::ifstream pin(port_f, std::ios::binary);
                    if (pin >> port) alive = port > 0;   // 首轮探测未跑：乐观回退
                }
            }
            row["port"] = port;
            row["alive"] = alive;
        }
        const fs::path st_f = home / "status.json";
        if (fs::exists(st_f, ec)) {
            std::ifstream st_in(st_f, std::ios::binary);
            json st = json::parse(st_in, nullptr, false);
            if (!st.is_discarded() && st.is_object()) {
                row["working"] = st.value("working", false);
                row["pending"] = st.value("pending", false);
            }
        }
        rows.push_back(row);
    }
    return json_resp({{"ok", true}, {"shell", true}, {"sessions", rows}});
}

}  // namespace vc_webui
