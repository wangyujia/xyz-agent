#pragma once

#include <mutex>
#include <string>

/// 调试日志管理器：WebView 模式下的日志文件写入与轮转。
///
/// 用法：
///   DebugLogger::instance().init(log_dir);   // 启动时调用一次，触发启动轮转
///   DebugLogger::instance().write(msg);      // 每次写日志（线程安全）
///
/// 轮转策略：
///   - 启动时：debug.log 已存在 → 改名 debug_bak.log（旧 bak 删除）→ 新建 debug.log
///   - 运行时：每次 write 前检查 debug.log 大小；超过 10MB 则同启动轮转
class DebugLogger {
 public:
  static DebugLogger& instance();

  void init(const std::string& log_dir);
  void write(const std::string& msg);
  void close();

  DebugLogger(const DebugLogger&) = delete;
  DebugLogger& operator=(const DebugLogger&) = delete;

 private:
  DebugLogger() = default;
  ~DebugLogger();

  void rotate_locked();

  std::mutex mu_;
  std::string log_path_;
  std::string bak_path_;
  void* file_ = nullptr;

  static constexpr long long kMaxSize = 10LL * 1024 * 1024;  // 10MB
};
