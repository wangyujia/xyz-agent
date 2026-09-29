#include "thin_agent/core/BackgroundProcessManager.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// v0.54.18: 后台进程监控两平台差异极大（POSIX select+fork/exec+waitpid/kill vs Win32
// PeekNamedPipe+CreateProcess+WaitForSingleObject/TerminateProcess）。按本仓约定**文件内隔离**。
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#define TA_READ  ::_read
#define TA_CLOSE ::_close
#define TA_WRITE ::_write
#else
#include <fcntl.h>   // v0.54.27: O_NONBLOCK（stdin 写端非阻塞）
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#define TA_READ  ::read
#define TA_CLOSE ::close
#define TA_WRITE ::write
#endif

namespace thin_agent {

// v0.54.27: Windows 侧单次 stdin 写入上限（匿名管道写端无"可写等待"⇒ 超过容量必然阻塞 ⇒ 硬拒）。
// v0.54.28: POSIX 侧不再硬拒 —— 改为**有界排空**（见 write_stdin），大载荷在子进程正常读时可完整送达。
#ifdef _WIN32
constexpr size_t kMaxStdinWriteBytes = 60 * 1024;   // POSIX 分支不使用（保留以避免平台宏噪音）
#else
// 有界排空总期限：子进程正常读时通常**一遍写完**（用不到它）；只有子进程不读、管道已满时才等待，
// 最长等这么久后**如实返回 false**，绝不无限阻塞（持锁时长因此有上界）。
constexpr int kStdinWriteDeadlineMs = 250;
#endif

// Proc 句柄访问（POSIX 无句柄概念 ⇒ 恒 nullptr；由 ta_* helper 按平台取用）
#ifdef _WIN32
#define TA_PROC_HANDLE(p) ((p)->handle)
#else
#define TA_PROC_HANDLE(p) (nullptr)
#endif

namespace {
// 两平台统一的"探测子进程是否已退出"（POSIX: waitpid WNOHANG / Windows: WaitForSingleObject(0)）
// 与"杀子进程"（POSIX: SIGTERM→SIGKILL / Windows: TerminateProcess，**无信号语义**）。
// 参数按平台各用一个：pid（POSIX）/ handle（Windows）。
inline bool ta_try_reap(int pid, void* handle, int* exit_code) {
#ifdef _WIN32
  (void)pid;
  if (!handle) return false;
  HANDLE h = static_cast<HANDLE>(handle);
  if (WaitForSingleObject(h, 0) != WAIT_OBJECT_0) return false;
  DWORD code = 0;
  if (GetExitCodeProcess(h, &code)) *exit_code = static_cast<int>(code);
  return true;
#else
  (void)handle;
  int status = 0;
  if (::waitpid(pid, &status, WNOHANG) <= 0) return false;
  if (WIFEXITED(status)) *exit_code = WEXITSTATUS(status);
  return true;
#endif
}

inline void ta_kill_proc(int pid, void* handle, bool force) {
#ifdef _WIN32
  (void)pid; (void)force;   // Windows 无 TERM/KILL 两段语义 ⇒ 直接终止（如实标注）
  if (handle) TerminateProcess(static_cast<HANDLE>(handle), 1);
#else
  (void)handle;
  ::kill(pid, force ? SIGKILL : SIGTERM);
#endif
}
}  // namespace

struct BackgroundProcessManager::Impl {
  struct Proc {
    std::string id;
    int pid = -1;
#ifdef _WIN32
    void* handle = nullptr;    // v0.54.18: 进程句柄（WaitForSingleObject/TerminateProcess）
#endif
    int pipe_fd = -1;          // 读取子进程 stdout 的 fd
    // v0.54.22: 写子进程 stdin 的 fd（两平台皆为 CRT fd；-1 = 不可写/已关闭）。
    // 背景：此前 `write_stdin` 写的是 **pipe_fd（stdout 的读端）** ⇒ 实证
    // `write(read_end) = -1 errno=9 EBADF`，即该功能**从未成功过**，而失败被
    // `if (::write(...) < 0) {}` 吞掉、工具却回 `success:true "wrote N bytes"`（对 LLM 说谎）。
    int stdin_fd = -1;
    std::string output;        // 累积输出
    std::atomic<int> exit_code{-1};
    std::atomic<bool> running{false};
    int64_t started_at_ms = 0;
    int timeout_ms = 0;
  };

  std::mutex mu;
  std::map<std::string, std::shared_ptr<Proc>> procs;
  std::atomic<int> seq{0};

  // 监控线程：select() 所有活跃进程的 pipe
  std::unique_ptr<std::thread> monitor_thread;
  std::atomic<bool> stop_monitor{false};

