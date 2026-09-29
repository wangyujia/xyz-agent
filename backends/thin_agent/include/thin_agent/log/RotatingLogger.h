#pragma once
// RotatingLogBuf — 替换 std::cout.rdbuf() 实现自动轮转日志（带毫秒时间戳）
//
// 用法:
//   #include "thin_agent/log/RotatingLogger.h"
//   thin_agent::RotatingLogBuf log_buf("/path/to/app.log", 20*1024*1024);
//   auto* old = std::cout.rdbuf(&log_buf);  // 之后所有 std::cout 写入日志
//
// 特性:
//   - 启动轮转: 旧日志 → xxx_bak.log（旧 bak 删除）
//   - 运行时轮转: 每次 write 检测文件大小，超限自动切
//   - 线程安全 (std::mutex)
//   - 每行自动带毫秒时间戳 [HH:MM:SS.mmm]
//   - std::endl / std::flush 触发 fflush 立即落盘

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <streambuf>
#include <string>

namespace thin_agent {

class RotatingLogBuf : public std::streambuf {
public:
    /// @param log_path  日志文件完整路径（目录会自动创建）
    /// @param max_size  最大字节数，超过后触发轮转
    RotatingLogBuf(const std::string& log_path, size_t max_size)
        : log_path_(log_path),
          bak_path_(make_bak_path(log_path)),
          max_size_(max_size) {
        std::filesystem::create_directories(
            std::filesystem::path(log_path_).parent_path());
        startup_rotate();
        file_ = std::fopen(log_path_.c_str(), "a");
        // v0.54.24: **无缓冲**。原因（R94 实锤）：stdio 默认 4KB 缓冲下，一行若**跨 4096 边界**，
        // 会被拆成两次 write()；并发读者（测试 / tail -f / 日志采集器）因此能看到**半行**
        // （实测到 `… last_req=` 后换行再接 `ping` 这种撕裂）。改为 _IONBF 后：一次 xsputn（一行）
        // = 一次 write()，对普通文件而言内核串行化该 write ⇒ 读者只会看到"整行或没有"。
        // 代价：每行多一次系统调用（日志量级下可忽略；且热路径已改为整行单次写出）。
        if (file_) std::setvbuf(file_, nullptr, _IONBF, 0);
    }

    ~RotatingLogBuf() override {
        // v0.54.28: **必须持锁** —— 析构可能与仍在写日志的工作线程并发（此前无锁访问 pending_/file_，
        // 且调用了名字带 locked 的 check_write_locked ⇒ 数据竞争 + 违反该函数调用约定）。
        std::lock_guard<std::mutex> lock(mu_);
        // v0.54.24: 未终结的残留（无换行的尾段）在析构时作为一行落盘（补换行），
        // 既不产出半行，也不静默丢内容。
        if (file_ && !pending_.empty()) flush_pending_locked(true);
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

protected:
    int overflow(int c) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (c == EOF || !file_) return EOF;
        append_locked(static_cast<char>(c));
        return c;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (n <= 0 || !s) return 0;
        std::lock_guard<std::mutex> lock(mu_);
        if (!file_) return 0;
        for (std::streamsize i = 0; i < n; ++i) append_locked(s[i]);
        return n;
    }

    int sync() override {
        std::lock_guard<std::mutex> lock(mu_);
        if (file_) std::fflush(file_);
        return 0;
    }

private:
    static std::string make_bak_path(const std::string& log_path) {
        auto dot = log_path.rfind('.');
        if (dot != std::string::npos)
            return log_path.substr(0, dot) + "_bak" + log_path.substr(dot);
        return log_path + "_bak";
    }

    void startup_rotate() {
        // v0.54.29: 全用 ec 版 —— 日志路径**不得抛异常**（旧实现 exists/remove 是抛出版本，
        // 权限异常可穿透构造函数）。失败时同样**保留日志**（不 remove、不 rename）。
        std::error_code ec;
        if (!std::filesystem::exists(log_path_, ec)) return;
        if (std::filesystem::exists(bak_path_, ec))
            (void)std::filesystem::remove(bak_path_, ec);
        std::filesystem::rename(log_path_, bak_path_, ec);   // 失败则保持原文件不动（下次启动再试）
    }

    /// v0.54.27: 写者令牌 —— 每个**新线程**取一次（thread_local 初始化只跑一次），全局单调、**不复用**。
    static uint64_t writer_token() {
        static std::atomic<uint64_t> counter{0};
        static thread_local const uint64_t token = counter.fetch_add(1) + 1;
        return token;
    }

