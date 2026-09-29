#include "thin_agent/core/PseudoTerminal.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// v0.54.19: 伪终端两平台差异极大（POSIX forkpty vs Windows ConPTY）。按本仓约定**文件内隔离**。
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // ConPTY 要求 Windows 10 1809+
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#ifdef _MSC_VER
#include <consoleapi.h>   // MSVC：ConPTY 声明来自 SDK
#else
// MinGW-w64 头**不含 ConPTY**（实测 HPCON 未声明）⇒ 按官方 ABI 手工声明（签名与 SDK 一致）。
extern "C" {
typedef void* HPCON;
HRESULT WINAPI CreatePseudoConsole(COORD size, HANDLE hInput, HANDLE hOutput, DWORD dwFlags,
                                   HPCON* phPC);
void WINAPI ClosePseudoConsole(HPCON hPC);
}
#endif
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016   // 官方文档值（ProcThreadAttributeValue(22,…)）
#endif
#else
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <pty.h>    // forkpty
#elif defined(__APPLE__)
#include <util.h>   // forkpty (macOS)
#endif
#endif

namespace thin_agent {

PtyResult pty_exec(const std::string& command,
                   const std::string& stdin_text,
                   int timeout_ms) {
  PtyResult result;
  auto t0 = std::chrono::steady_clock::now();

#ifdef _WIN32
  // v0.54.19: Windows 分支用 **ConPTY**（`CreatePseudoConsole` + 属性表 `CreateProcessA`）复刻
  // forkpty 语义：伪终端里跑 `cmd.exe /c <command>`，父侧写 stdin、读输出到 EOF 或超时；
  // 超时 `TerminateProcess`（POSIX 是 TERM→2s→KILL，Windows 无信号语义，如实标注）。
  // 差异（如实标注）：POSIX `forkpty` 一步给出 master fd + 子 pid；ConPTY 需"建 PTY → 属性表
  // 建进程"两步，且**要 Windows 10 1809+**（失败路径如实返回错误文本，不静默降级）。
  HANDLE pty_in = nullptr, mine_in = nullptr, mine_out = nullptr, pty_out = nullptr;
  if (!CreatePipe(&pty_in, &mine_in, nullptr, 0) || !CreatePipe(&mine_out, &pty_out, nullptr, 0)) {
    result.output = "CreatePipe failed: " + std::to_string(GetLastError());
    if (pty_in) CloseHandle(pty_in);
    if (mine_in) CloseHandle(mine_in);
    if (mine_out) CloseHandle(mine_out);
    if (pty_out) CloseHandle(pty_out);
    return result;
  }
  COORD pty_size{120, 30};
  HPCON hpc = nullptr;
  HRESULT hr = CreatePseudoConsole(pty_size, pty_in, pty_out, 0, &hpc);
  if (FAILED(hr) || hpc == nullptr) {
    result.output = "CreatePseudoConsole failed (需要 Windows 10 1809+): " + std::to_string(hr);
    CloseHandle(pty_in); CloseHandle(mine_in); CloseHandle(mine_out); CloseHandle(pty_out);
    return result;
  }

  STARTUPINFOEXA siex{};
  siex.StartupInfo.cb = sizeof(siex);
  SIZE_T attr_bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_bytes);
  siex.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
      HeapAlloc(GetProcessHeap(), 0, attr_bytes));
  bool attr_ok = siex.lpAttributeList != nullptr &&
                 InitializeProcThreadAttributeList(siex.lpAttributeList, 1, 0, &attr_bytes) != FALSE &&
                 UpdateProcThreadAttribute(siex.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                           hpc, sizeof(hpc), nullptr, nullptr) != FALSE;
  if (!attr_ok) {
    result.output = "pseudoconsole attribute setup failed: " + std::to_string(GetLastError());
    if (siex.lpAttributeList) HeapFree(GetProcessHeap(), 0, siex.lpAttributeList);
    ClosePseudoConsole(hpc);
    CloseHandle(pty_in); CloseHandle(mine_in); CloseHandle(mine_out); CloseHandle(pty_out);
    return result;
  }

  PROCESS_INFORMATION pi{};
  std::string cmdline = "cmd.exe /c " + command;
  BOOL started = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                                EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                                nullptr, nullptr, &siex.StartupInfo, &pi);
  DeleteProcThreadAttributeList(siex.lpAttributeList);
  HeapFree(GetProcessHeap(), 0, siex.lpAttributeList);
  // ConPTY 已持有这两端 ⇒ 父进程必须关掉，否则读端永不 EOF
  CloseHandle(pty_in);
  CloseHandle(pty_out);
  if (!started) {
    result.output = "CreateProcess failed: " + std::to_string(GetLastError());
    ClosePseudoConsole(hpc);
    CloseHandle(mine_in);
    CloseHandle(mine_out);
    return result;
  }
  CloseHandle(pi.hThread);

  std::string output;
  char buf[4096];
  bool timed_out = false;

  // 先发 stdin（多数程序需先收到输入才产出）
  if (!stdin_text.empty()) {
    DWORD written = 0;
    WriteFile(mine_in, stdin_text.data(), static_cast<DWORD>(stdin_text.size()), &written, nullptr);
  }

  while (true) {
    DWORD avail = 0;
    if (!PeekNamedPipe(mine_out, nullptr, 0, nullptr, &avail, nullptr)) break;  // 管道已断 = EOF
    if (avail > 0) {
      DWORD got = 0;
      if (!ReadFile(mine_out, buf, sizeof(buf), &got, nullptr) || got == 0) break;
      output.append(buf, static_cast<size_t>(got));
      continue;
    }
    if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
      // 子进程已退出：把管道里剩余的输出读净
      for (int pass = 0; pass < 2; ++pass) {
        DWORD left = 0;
        if (!PeekNamedPipe(mine_out, nullptr, 0, nullptr, &left, nullptr) || left == 0) break;
        DWORD got = 0;
        if (!ReadFile(mine_out, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        output.append(buf, static_cast<size_t>(got));
      }
      break;
    }
    if (timeout_ms > 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0).count();
      if (elapsed >= timeout_ms) {
        timed_out = true;
        break;
      }
    }
  }

  if (timed_out) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 2000);
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);

  CloseHandle(mine_in);
  CloseHandle(mine_out);
  CloseHandle(pi.hProcess);
  ClosePseudoConsole(hpc);

  result.exit_code = static_cast<int>(code);
  result.output = std::move(output);
  result.timed_out = timed_out;

  auto t1 = std::chrono::steady_clock::now();
  result.elapsed_ms = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

  return result;
