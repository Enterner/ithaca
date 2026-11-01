#pragma once
// host/profile —— 产品形态（profile）装载层：一份 JSON = 一个形态。
//
// profile 是宿主层的声明式装配文件，只做两件事：决定装载哪些插件；把产品级
// 配置翻译成插件既有的 env 旋钮。插件侧不知道 profile 存在——字段 → env 的
// 映射表（apply_envs_ 内）就是本层全部知识，新增旋钮 = 多一行映射，不是一次
// 插件改造。产品差异只许落在插件集 / profile 字段 / UI 产物三处；共享代码里
// 出现按产品分支的 if 即架构违规——本头不含任何形态名。
//
// 纪律：
//   · env 应用顺序 = ITHACA_PROFILE（绝对路径只读钩子）→ 建模字段 → env{}
//     逃生口（最后、最高优先级）。profile 覆盖外部同名 env（产品权威）；唯一
//     例外是 selftest 场景 env——宿主在本模块之后才应用它（场景盖过默认）。
//     NOTE: 调试想临时改值用 env{} 而不是外部 env，否则被 profile 盖掉。
//   · profile 声明的插件缺失 = 硬失败（产品装配不完整必须启动即报，带病启动
//     只会把故障推迟到会话中期且症状间接）；位置参数的老路径不受此管
//     （打日志继续，向后兼容）。
//   · 相对路径一律相对 profile 文件目录解析——profile 与提示词/UI 一起搬不失效。
//   · header-only：宿主唯一包含点 main.cpp，无独立编译单元。
#include <vckit/win.hpp>  // utf8_to_wide/wide_to_utf8（路径经宽字符 API 才不失真）
#include "vcws/text.hpp"  // ascii_lower：清单项折叠（vcws 单源，全仓唯一一份）
#include "nlohmann/json.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

