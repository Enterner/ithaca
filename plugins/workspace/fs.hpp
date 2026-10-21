#pragma once
// 文本文件存取的接缝：真实实现走磁盘（内容即字节串，UTF-8 由调用方保证/校验），
// 单元测试用内存实现。键一律是"jail 解析后的全路径（宽字符）"——纯逻辑层因此与
// jail 解耦（测试里键就是任意字符串）。

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "text.hpp"  // utf8_valid / trim_utf8（收敛点，多插件共用）

namespace vcws {

namespace fs = std::filesystem;

class TextStore {
public:
    virtual ~TextStore() = default;
    virtual bool read(const std::wstring& key, std::string& out_bytes) = 0;  // 存在且可读 ⇒ true
    virtual bool write(const std::wstring& key, const std::string& bytes) = 0;
    virtual bool remove(const std::wstring& key) = 0;
    virtual bool exists(const std::wstring& key) = 0;
    virtual std::string last_error() const { return {}; }  // 最近一次失败的系统原因（诊断用）
};

// 磁盘实现。write 隐式建父目录（"写新路径"的常规期望；越界已由 jail 拦住）。
class DiskStore final : public TextStore {
public:
    std::string last_error_;   // 最近一次写失败的系统原因
    bool read(const std::wstring& key, std::string& out) override {
        std::ifstream in(key, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    bool write(const std::wstring& key, const std::string& data) override {
        std::error_code ec;
        const fs::path p(key);
        if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
        errno = 0;
        std::ofstream out(key, std::ios::binary | std::ios::trunc);
        if (!out) {
            // NOTE: 失败原因必留痕——EACCES=权限/安全软件拦截，ENOENT=目录缺失……
            // （踩过：安全软件拦未签名 exe 写盘，dry 通过、真实落盘一律失败，错误必须点名）
            last_error_ = std::generic_category().message(errno ? errno : EIO);
            return false;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        const bool ok = out.good();
        if (!ok) last_error_ = "写入过程失败（流状态异常）";
        return ok;
    }

    std::string last_error() const override { return last_error_; }

    bool remove(const std::wstring& key) override {
        std::error_code ec;
        return fs::remove(fs::path(key), ec) && !ec;
    }

    bool exists(const std::wstring& key) override {
        std::error_code ec;
        return fs::exists(fs::path(key), ec) && !ec;
    }
};

// 内存实现（单元测试）。留出继承位：测试替身可覆盖 write 制造"写盘失败"的回滚场景。
class MemoryStore : public TextStore {
public:
    bool read(const std::wstring& key, std::string& out) override {
        const auto it = files_.find(key);
        if (it == files_.end()) return false;
        out = it->second;
        return true;
    }
    bool write(const std::wstring& key, const std::string& data) override {
        files_[key] = data;
        return true;
    }
    bool remove(const std::wstring& key) override { return files_.erase(key) != 0; }
    bool exists(const std::wstring& key) override { return files_.count(key) != 0; }

private:
    std::unordered_map<std::wstring, std::string> files_;
};

// ---- UTF-8 工具已收敛到 text.hpp：utf8_valid / trim_utf8 ----------------

}  // namespace vcws
