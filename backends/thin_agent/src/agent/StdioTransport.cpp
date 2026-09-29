#include "thin_agent/agent/StdioTransport.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <map>
#include <signal.h>
#include <string>
#include <vector>

// v0.54.18: 子进程 stdio 两平台差异极大（POSIX fork/pipe/dup2/execvp/waitpid vs Win32
// CreateProcess/管道句柄）。按本仓约定**文件内隔离**（不跨文件封装）。
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
typedef int ta_ssize_t;            // Windows CRT 的 _read/_write 返回 int
#define TA_READ  ::_read
#define TA_WRITE ::_write
#define TA_CLOSE ::_close
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
typedef ssize_t ta_ssize_t;
#define TA_READ  ::read
#define TA_WRITE ::write
#define TA_CLOSE ::close
#endif
#include "thin_agent/log/LogEvent.h"

namespace thin_agent {
namespace agent {

#ifdef _WIN32
namespace {
/// argv → Windows 命令行（按 CreateProcess 解析规则转义：反斜杠仅在其后紧跟引号时加倍）
std::string win_quote_arg(const std::string& a) {
  if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
  std::string out = "\"";
  int backslashes = 0;
  for (char c : a) {
    if (c == '\\') { ++backslashes; continue; }
    if (c == '"') {
      out.append(static_cast<size_t>(backslashes * 2 + 1), '\\');
      out.push_back('"');
      backslashes = 0;
      continue;
    }
    out.append(static_cast<size_t>(backslashes), '\\');
    backslashes = 0;
    out.push_back(c);
  }
  out.append(static_cast<size_t>(backslashes * 2), '\\');
  out.push_back('"');
  return out;
}

std::string win_build_cmdline(const std::vector<std::string>& args) {
  std::string cl;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) cl.push_back(' ');
    cl += win_quote_arg(args[i]);
  }
  return cl;
}

/// CreateProcess 需**整块**环境（不能像 POSIX 那样逐项 setenv）⇒ 复制当前环境后追加注入项。
/// 无注入项时返回空串（⇒ 传 nullptr = 继承父进程环境，零开销）。
std::string win_build_env(const std::map<std::string, std::string>& extra) {
  if (extra.empty()) return std::string();
  std::string blk;
  LPCH cur = GetEnvironmentStringsA();
  if (cur) {
    for (LPCH p = cur; *p; p += std::strlen(p) + 1) {
      blk.append(p);
      blk.push_back('\0');
    }
    FreeEnvironmentStringsA(cur);
  }
  for (const auto& kv : extra) {
    blk += kv.first;
    blk.push_back('=');
    blk += kv.second;
    blk.push_back('\0');
  }
  blk.push_back('\0');
  return blk;
}
}  // namespace
#endif  // _WIN32

StdioTransport::~StdioTransport() {
  disconnect();
}

