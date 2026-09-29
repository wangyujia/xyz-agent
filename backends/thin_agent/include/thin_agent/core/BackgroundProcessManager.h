#pragma once

#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <cstdint>

namespace thin_agent {

/// 后台进程状态
struct BgProcessState {
  std::string id;           // 会话 ID
  int pid = -1;             // 子进程 PID
  int exit_code = -1;       // 退出码（-1=仍在运行）
  std::string output;       // 累积输出（环形缓冲区）
  bool running = false;     // 是否仍在运行
  int64_t started_at_ms = 0;
};

/// 后台进程管理器
///
/// 管理 fork()+pipe() 创建的后台进程，通过内部线程 select() 读取输出。
/// 线程安全：所有公共方法加锁。
class BackgroundProcessManager {
 public:
  static BackgroundProcessManager& instance();

  /// 启动后台进程。
  /// @param command   要执行的命令（通过 /bin/sh -c 执行）
  /// @param timeout_ms 超时（毫秒，默认 0=无超时）
  /// @return session_id（用于后续 poll/wait/kill）
  std::string start(const std::string& command, int timeout_ms = 0);

  /// 轮询进程状态 + 最新输出。
  BgProcessState poll(const std::string& session_id);

  /// 阻塞等待进程结束。
  BgProcessState wait(const std::string& session_id, int timeout_ms = 0);

  /// 终止进程（SIGTERM → 2s → SIGKILL）。
  BgProcessState kill(const std::string& session_id);

  /// 向进程 stdin 写入数据。
  /// 写入子进程 stdin。**返回是否写成功**（v0.54.22 起）。
  /// 注意历史缺陷：本方法曾经写的是 stdout 的读端（恒 EBADF）且吞掉错误，调用方却照报成功。
  bool write_stdin(const std::string& session_id, const std::string& data);

 private:
  BackgroundProcessManager();
  ~BackgroundProcessManager();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace thin_agent