  void monitor_loop() {
    while (!stop_monitor.load(std::memory_order_relaxed)) {
      // 收集活跃进程
      std::vector<std::shared_ptr<Proc>> active;
      {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& [id, p] : procs) {
          // v0.54.8 (R93): **进程退出后仍要继续 drain**。此前只收集 running==true 的进程，
          // 而 wait()/kill() 一发现子进程退出就把 running 置 false ⇒ 管道里还没读走的尾巴
          // **永远不会被读**（不是延迟，是永久丢失）⇒ 快命令返回 "exit_code=0 + output 为空"。
          // 实测：unit_pty_background 在负载下偶发 `bg echo output` 失败（25 次命中 1 次）。
          // 改为"只要管道还开着就继续 drain"，EOF 时下面会把 pipe_fd 置 -1（自然退出 active）。
          if (p->pipe_fd >= 0) active.push_back(p);
        }
      }

      if (active.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }

      // 等可读数据（500ms 周期）
#ifdef _WIN32
      // v0.54.18: 管道句柄**不可 select**（Windows select 只支持 socket）⇒ 用 PeekNamedPipe 轮询：
      // 有可读字节才读；ERROR_BROKEN_PIPE = 写端已关 = 原 POSIX 的 EOF 语义。
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      bool any_readable = false;
      for (auto& p : active) {
        HANDLE h = reinterpret_cast<HANDLE>(::_get_osfhandle(p->pipe_fd));
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
          std::lock_guard<std::mutex> lk(mu);
          TA_CLOSE(p->pipe_fd);
          p->pipe_fd = -1;
          continue;
        }
        if (avail > 0) any_readable = true;
      }
      int rc = any_readable ? 1 : 0;
#else
      fd_set fds;
      FD_ZERO(&fds);
      int max_fd = -1;
      for (auto& p : active) {
        FD_SET(p->pipe_fd, &fds);
        if (p->pipe_fd > max_fd) max_fd = p->pipe_fd;
      }

      struct timeval tv = {0, 500000};  // 500ms
      int rc = ::select(max_fd + 1, &fds, nullptr, nullptr, &tv);
#endif

      if (rc > 0) {
        char buf[4096];
        for (auto& p : active) {
#ifdef _WIN32
          // Windows：只读"已确认可读"的管道（PeekNamedPipe 已在上面筛过）
          HANDLE h2 = reinterpret_cast<HANDLE>(::_get_osfhandle(p->pipe_fd));
          DWORD avail2 = 0;
          const bool ready = PeekNamedPipe(h2, nullptr, 0, nullptr, &avail2, nullptr) && avail2 > 0;
#else
          const bool ready = FD_ISSET(p->pipe_fd, &fds);
#endif
          if (ready) {
            int n = static_cast<int>(TA_READ(p->pipe_fd, buf, sizeof(buf)));
            if (n > 0) {
              std::lock_guard<std::mutex> lk(mu);
              p->output.append(buf, static_cast<size_t>(n));
              // 限制输出大小（64KB 环形）
              if (p->output.size() > 65536) {
                p->output = p->output.substr(p->output.size() - 32768);
              }
            } else {
              // EOF — 子进程关闭了 stdout
              std::lock_guard<std::mutex> lk(mu);
              TA_CLOSE(p->pipe_fd);
              p->pipe_fd = -1;
            }
          }
        }
      }

      // 检查超时（v0.54.8: 加 running 门——drain 阶段会保留已退出的进程在 active 里，
      // 对已回收的 pid 发信号既无意义、又可能误杀被内核复用的新 pid）
      auto now = std::chrono::steady_clock::now();
      for (auto& p : active) {
        if (p->running.load() && p->timeout_ms > 0) {
          auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
              now.time_since_epoch()).count() - p->started_at_ms;
          if (elapsed > p->timeout_ms) {
#ifdef _WIN32
            ta_kill_proc(p->pid, p->handle, true);
#else
            ta_kill_proc(p->pid, nullptr, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            ta_kill_proc(p->pid, nullptr, true);
#endif
          }
        }
      }
    }
  }
};

BackgroundProcessManager::BackgroundProcessManager()
    : impl_(std::make_unique<Impl>()) {
  impl_->monitor_thread = std::make_unique<std::thread>(
      [this] { impl_->monitor_loop(); });
}

BackgroundProcessManager::~BackgroundProcessManager() {
  impl_->stop_monitor.store(true);
  if (impl_->monitor_thread && impl_->monitor_thread->joinable()) {
    impl_->monitor_thread->join();
  }
}