bool StdioTransport::connect(const std::string& endpoint) {
  if (child_pid_ > 0) {
    log_event("mcp-transport", LogLevel::Warn,
              "already connected, disconnect first");
    return false;
  }

  // v0.53.7: 市面 mcpServers 标准格式——command+args 分离优先。
  // extra_args_ 非空：endpoint 即 command 本体（不再空格切分，参数显式
  // 无 shell 引号歧义）；为空：沿用旧空格切分（向后兼容存量配置）。
  std::vector<std::string> args;
  if (!extra_args_.empty()) {
    if (endpoint.empty()) return false;
    args.push_back(endpoint);
    args.insert(args.end(), extra_args_.begin(), extra_args_.end());
  } else {
    // Parse command: split by spaces (simple, no shell quoting)
    std::string current;
    for (char c : endpoint) {
      if (c == ' ') {
        if (!current.empty()) {
          args.push_back(current);
          current.clear();
        }
      } else {
        current += c;
      }
    }
    if (!current.empty()) args.push_back(current);
    if (args.empty()) return false;
  }

  // Build argv array
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(&a[0]);
  argv.push_back(nullptr);

#ifdef _WIN32
  // v0.54.18: Windows 分支——语义对齐 POSIX 版（子进程 stdin/stdout 接管道、stderr 丢弃、
  // extra_env_ 注入、启动失败如实 false 且不残留句柄）。实现差异（如实标注）：
  //  · POSIX 的"exec 状态管道 + CLOEXEC"技巧在 Windows **不需要**：`CreateProcessA` 创建与
  //    程序解析是一步，当场返回成败 ⇒ 无 errno 管道；
  //  · 管道句柄经 `_open_osfhandle` 转 CRT fd（int）⇒ `stdin_fd_`/`stdout_fd_` 仍是 int，
  //    类接口与收发路径不变（仅 `read/write/close` 换 `_read/_write/_close`）；
  //  · 子进程命令行走 Windows 引用规则拼 argv（CreateProcess 只吃命令行字符串）；
  //  · 环境块整块构造（见 win_build_env）。
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  HANDLE in_rd = nullptr, in_wr = nullptr, out_rd = nullptr, out_wr = nullptr;
  bool pipes_ok = CreatePipe(&in_rd, &in_wr, &sa, 0) && CreatePipe(&out_rd, &out_wr, &sa, 0);
  if (!pipes_ok) {
    log_event("mcp-transport", LogLevel::Error, "CreatePipe failed",
              {{"error", GetLastError()}});
    if (in_rd) CloseHandle(in_rd);
    if (in_wr) CloseHandle(in_wr);
    if (out_rd) CloseHandle(out_rd);
    if (out_wr) CloseHandle(out_wr);
    return false;
  }
  // 父进程持有的两端不可继承（否则子进程退出后管道不 EOF/不关闭）
  SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);

  HANDLE nul_h = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  if (nul_h == INVALID_HANDLE_VALUE) {
    CloseHandle(in_rd); CloseHandle(in_wr); CloseHandle(out_rd); CloseHandle(out_wr);
    log_event("mcp-transport", LogLevel::Error, "open NUL failed",
              {{"error", GetLastError()}});
    return false;
  }

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_rd;
  si.hStdOutput = out_wr;
  si.hStdError = nul_h;   // 对齐 POSIX：stderr 丢弃
  PROCESS_INFORMATION pi{};
  std::string cmdline = win_build_cmdline(args);
  std::string env_block = win_build_env(extra_env_);
  BOOL started = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                CREATE_NO_WINDOW,
                                env_block.empty() ? nullptr : env_block.data(),
                                nullptr, &si, &pi);
  CloseHandle(in_rd);
  CloseHandle(out_wr);
  CloseHandle(nul_h);
  if (!started) {
    CloseHandle(in_wr);
    CloseHandle(out_rd);
    log_event("mcp-transport", LogLevel::Error, "CreateProcess failed",
              {{"cmd", args.empty() ? std::string() : args[0]},
               {"error", GetLastError()}});
    return false;
  }
  CloseHandle(pi.hThread);

  int fd_in = ::_open_osfhandle(reinterpret_cast<intptr_t>(in_wr), _O_WRONLY | _O_BINARY);
  int fd_out = ::_open_osfhandle(reinterpret_cast<intptr_t>(out_rd), _O_RDONLY | _O_BINARY);
  if (fd_in < 0 || fd_out < 0) {
    // fd 转换失败：回收子进程 + 关句柄，绝不留下"半连接"状态
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    if (fd_in < 0) CloseHandle(in_wr); else ::_close(fd_in);
    if (fd_out < 0) CloseHandle(out_rd); else ::_close(fd_out);
    log_event("mcp-transport", LogLevel::Error, "_open_osfhandle failed",
              {{"error", GetLastError()}});
    return false;
  }

  child_pid_ = static_cast<int>(pi.dwProcessId);
  child_handle_ = pi.hProcess;
  stdin_fd_ = fd_in;
  stdout_fd_ = fd_out;
  log_event("mcp-transport", LogLevel::Info, "connected",
            {{"cmd", args[0]}, {"pid", child_pid_}});
  return true;