    /// v0.54.24（R94 根治）：**日志文件里只出现完整行**。
    /// 攒够一整行（遇到 '\n'）才一次性写出（时间戳与本行合并、单次 fwrite；配合 _IONBF ⇒ 单次 write）。
    /// 这样**无论调用方用多少段 `<<`**，并发读者（测试 / tail -f / 日志采集器）都不可能读到半行。
    /// 超长且始终无换行的内容（≥kMaxPendingBytes）为防内存无界，才落一份不完整数据（记档：病态输入）。
    void append_locked(char c) {
        // v0.54.27: **不同线程不互相"续行"**。pending_ 是所有写者共享的：若线程 A 写到一半（多段 `<<`），
        // 线程 B 接着写，就会把两行粘成一行（内容错乱）。故发现 pending_ 属于**别的线程**时，先把那半行
        // **补换行成行落盘**（宁可多一个换行，也不产出粘行/半行）。
        // 归属用**线程局部唯一令牌**，不能用 std::thread::id —— 线程结束后其 id 会被**复用**，
        // 新线程启动时会误判"这半行是我写的"⇒ 保护失效（本用例实测到 AAABBB 粘行）。
        const uint64_t me = writer_token();
        if (!pending_.empty() && pending_owner_token_ != me) flush_pending_locked(true);
        if (pending_.empty()) {
            pending_ = make_timestamp_locked();   // 行首：先攒时间戳
            pending_owner_token_ = me;
        }
        pending_.push_back(c);
        const bool overflow = (c != '\n' && pending_.size() >= kMaxPendingBytes);
        if (c != '\n' && !overflow) return;
        // 超长且始终无换行：**补换行**后成行落盘（保持"文件里只有完整行"这条不变量）
        flush_pending_locked(overflow);
    }

    /// 把 pending_ 成行落盘；append_newline=true 时补一个换行（用于"未终结"与"超长"两种情形）
    void flush_pending_locked(bool append_newline) {
        if (pending_.empty()) return;
        if (append_newline) pending_.push_back('\n');
        check_write_locked(pending_.size());
        // v0.54.29: **file_ 可能为 nullptr**（rotate_locked 里 fclose 后 fopen 失败，如磁盘满/权限）。
        // 此时直接 fwrite(nullptr) 是 UB（实测路线：崩溃）。故先判空：丢弃该行并返回（宁可丢一行日志，
        // 不可崩进程；条目已在 pending_ 里，不会与后续内容拼错）。
        if (file_ == nullptr) {
            pending_.clear();
            return;
        }
        const size_t w = std::fwrite(pending_.data(), 1, pending_.size(), file_);
        if (w < pending_.size()) {
            // 写失败：丢弃，避免半行残留与新内容拼接（如实丢弃优于产出错行）
            pending_.clear();
            return;
        }
        pending_.clear();
    }

    /// v0.54.24: `write_timestamp_locked()` → 仅**构造**时间戳文本，由调用方与正文合并后一次写出。
    std::string make_timestamp_locked() {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) %
                  1000;
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
        localtime_r(&t, &tm_buf);

        char ts[32];
        const int len = std::snprintf(ts, sizeof(ts), "[%02d:%02d:%02d.%03lld] ",
                                      tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                                      static_cast<long long>(ms.count()));
        return std::string(ts, static_cast<size_t>(len > 0 ? len : 0));
    }

    void check_write_locked(size_t about_to_write) {
        if (rotate_disabled_) return;   // v0.54.29: 轮转已失败 ⇒ 不再每行重试
        std::error_code ec;
        if (!std::filesystem::exists(log_path_, ec)) return;
        const auto sz = std::filesystem::file_size(log_path_, ec);
        if (ec) return;
        if (sz >= max_size_ || sz + about_to_write >= max_size_) rotate_locked();
    }

    void rotate_locked() {
        // v0.54.29: 轮转是**尽力而为**，绝不能因轮转失败而**丢日志**。
        // 旧实现的问题：`rename` 抛异常时兜底 `remove(log_path_)` ⇒ **把整份日志删掉**（数据丢失）；
        // 且失败后每次写都会再试一遍（每行一次重试风暴）。
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
        std::error_code ec;                    // v0.54.29: 全部用 ec 版（日志路径**不得抛异常**）
        bool rotated = false;
        if (std::filesystem::exists(log_path_, ec)) {
            if (std::filesystem::exists(bak_path_, ec))
                (void)std::filesystem::remove(bak_path_, ec);
            std::filesystem::rename(log_path_, bak_path_, ec);
            rotated = !ec;
        } else {
            rotated = true;                    // 没有旧文件 ⇒ 无需轮转
        }
        if (!rotated) {
            // rename 失败（如 bak 路径不可写/被占位）：**保留原日志继续追加**，并停用轮转，
            // 避免"每写一行就重试一次"。宁可日志不轮转，也不丢内容、不刷屏。
            rotate_disabled_ = true;
        }
        file_ = std::fopen(log_path_.c_str(), "a");
        if (file_ == nullptr) {
            std::fprintf(stderr, "[log] rotate: reopen failed, log writes dropped until restart\n");
        }
        need_ts_ = true;
    }

    std::string log_path_;
    std::string bak_path_;
    size_t max_size_;
    std::mutex mu_;
    std::FILE* file_ = nullptr;
    std::string pending_;   // v0.54.24: 未写完的当前行（只在换行时成行落盘，故文件里无半行）
    uint64_t pending_owner_token_ = 0;   // v0.54.27: 该半行归属写者（线程局部唯一令牌，防跨线程粘行）
    static constexpr size_t kMaxPendingBytes = 64 * 1024;
    bool need_ts_ = true;  // 下一行需要时间戳（启动或换行后）
    bool rotate_disabled_ = false;   // v0.54.29: 轮转失败后停用（避免每行重试 + 绝不删日志）
};

}  // namespace thin_agent