BackgroundProcessManager& BackgroundProcessManager::instance() {
  static BackgroundProcessManager mgr;
  return mgr;
}

std::string BackgroundProcessManager::start(const std::string& command,
                                             int timeout_ms) {
#ifdef _WIN32
  // v0.54.18: Windows 分支——`CreateProcessA("cmd.exe /c <command>")`，stdout+stderr **合并**到
  // 同一管道（与 POSIX 版一致）。无 fork/exec/进程组概念 ⇒ 无 setpgid 竞态处理。
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return "";
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  // v0.54.22: stdin 管道（父持写端、子持读端）——替换此前"stdin 指向 NUL"的临时方案
  HANDLE ts_rd = nullptr, ts_wr = nullptr;
  if (!CreatePipe(&ts_rd, &ts_wr, &sa, 0)) {
    CloseHandle(rd);
    CloseHandle(wr);
    return "";
  }
  SetHandleInformation(ts_wr, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = ts_rd;
  si.hStdOutput = wr;
  si.hStdError = wr;   // stdout+stderr 合并
  PROCESS_INFORMATION pi{};
  std::string cmdline = "cmd.exe /c " + command;
  BOOL started = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(wr);
  CloseHandle(ts_rd);   // 子进程已持有读端
  if (!started) {
    CloseHandle(rd);
    CloseHandle(ts_wr);
    return "";
  }
  CloseHandle(pi.hThread);
  int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(rd), _O_RDONLY | _O_BINARY);
  int fd_in = ::_open_osfhandle(reinterpret_cast<intptr_t>(ts_wr), _O_WRONLY | _O_BINARY);
  // v0.54.27: Windows 侧把写端设为 **PIPE_NOWAIT**（匿名管道支持有限，故再叠加尺寸上限哨兵）
  if (fd_in >= 0 && ts_wr != nullptr) {
    DWORD mode = PIPE_NOWAIT;
    (void)SetNamedPipeHandleState(ts_wr, &mode, nullptr, nullptr);
  }
  if (fd < 0 || fd_in < 0) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    if (fd < 0) CloseHandle(rd); else ::_close(fd);
    if (fd_in < 0) CloseHandle(ts_wr); else ::_close(fd_in);
    return "";
  }
  const int ta_pid_val = static_cast<int>(pi.dwProcessId);
  const int ta_fd_val = fd;
  const int ta_stdin_val = fd_in;
  void* ta_handle_val = pi.hProcess;
#else
  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) {
    return "";
  }
  // v0.54.22: 子进程 stdin 管道 —— 此前子进程**继承服务端 stdin**（卫生问题），且 write_stdin
  // 写的是 pipe_fds[0]（stdout 读端）⇒ 该功能从未成功。改为显式管道：父持写端、子绑读端。
  int stdin_pipe[2];
  if (::pipe(stdin_pipe) != 0) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    return "";
  }

  pid_t pid = ::fork();
  if (pid == -1) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    ::close(stdin_pipe[0]);
    ::close(stdin_pipe[1]);
    return "";
  }

  if (pid == 0) {
    // 子进程：绑定 stdin + stdout/stderr 到 pipe
    ::close(pipe_fds[0]);
    ::close(stdin_pipe[1]);
    ::dup2(stdin_pipe[0], STDIN_FILENO);
    ::close(stdin_pipe[0]);
    ::dup2(pipe_fds[1], STDOUT_FILENO);
    ::dup2(pipe_fds[1], STDERR_FILENO);
    ::close(pipe_fds[1]);

    const char* shell = "/bin/sh";
    ::execlp(shell, shell, "-c", command.c_str(), nullptr);
    ::_exit(127);
  }

  // 父进程
  TA_CLOSE(pipe_fds[1]);
  TA_CLOSE(stdin_pipe[0]);
  // v0.54.27: 写端设**非阻塞** —— 隐患实锤：子进程不读 stdin 时，管道写满（64KB）后 write() 会**阻塞**，
  // 而 write_stdin 是持管理器互斥锁调的 ⇒ poll/wait/kill 全部卡死（后台进程整体冻结；实测 1MB 写 + `sleep`
  // 子进程 ⇒ 用例超时 EXIT=124）。非阻塞后：写不下就**立即返回失败**，绝不阻塞。
  {
    const int fl = ::fcntl(stdin_pipe[1], F_GETFL, 0);
    if (fl >= 0) (void)::fcntl(stdin_pipe[1], F_SETFL, fl | O_NONBLOCK);
  }

  const int ta_pid_val = static_cast<int>(pid);
  const int ta_fd_val = pipe_fds[0];
  const int ta_stdin_val = stdin_pipe[1];   // 父侧写端（子进程 stdin 的入口）
  void* ta_handle_val = nullptr;