#else
  int master_fd;
  pid_t pid = ::forkpty(&master_fd, nullptr, nullptr, nullptr);

  if (pid == -1) {
    result.output = "forkpty failed: " + std::string(strerror(errno));
    return result;
  }

  if (pid == 0) {
    // ── 子进程 ──
    const char* shell = "/bin/sh";
    ::execlp(shell, shell, "-c", command.c_str(), nullptr);
    ::_exit(127);  // execlp 失败
  }

  // ── 父进程：先发送 stdin（如果有），然后 select() 循环读取输出 ──
  std::string output;
  char buf[4096];
  bool stdin_sent = false;
  bool timed_out = false;

  // 立即发送 stdin（大部分程序需要先收到输入才会产生输出）
  if (!stdin_text.empty()) {
    if (::write(master_fd, stdin_text.c_str(), stdin_text.size()) < 0) {}  // v0.53.16: 消费返回值
    stdin_sent = true;
  }

  while (true) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(master_fd, &fds);

    struct timeval tv;
    struct timeval* ptv = nullptr;
    if (timeout_ms > 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0).count();
      long remaining = timeout_ms - static_cast<long>(elapsed);
      if (remaining <= 0) {
        timed_out = true;
        break;
      }
      tv.tv_sec = remaining / 1000;
      tv.tv_usec = (remaining % 1000) * 1000;
      ptv = &tv;
    }

    int rc = ::select(master_fd + 1, &fds, nullptr, nullptr, ptv);

    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (rc == 0) {
      // 超时
      timed_out = true;
      break;
    }

    ssize_t n = ::read(master_fd, buf, sizeof(buf));
    if (n > 0) {
      output.append(buf, static_cast<size_t>(n));
    } else if (n == 0) {
      // EOF — 子进程关闭了 PTY
      break;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      break;
    }
  }

  // 超时 → 先 SIGTERM，等待 2s，再 SIGKILL
  if (timed_out) {
    ::kill(pid, SIGTERM);
    // 等待最多 2 秒让子进程优雅退出
    for (int i = 0; i < 20; ++i) {
      int status;
      if (::waitpid(pid, &status, WNOHANG) > 0) break;
      ::usleep(100000);  // 100ms
    }
    ::kill(pid, SIGKILL);
  }

  // 等待子进程退出并读取剩余输出
  ::close(master_fd);
  int status;
  ::waitpid(pid, &status, 0);

  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  }

  result.output = std::move(output);
  result.timed_out = timed_out;

  auto t1 = std::chrono::steady_clock::now();
  result.elapsed_ms = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

  return result;
#endif  // _WIN32
}

}  // namespace thin_agent
