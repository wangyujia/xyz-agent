#pragma once

#include <string>
#include <cstdint>

namespace thin_agent {

/// PTY 会话执行结果
struct PtyResult {
  std::string output;       // 完整 stdout+stderr 输出
  int exit_code = -1;       // 子进程退出码（-1=异常终止）
  int elapsed_ms = 0;       // 执行耗时
  bool timed_out = false;   // 是否因超时被 SIGKILL
};

/// 在 PTY 中执行命令，支持交互式 CLI（npm init / python REPL / git rebase -i 等）。
/// 使用 forkpty() 创建伪终端，通过 select() 循环读取输出。
///
/// @param command    要执行的命令（通过 /bin/sh -c 执行）
/// @param stdin_text 发送到子进程 stdin 的文本（可选，发送后关闭 stdin）
/// @param timeout_ms 超时毫秒数（0=无超时，默认 30000）
PtyResult pty_exec(const std::string& command,
                   const std::string& stdin_text = "",
                   int timeout_ms = 30000);

}  // namespace thin_agent