#endif  // _WIN32

  auto proc = std::make_shared<Impl::Proc>();
  int seq = impl_->seq.fetch_add(1);
  std::ostringstream ss;
  ss << "proc_" << seq;
  proc->id = ss.str();
  proc->pid = ta_pid_val;
  proc->pipe_fd = ta_fd_val;
  proc->stdin_fd = ta_stdin_val;
#ifdef _WIN32
  proc->handle = ta_handle_val;
#endif
  proc->running.store(true);
  proc->started_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  proc->timeout_ms = timeout_ms;

  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->procs[proc->id] = proc;
  }

  return proc->id;
}

BgProcessState BackgroundProcessManager::poll(const std::string& session_id) {
  BgProcessState state;
  state.id = session_id;

  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->procs.find(session_id);
  if (it == impl_->procs.end()) {
    state.output = "process not found: " + session_id;
    return state;
  }

  auto& p = it->second;
  state.pid = p->pid;
  state.output = p->output;
  state.started_at_ms = p->started_at_ms;

  // 检查子进程是否已退出
  int code = -1;
  if (ta_try_reap(p->pid, TA_PROC_HANDLE(p), &code)) {
    p->running.store(false);
    if (code >= 0) p->exit_code.store(code);
    if (p->stdin_fd >= 0) { TA_CLOSE(p->stdin_fd); p->stdin_fd = -1; }   // v0.54.22
  }

  state.running = p->running.load();
  state.exit_code = p->exit_code.load();
  return state;
}

BgProcessState BackgroundProcessManager::wait(const std::string& session_id,
                                               int timeout_ms) {
  BgProcessState state;
  state.id = session_id;

  std::shared_ptr<Impl::Proc> proc;
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto it = impl_->procs.find(session_id);
    if (it == impl_->procs.end()) {
      state.output = "process not found: " + session_id;
      return state;
    }
    proc = it->second;
  }

  auto t0 = std::chrono::steady_clock::now();

  while (proc->running.load()) {
    int code = -1;
    if (ta_try_reap(proc->pid, TA_PROC_HANDLE(proc), &code)) {
      proc->running.store(false);
      if (code >= 0) proc->exit_code.store(code);
      if (proc->stdin_fd >= 0) { TA_CLOSE(proc->stdin_fd); proc->stdin_fd = -1; }   // v0.54.22
      break;
    }

    if (timeout_ms > 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0).count();
      if (elapsed >= timeout_ms) break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // v0.54.8 (R93): **子进程退出 ≠ 输出读完** —— monitor 线程异步 drain（select 周期 500ms），
  // 必须等它把管道读到 EOF（pipe_fd=-1）再取 output；否则快命令会报"exit_code=0 + 空输出"。
  // 有界 1000ms 兜底：即使 drain 卡住也不让 wait 变成无界阻塞（超时则返回当前已读部分）。
  {
    auto td = std::chrono::steady_clock::now();
    for (;;) {
      bool drained;
      {
        std::lock_guard<std::mutex> lk(impl_->mu);
        drained = (proc->pipe_fd < 0);
      }
      if (drained) break;
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - td).count() >= 1000) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // 最后读一次输出
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    state.output = proc->output;
    state.pid = proc->pid;
    state.running = proc->running.load();
    state.exit_code = proc->exit_code.load();
    state.started_at_ms = proc->started_at_ms;
  }

  return state;
}

BgProcessState BackgroundProcessManager::kill(const std::string& session_id) {
  BgProcessState state;
  state.id = session_id;

  std::shared_ptr<Impl::Proc> proc;
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto it = impl_->procs.find(session_id);
    if (it == impl_->procs.end()) {
      state.output = "process not found: " + session_id;
      return state;
    }
    proc = it->second;
  }

  if (proc->running.load()) {
    // POSIX: SIGTERM → 等 2s → SIGKILL；Windows: 无信号语义 ⇒ 直接 TerminateProcess（见 ta_kill_proc）
    ta_kill_proc(proc->pid, TA_PROC_HANDLE(proc), false);
    for (int i = 0; i < 20 && proc->running.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      int code = -1;
      if (ta_try_reap(proc->pid, TA_PROC_HANDLE(proc), &code)) {
        proc->running.store(false);
        if (code >= 0) proc->exit_code.store(code);
        if (proc->stdin_fd >= 0) { TA_CLOSE(proc->stdin_fd); proc->stdin_fd = -1; }   // v0.54.22
        break;
      }
    }
#ifndef _WIN32
    if (proc->running.load()) ta_kill_proc(proc->pid, nullptr, true);
#endif
  }

  // v0.54.8 (R93): 同 wait()——等 monitor 把管道 drain 到 EOF 再取 output（有界 1000ms），
  // 否则刚被 kill 的进程同样会报"exit_code=… + 空输出"。
  {
    auto td = std::chrono::steady_clock::now();
    for (;;) {
      bool drained;
      {
        std::lock_guard<std::mutex> lk(impl_->mu);
        drained = (proc->pipe_fd < 0);
      }
      if (drained) break;
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - td).count() >= 1000) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // 最后读一次输出
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    state.output = proc->output;
    state.pid = proc->pid;
    state.running = proc->running.load();
    state.exit_code = proc->exit_code.load();
    state.started_at_ms = proc->started_at_ms;
  }

  return state;
}