#else
  // Create pipes: parent writes to child_stdin[1], child reads from child_stdin[0]
  //                child writes to child_stdout[1], parent reads from child_stdout[0]
  int child_stdin[2], child_stdout[2];
  if (pipe(child_stdin) == -1 || pipe(child_stdout) == -1) {
    log_event("mcp-transport", LogLevel::Error, "pipe failed",
              {{"error", strerror(errno)}});
    return false;
  }

  // v0.50.1: exec 状态管道 — 子进程 execvp 成功后 CLOEXEC 自动关闭写端，
  // 父进程读到 EOF；exec 失败则读到 errno 字节。修复 fork 成功但程序
  // 不存在时 connect 误报 true 的缺陷。
  int exec_status[2];
  if (pipe(exec_status) == -1) {
    log_event("mcp-transport", LogLevel::Error, "pipe failed",
              {{"error", strerror(errno)}});
    close(child_stdin[0]); close(child_stdin[1]);
    close(child_stdout[0]); close(child_stdout[1]);
    return false;
  }
  ::fcntl(exec_status[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(exec_status[1], F_SETFD, FD_CLOEXEC);

  pid_t pid = fork();
  if (pid == -1) {
    log_event("mcp-transport", LogLevel::Error, "fork failed",
              {{"error", strerror(errno)}});
    close(child_stdin[0]); close(child_stdin[1]);
    close(child_stdout[0]); close(child_stdout[1]);
    close(exec_status[0]); close(exec_status[1]);
    return false;
  }

  if (pid == 0) {
    // --- Child process ---
    // Redirect stdin from parent's pipe
    dup2(child_stdin[0], STDIN_FILENO);
    dup2(child_stdout[1], STDOUT_FILENO);

    // Close all pipe ends in child
    close(child_stdin[0]); close(child_stdin[1]);
    close(child_stdout[0]); close(child_stdout[1]);
    close(exec_status[0]);

    // Close stderr to avoid noise (or redirect to /dev/null)
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    // v0.53.7: 环境变量注入（fork 后 execvp 前——仅本子进程可见；
    // 未设置的变量自然继承父进程环境）
    for (const auto& [k, v] : extra_env_) ::setenv(k.c_str(), v.c_str(), 1);

    execvp(argv[0], argv.data());
    // execvp only returns on error — 写 errno 给父进程（注意 stderr 已被
    // 重定向到 /dev/null，不能再依赖 cerr 传递）
    int err = errno;
    ssize_t w = ::write(exec_status[1], &err, sizeof(err));
    (void)w;
    _exit(127);
  }

  // --- Parent process ---
  child_pid_ = pid;
  stdin_fd_ = child_stdin[1];   // write to child's stdin
  stdout_fd_ = child_stdout[0]; // read from child's stdout

  // Close unused pipe ends in parent
  close(child_stdin[0]);
  close(child_stdout[1]);
  close(exec_status[1]);

  // 等待 exec 结果：EOF=成功启动；errno 字节=exec 失败
  int child_errno = 0;
  ssize_t r = ::read(exec_status[0], &child_errno, sizeof(child_errno));
  close(exec_status[0]);
  if (r > 0) {
    // exec 失败 — 回收子进程并清理
    log_event("mcp-transport", LogLevel::Error, "execvp failed",
              {{"error", strerror(child_errno)}});
    int status = 0;
    waitpid(pid, &status, 0);
    child_pid_ = -1;
    close(stdin_fd_); stdin_fd_ = -1;
    close(stdout_fd_); stdout_fd_ = -1;
    return false;
  }

  log_event("mcp-transport", LogLevel::Info, "connected",
            {{"cmd", args[0]}, {"pid", pid}});
  return true;
#endif  // _WIN32
}

std::string StdioTransport::send(const std::string& request) {
  if (child_pid_ <= 0 || stdin_fd_ < 0 || stdout_fd_ < 0) {
    return R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"not connected"}})";
  }

  // Write request + newline delimiter (JSON-RPC framing)
  std::string framed = request;
  if (framed.empty() || framed.back() != '\n') framed += '\n';

  ta_ssize_t written = TA_WRITE(stdin_fd_, framed.data(), framed.size());
  if (written != static_cast<ta_ssize_t>(framed.size())) {
    log_event("mcp-transport", LogLevel::Error, "write failed",
              {{"written", written}, {"expect", framed.size()}});
    return R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"write failed"}})";
  }

  // Read response — line-delimited JSON-RPC
  // v0.53.7: 上限 64KB→8MB（对齐 code_exec 输出上限）。真 server
  // （npx filesystem list_directory）单响应可超 64KB，旧上限截断半截
  // JSON → parse error（实测复现）。buffer 单次 read 仍 64KB 分块累积。
  std::array<char, 65536> buf;
  std::string response;
  constexpr ta_ssize_t kMaxResponse = 8 * 1024 * 1024;

  while (response.size() < static_cast<size_t>(kMaxResponse)) {
    ta_ssize_t n = TA_READ(stdout_fd_, buf.data(), buf.size() - 1);
    if (n <= 0) {
      if (n == 0) {
        log_event("mcp-transport", LogLevel::Info, "child closed stdout",
                  {{"pid", child_pid_}});
      } else {
        log_event("mcp-transport", LogLevel::Error, "read error",
                  {{"error", strerror(errno)}});
      }
      break;
    }
    buf[n] = '\0';
    response.append(buf.data(), n);

    // Check if we have a complete JSON-RPC response (ends with \n)
    if (response.find('\n') != std::string::npos) {
      // Extract first line (JSON-RPC is line-delimited)
      size_t nl = response.find('\n');
      std::string first_line = response.substr(0, nl);
      return first_line;
    }
  }

  if (response.empty()) {
    return R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"no response"}})";
  }
  return response;
}