namespace ithaca::profile {

namespace fs = std::filesystem;
using json = nlohmann::json;

struct Loaded {
    bool ok = false;
    std::string error;                  // ok=false 的人话原因（点名到文件/键）；
                                        // ok=true 时可承载告警（如未知 schema 版本）
    std::string name;                   // 人读名（启动打屏）
    std::string path;                   // profile 绝对路径（ITHACA_PROFILE 同值）
    std::vector<std::string> plugins;   // 解析后的 DLL 路径（顺序 = 装载顺序）
};

// ---- 小件 ---------------------------------------------------------------------

// 启动期单线程调用，无并发面。NOTE: Windows 走 _putenv_s 保 CRT getenv 的字节级
// 回传（UTF-8 值原样可达消费方）；_wgetenv 读宽环境副本，非 ASCII 值经 ACP 转换
// 可能失真——现有消费方只有 runner 的 shell 选择用宽口，值恒为 ASCII 路径可接受。
inline void set_env(const std::string& key, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(key.c_str(), value.c_str());
#else
    ::setenv(key.c_str(), value.c_str(), 1);
#endif
}

// %NAME% 展开；未定义的原样保留（缺变量的字面值可见，静默清空难以排查）。
inline std::string expand_env(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    std::size_t pos = 0;
    while (pos < in.size()) {
        const std::size_t head = in.find('%', pos);
        if (head == std::string::npos) break;
        const std::size_t tail = in.find('%', head + 1);
        if (tail == std::string::npos) break;
        const std::string name = in.substr(head + 1, tail - head - 1);
        const char* v = name.empty() ? nullptr : std::getenv(name.c_str());
        if (v == nullptr) {
            out += in.substr(pos, tail + 1 - pos);
        } else {
            out += in.substr(pos, head - pos);
            out += v;
        }
        pos = tail + 1;
    }
    out += in.substr(pos);
    return out;
}

// MSVC 的 fs::path(string) 按 ANSI 码页解释——profile 里的中文路径必须走宽字符
// 入口（与全仓 utf8_to_wide 纪律一致），POSIX 下窄字符即原生编码。
inline fs::path path_from_utf8(const std::string& s) {
#if defined(_WIN32)
    return fs::path(vckit::win::utf8_to_wide(s));
#else
    return fs::path(s);
#endif
}
inline std::string path_to_utf8(const fs::path& p) {
#if defined(_WIN32)
    return vckit::win::wide_to_utf8(p.wstring().c_str());
#else
    return p.string();
#endif
}

// 相对 base 解析 + %VAR% 展开 + 规范化；base = profile 所在目录。
inline fs::path resolve_path(const fs::path& base, const std::string& value) {
    fs::path p = path_from_utf8(expand_env(value));
    if (p.is_relative()) p = base / p;
    return p.lexically_normal();
}

// profile 标量 → env 字符串：string 原样 / bool 1|0 / 数值十进制；其余（对象、
// 数组、null）不是合法旋钮值，返回空由调用方跳过。
inline std::string scalar_text(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
    if (v.is_number()) return v.dump();
    return "";
}

// 按键路径下钻；任一层缺失或不是对象 ⇒ nullptr。
inline const json* find_key(const json& doc, std::initializer_list<const char*> path) {
    const json* cur = &doc;
    for (const char* k : path) {
        if (!cur->is_object()) return nullptr;
        const auto it = cur->find(k);
        if (it == cur->end()) return nullptr;
        cur = &*it;
    }
    return cur;
}

// 建模字段应用三态：键缺失/空值 = 不设 env（用插件既有默认，profile 只表达差异）。
inline void apply_scalar(const json& doc, std::initializer_list<const char*> path,
                         const char* env_key) {
    const json* v = find_key(doc, path);
    if (v == nullptr || v->is_null()) return;
    const std::string text = scalar_text(*v);
    if (!text.empty()) set_env(env_key, text);
}

inline void apply_path(const json& doc, std::initializer_list<const char*> path,
                       const char* env_key, const fs::path& base) {
    const json* v = find_key(doc, path);
    if (v == nullptr || !v->is_string()) return;
    const std::string raw = v->get<std::string>();
    if (raw.empty()) return;
    set_env(env_key, path_to_utf8(resolve_path(base, raw)));
}

// 布尔旋钮走 agent 的关闭口径：字符串 "off"（true → "1" 仅为显式开，便于日志辨析）。
inline void apply_bool_off(const json& doc, std::initializer_list<const char*> path,
                           const char* env_key) {
    const json* v = find_key(doc, path);
    if (v == nullptr || !v->is_boolean()) return;
    set_env(env_key, v->get<bool>() ? "1" : "off");
}

// ---- 清单项解析 ----------------------------------------------------------------

// 清单项 → DLL 路径：含分隔符或带 .dll/.so 扩展名 = 路径（相对 profile 目录，
// 支持 %VAR%）；否则 <plugin_dir>/<名>_plugin<后缀>。plugin_dir 缺省 = exe 目录
//（开发构建树宿主与 dll 同目录平铺；打包布局的 plugins\ 子目录由 profile 显式
// 写 "plugin_dir": "plugins"）。
inline std::string resolve_plugin(const std::string& entry, const fs::path& plugin_dir,
                                  const fs::path& profile_dir) {
    const std::string lower = vcws::ascii_lower(entry);
    // NOTE: 后缀比较必须先卡长度——短于后缀时 size()-N 下溢回绕成 SIZE_MAX，
    // 恰与 npos 相等，"mcp" 这类短名会被误判成路径
    const bool is_path = entry.find('/') != std::string::npos ||
                         entry.find('\\') != std::string::npos ||
                         (lower.size() >= 4 && lower.rfind(".dll") == lower.size() - 4) ||
                         (lower.size() >= 3 && lower.rfind(".so") == lower.size() - 3);
    if (is_path) return path_to_utf8(resolve_path(profile_dir, entry));
#if defined(_WIN32)
    const char* suffix = ".dll";
#else
    const char* suffix = ".so";
#endif
    return path_to_utf8((plugin_dir / path_from_utf8(entry + "_plugin" + suffix))
                            .lexically_normal());
}

// profile 定位：显式路径（cwd 相对，绝对路径自然命中）→ 裸名依次试 <cwd>/profiles/
// 与 <exe>/profiles/ 及上溯三级父目录（开发构建布局 build/<preset>/bin/ 到仓根是
// 三级；便携包 exe 与 profiles/ 同级，第一级即命中）。候选必须是常规文件——裸名
// 与同名目录撞车时（如仓根 docs/）回落下一候选而非把目录当 profile 打不开。
// 找不到返回空路径。
inline fs::path locate(const std::string& arg, const fs::path& exe_dir_path) {
    std::error_code ec;
    const auto usable = [&ec](const fs::path& p) {
        return fs::is_regular_file(p, ec);
    };
    const fs::path direct = path_from_utf8(arg);
    if (usable(direct)) return direct.lexically_normal();
    std::string name = vcws::ascii_lower(arg);
    if (name.size() > 5 && name.rfind(".json") == name.size() - 5)
        name = name.substr(0, name.size() - 5);   // "docs.json" 与 "docs" 同义
    const std::string fname = name + ".json";
    const fs::path bases[] = {
        fs::current_path(ec) / "profiles", exe_dir_path / "profiles",
        exe_dir_path / ".." / "profiles",  exe_dir_path / ".." / ".." / "profiles",
        exe_dir_path / ".." / ".." / ".." / "profiles",
    };
    for (const auto& b : bases) {
        const fs::path cand = (b / path_from_utf8(fname)).lexically_normal();
        if (usable(cand)) return cand;
    }
    return {};
}

inline fs::path exe_dir(const char* argv0) {
#if defined(_WIN32)
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    if (n > 0 && n < 32768) return fs::path(std::wstring(buf, n)).parent_path();
#endif
    std::error_code ec;
    const fs::path p =
        fs::absolute(path_from_utf8(argv0 != nullptr ? argv0 : "."), ec);
    return ec || p.empty() ? fs::current_path() : p.parent_path();
}

// ---- 装载 + env 翻译 --------------------------------------------------------------

// 字段 → env 映射表（本层全部知识；env 名 = 插件既有旋钮，消费方见各插件 activate）。
inline void apply_envs_(const json& doc, const fs::path& base, const std::string& abs_path) {
    set_env("ITHACA_PROFILE", abs_path);
    apply_path(doc, {"home"}, "ITHACA_WEBUI_HOME", base);
    apply_scalar(doc, {"llm", "active"}, "ITHACA_LLM_ACTIVE");
    apply_scalar(doc, {"llm", "model"}, "ITHACA_MODEL");
    apply_scalar(doc, {"llm", "api_base"}, "ITHACA_API_BASE");
    apply_scalar(doc, {"llm", "api_key"}, "ITHACA_API_KEY");
    // 工具清单：空数组/缺失 = 不设 env（agent 用内置默认清单）；非空写 CSV
    if (const json* t = find_key(doc, {"agent", "tools"});
        t != nullptr && t->is_array() && !t->empty()) {
        std::string csv;
        for (const auto& e : *t) {
            if (!e.is_string()) continue;
            if (!csv.empty()) csv += ',';
            csv += e.get<std::string>();
        }
        if (!csv.empty()) set_env("ITHACA_AGENT_TOOLS", csv);
    }
    apply_scalar(doc, {"agent", "permission_mode"}, "ITHACA_PERMISSION_MODE");
    apply_scalar(doc, {"agent", "system_prompt"}, "ITHACA_SYSTEM_PROMPT");
    apply_path(doc, {"agent", "system_prompt_file"}, "ITHACA_SYSTEM_PROMPT_FILE", base);
    apply_scalar(doc, {"agent", "context", "window"}, "ITHACA_CONTEXT_WINDOW");
    apply_bool_off(doc, {"agent", "context", "summary"}, "ITHACA_CONTEXT_SUMMARY");
    apply_scalar(doc, {"agent", "context", "compress_tokens"},
                 "ITHACA_CONTEXT_COMPRESS_TOKENS");
    // 上下文策略选择（vc.ctx.v1 的实现名，如 "basic"）：映射到策略插件的 env 门
    //（ITHACA_CTX_ACTIVE，ctx_basic 缺省/同值活跃，其他值休眠）
    apply_scalar(doc, {"agent", "context", "strategy"}, "ITHACA_CTX_ACTIVE");
    apply_path(doc, {"agent", "session_file"}, "ITHACA_SESSION_FILE", base);
    apply_path(doc, {"agent", "dir"}, "ITHACA_AGENT_DIR", base);
    apply_path(doc, {"workspace", "dir"}, "ITHACA_WORKSPACE_DIR", base);
    apply_scalar(doc, {"runner", "shell"}, "ITHACA_RUNNER_SHELL");
    apply_path(doc, {"mcp", "config"}, "ITHACA_MCP_CONFIG", base);
    apply_path(doc, {"ui", "web"}, "ITHACA_WEBUI_WEB", base);
    apply_scalar(doc, {"ui", "port"}, "ITHACA_WEBUI_PORT");
    apply_path(doc, {"ui", "config"}, "ITHACA_WEBUI_CONFIG", base);
    // env{} 逃生口：未建模旋钮先走这里（最后应用 = 最高优先级）；新增旋钮须在
    // 方案文档登记后再字段化
    if (const json* env = find_key(doc, {"env"}); env != nullptr && env->is_object())
        for (auto it = env->begin(); it != env->end(); ++it) {
            const std::string v = scalar_text(it.value());
            if (!v.empty()) set_env(it.key(), v);
        }
}

inline Loaded load(const std::string& arg, const fs::path& exe_dir_path) {
    Loaded out;
    const fs::path file = locate(arg, exe_dir_path);
    if (file.empty()) {
        out.error = "找不到 profile：" + arg + "（已试 cwd、<cwd>/profiles/ 与 "
                    "<exe> 三级父目录下的 profiles/）";
        return out;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        out.error = "profile 打不开：" + path_to_utf8(file);
        return out;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        out.error = "profile 不是合法的 JSON 对象：" + path_to_utf8(file);
        return out;
    }
    out.ok = true;
    std::error_code ec;
    const fs::path abs = fs::weakly_canonical(file, ec);
    out.path = path_to_utf8(ec ? fs::absolute(file) : abs);
    out.name = doc.is_object() && doc.value("name", "") != ""
                   ? doc.value("name", "")
                   : path_to_utf8(file.stem());
    // schema 版本位：未知版本只告警不失败（形态演进时旧宿主仍可装载新文件）
    if (const json* s = find_key(doc, {"schema"});
        s != nullptr && !(s->is_number_integer() && s->get<long long>() == 1))
        out.error = "profile schema 版本未识别，按当前版本解析：" + scalar_text(*s);

    // 插件清单：缺失项收集齐再报（一次看全缺哪些，不必逐个补）
    if (const json* pl = find_key(doc, {"plugins"}); pl != nullptr && !pl->is_null()) {
        if (!pl->is_array()) {
            out.ok = false;
            out.error = "profile 的 plugins 必须是字符串数组：" + out.path;
            return out;
        }
        fs::path pdir = exe_dir_path;
        if (const json* pd = find_key(doc, {"plugin_dir"}); pd != nullptr &&
                                                             pd->is_string() &&
                                                             !pd->get<std::string>().empty())
            pdir = resolve_path(file.parent_path(), pd->get<std::string>());
        std::string missing;
        for (const auto& e : *pl) {
            if (!e.is_string()) {
                out.ok = false;
                out.error = "profile 的 plugins 含非字符串项：" + out.path;
                return out;
            }
            const std::string dll = resolve_plugin(e.get<std::string>(), pdir,
                                                   file.parent_path());
            if (!fs::exists(path_from_utf8(dll), ec))
                missing += missing.empty() ? dll : "；" + dll;
            out.plugins.push_back(dll);
        }
        if (!missing.empty()) {
            out.ok = false;
            out.error = "profile 声明的插件不存在：" + missing;
            return out;
        }
    }
    apply_envs_(doc, file.parent_path(), out.path);
    return out;
}

}  // namespace ithaca::profile