#ifndef _WIN32
// v0.54.27/28: 带 **SIGPIPE 防护**的管道写。子进程已退出（读端关闭）时对管道 write 会触发 SIGPIPE，
// 默认处置是**终止进程**（实测测试进程被杀，EXIT=141）；在服务里 = 写一次 stdin 打死服务。
// POSIX 惯用法：写前屏蔽 SIGPIPE、写后用 sigpending/sigtimedwait **消费**掉挂起的那个，再恢复掩码。
static ssize_t safe_pipe_write(int fd, const char* buf, size_t len) {
  sigset_t block_set, old_set;
  sigemptyset(&block_set);
  sigaddset(&block_set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &block_set, &old_set);
  const ssize_t n = ::write(fd, buf, len);
  sigset_t pend_set;
  if (sigpending(&pend_set) == 0 && sigismember(&pend_set, SIGPIPE) == 1) {
    struct timespec zero_timeout;
    zero_timeout.tv_sec = 0;
    zero_timeout.tv_nsec = 0;
    (void)sigtimedwait(&block_set, nullptr, &zero_timeout);
  }
  pthread_sigmask(SIG_SETMASK, &old_set, nullptr);
  return n;
}
#endif

bool BackgroundProcessManager::write_stdin(const std::string& session_id,
                                            const std::string& data) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto it = impl_->procs.find(session_id);
  if (it == impl_->procs.end()) return false;

  auto& p = it->second;
  // v0.54.22: 写 **stdin_fd**（此前错写 stdout 的读端 ⇒ 恒 EBADF，且失败被吞、上层照报成功）。
  // 现在**如实回报**：不可写（未启动/已退出/已关闭）或写入不完整都返回 false。
  if (p->stdin_fd < 0) return false;
  // v0.54.27/28: **绝不阻塞**（写端已设非阻塞；POSIX 走有界排空、Windows 走尺寸上限 + 单次写）。
  // 尺寸上限的判定在平台分支内（POSIX 无上限，改为有界排空支持大载荷）。
#ifdef _WIN32
  // Windows：PIPE_NOWAIT 对匿名管道支持有限 ⇒ 保留尺寸上限 + 单次写；写不下即如实 false（不阻塞）。
  // 残余风险（无 wine 无法验证，已记 BACKLOG）：若 PIPE_NOWAIT 不生效且子进程持续不读，本次写仍可能阻塞。
  if (data.size() > kMaxStdinWriteBytes) return false;
  const int n = static_cast<int>(TA_WRITE(p->stdin_fd, data.data(), data.size()));
  return n == static_cast<int>(data.size());
#else
  // v0.54.28: **有界排空**（取代 v0.54.27 的 60KB 硬拒）：子进程正常读时一遍写完；写不下则等写端可写，
  // 到总期限仍写不完 ⇒ 如实返回 false。既不再让大载荷无谓失败，也绝不无限阻塞
  //（旧版持锁阻塞 = 管理器冻结：poll/wait/kill 全卡死）。
  size_t sent = 0;
  bool failed = false;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kStdinWriteDeadlineMs);
  while (sent < data.size()) {
    const ssize_t n = safe_pipe_write(p->stdin_fd, data.data() + sent, data.size() - sent);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (std::chrono::steady_clock::now() >= deadline) { failed = true; break; }
      fd_set wfds;
      FD_ZERO(&wfds);
      FD_SET(p->stdin_fd, &wfds);
      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 20 * 1000;   // 20ms 片轮询：等对端排空，非人为延迟
      const int rv = ::select(p->stdin_fd + 1, nullptr, &wfds, nullptr, &tv);
      if (rv < 0 && errno != EINTR) { failed = true; break; }
      continue;
    }
    failed = true;   // EPIPE / EBADF / 其它 ⇒ 如实失败，不重试
    break;
  }
  return !failed && sent == data.size();
#endif
}

}  // namespace thin_agent