void StdioTransport::disconnect() {
#ifdef _WIN32
  // v0.54.18: Windows 分支。无 SIGTERM 语义 ⇒ 先关管道（子进程 read 端 EOF 会自行退出，
  // 这是 MCP stdio server 的正常退路），2s 内未退则 TerminateProcess 兜底。
  if (child_pid_ > 0) {
    log_event("mcp-transport", LogLevel::Info, "disconnecting child",
              {{"pid", child_pid_}});
    if (stdin_fd_ >= 0) { TA_CLOSE(stdin_fd_); stdin_fd_ = -1; }
    if (stdout_fd_ >= 0) { TA_CLOSE(stdout_fd_); stdout_fd_ = -1; }
    if (child_handle_) {
      HANDLE h = static_cast<HANDLE>(child_handle_);
      if (WaitForSingleObject(h, 2000) != WAIT_OBJECT_0) {
        TerminateProcess(h, 1);
        WaitForSingleObject(h, 5000);
      }
      CloseHandle(h);
      child_handle_ = nullptr;
    }
    child_pid_ = -1;
    log_event("mcp-transport", LogLevel::Info, "disconnected");
  }
#else
  if (child_pid_ > 0) {
    log_event("mcp-transport", LogLevel::Info, "disconnecting child",
            {{"pid", child_pid_}});

    // Close pipes first
    if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; }
    if (stdout_fd_ >= 0) { close(stdout_fd_); stdout_fd_ = -1; }

    // Send SIGTERM, wait briefly, then SIGKILL if needed
    kill(child_pid_, SIGTERM);
    int status = 0;
    // Wait up to 2 seconds
    for (int i = 0; i < 20; ++i) {
      pid_t r = waitpid(child_pid_, &status, WNOHANG);
      if (r == child_pid_) break;
      if (r == -1) break;
      usleep(100000);  // 100ms
    }
    // If still alive, force kill
    if (waitpid(child_pid_, &status, WNOHANG) == 0) {
      kill(child_pid_, SIGKILL);
      waitpid(child_pid_, &status, 0);
    }
    child_pid_ = -1;
    log_event("mcp-transport", LogLevel::Info, "disconnected");
  }
#endif  // _WIN32
}

}  // namespace agent
}  // namespace thin_agent
