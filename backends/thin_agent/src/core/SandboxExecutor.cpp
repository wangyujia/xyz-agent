/// SandboxExecutor — 跨平台进程隔离沙箱实现
///
/// 平台策略（编译期决定）：
///   __linux__   → clone() + namespaces + cgroups v2
///   __APPLE__   → sandbox-exec + posix_spawn + setrlimit
///   _WIN32      → CreateJobObject + pipe
///
/// 所有平台共用 fallback 兜底（setsid + setrlimit）。

#include "thin_agent/core/SandboxExecutor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

// ── POSIX 公共头 ──
#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// ── Linux 特有 ──
#ifdef __linux__
#include <sched.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>  // v0.45.6: makedev() 用于 mknod /dev/null
#endif

// ── macOS 特有 ──
#ifdef __APPLE__
#include <spawn.h>
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#endif

// ── Windows 特有 ──
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// v0.54.16: `jobapi2.h` **只有部分 Windows SDK 提供**（MinGW-w64 无此头 ⇒ 交叉/真机编译
// 直接 fatal error: jobapi2.h: No such file or directory）。本文件用到的 Job Object API/结构体
// （CreateJobObjectW / AssignProcessToJobObject / SetInformationJobObject /
// JOBOBJECT_EXTENDED_LIMIT_INFORMATION）都在 `winnt.h`（windows.h 已含）里声明，
// 故改为"有则包含"——MSVC 行为不变，MinGW/精简 SDK 也能编。
#if __has_include(<jobapi2.h>)
#include <jobapi2.h>
#endif
#endif

namespace thin_agent {

namespace {

// ═══════════════════════════════════════════════════
// 工具函数 (anonymous namespace helpers)
// ═══════════════════════════════════════════════════

// 将毫秒时间戳转换为秒（用于 setrlimit/watchdog）
static int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 安全读取管道所有数据（带超时和截断）
// v0.54.16: **POSIX-only 助手**——用 fcntl/select/FD_SET/read，MSVC 与 MinGW 都没有这套。
// 三个调用点都在 `#if !defined(_WIN32)` 的 POSIX 实现块内（Windows 实现走
// WaitForSingleObject + PeekNamedPipe + ReadFile），故整体包进守卫；不这么做时交叉编译报
// error: 'F_GETFL' was not declared in this scope。
#ifndef _WIN32
static std::string drain_fd(int fd, int max_bytes, int timeout_ms) {
  std::string out;
  out.reserve(max_bytes + 128);
  char buf[4096];
  int64_t start = now_ms();

  // 设置非阻塞
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  while (static_cast<int>(out.size()) < max_bytes) {
    int remaining = timeout_ms - static_cast<int>(now_ms() - start);
    if (remaining <= 0) break;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = {remaining / 1000, (remaining % 1000) * 1000};

    int sel = select(fd + 1, &fds, nullptr, nullptr, &tv);
    if (sel < 0) break;
    if (sel == 0) break;  // timeout

    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) break;
    buf[n] = '\0';
    out.append(buf, n);
  }

  if (static_cast<int>(out.size()) >= max_bytes) {
    out = out.substr(0, max_bytes);
    out += "\n...[truncated]";
  }

  return out;
}
#endif  // !_WIN32 —— drain_fd（POSIX-only）

#ifndef _WIN32
// POSIX: 发送信号终止进程组
static void kill_process_group(pid_t pid) {
  kill(-pid, SIGKILL);  // 杀整个进程组
}
#endif

}

// ═══════════════════════════════════════════════════
// Fallback（所有平台兜底）
// ═══════════════════════════════════════════════════

#if !defined(_WIN32)
SandboxExecutor::Result SandboxExecutor::execute_fallback(const std::string& command,
                                          const SandboxExecutor::Config& config) {
  SandboxExecutor::Result result;
  int pipefd[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pipefd) < 0) {
    result.error = "socketpair_failed: " + std::string(strerror(errno));
    return result;
  }

  int64_t start = now_ms();
  pid_t pid = fork();

  if (pid < 0) {
    result.error = "fork_failed: " + std::string(strerror(errno));
    close(pipefd[0]); close(pipefd[1]);
    return result;
  }

  if (pid == 0) {
    // ── 子进程 ──
    close(pipefd[0]);

    // 1. 独立进程组
    setsid();

    // 2. 资源限制
    if (config.max_memory_mb > 0) {
      struct rlimit rl;
      rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(config.max_memory_mb) * 1024 * 1024;
      setrlimit(RLIMIT_AS, &rl);
    }
    {
      struct rlimit rl;
      rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(config.timeout_ms / 1000 + 2);
      setrlimit(RLIMIT_CPU, &rl);
    }

    // 3. 重定向 stdout/stderr 到管道
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    // 4. 设置基础 PATH
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);

    // 5. 执行
    execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
    _exit(127);
  }

  // ── 父进程 ──
  close(pipefd[1]);

  // 读取输出
  result.output = drain_fd(pipefd[0], config.max_output_bytes, config.timeout_ms);
  close(pipefd[0]);

  // 等待子进程（带超时）
  int64_t waited = now_ms() - start;
  int remaining_ms = config.timeout_ms - static_cast<int>(waited);
  if (remaining_ms < 0) remaining_ms = 0;

  int status = 0;
  if (remaining_ms > 0) {
    // 超时轮询 waitpid
    int poll_interval_ms = 100;
    while (remaining_ms > 0) {
      pid_t w = waitpid(pid, &status, WNOHANG);
      if (w == pid) break;
      if (w < 0 && errno != EINTR) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
      remaining_ms -= poll_interval_ms;
    }
  }

  if (remaining_ms <= 0 && waitpid(pid, &status, WNOHANG) == 0) {
    // 超时 → 强制终止
    result.timed_out = true;
    kill_process_group(pid);
    // v0.53.64: SIGKILL 后有限收割——无限期 waitpid(0) 在子进程
    /// D 状态(不可中断 IO)时永挂父线程;2s 重试窗后放弃(僵尸由
    /// init 收,WNOHANG 非阻塞不留悬挂)
    for (int i = 0; i < 20; ++i) {
      if (waitpid(pid, &status, WNOHANG) == pid) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    result.output += "\n...[timeout]";
  }

  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  result.elapsed_ms = static_cast<int>(now_ms() - start);
  if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL && !result.timed_out) {
    result.killed_by_oom = true;
  }

  return result;
}
#endif

// ═══════════════════════════════════════════════════
// Linux 原生沙箱 (clone + namespaces + cgroups v2)
// ═══════════════════════════════════════════════════

#ifdef __linux__

// Linux 子进程入口（在 clone 后的新 namespace 中运行）
static int linux_child_entry(void* arg_ptr) {
  auto* args = static_cast<std::pair<std::string, int>*>(arg_ptr);
  const std::string& command = args->first;
  int pipe_fd = args->second;

  // v0.45.8: 成为新进程组组长 — 否则超时 kill(-pid) 杀不了整个进程组，
  // waitpid 永久阻塞 → worker 卡死（实测：模型 shell 里 bash monitor.sh
  // 死循环，30s 超时后服务 chat 通道彻底无响应）。
  (void)setpgid(0, 0);

  // 1. 私有化挂载命名空间
  mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr);

  // 2. v0.45.6: 先把宿主 /tmp bind 到临时锚点（此时 /tmp 还是宿主），
  // 稍后替换 /tmp 为 tmpfs 后，再把锚点 bind 进沙箱 /host_tmp。
  // 背景：write_file（宿主文件系统）与 shell_exec（tmpfs 沙箱）文件
  // 系统隔离，编程"写代码→运行"链路断裂（实测：模型被迫 find / 找
  // 文件→卡死）。bind 宿主 /tmp 为沙箱可写区：模型把代码写到
  // /tmp/...（write_file 持久），shell 里用 /host_tmp/... 访问同一文件。
  const char* kTmpAnchor = "/tmp_anchor_host";
  mkdir(kTmpAnchor, 0777);
  mount("/tmp", kTmpAnchor, nullptr, MS_BIND | MS_REC, nullptr);

  // 3. tmpfs 作为沙箱根（pivot_root 要求 new_root 与当前根同文件系统，
  // tmpfs 挂 /tmp 满足）
  mount("tmpfs", "/tmp", "tmpfs", 0, "size=64M");

  // 4. 创建最小文件系统
  mkdir("/tmp/bin", 0755);
  mkdir("/tmp/lib", 0755);
  mkdir("/tmp/lib64", 0755);  // v0.45.6: 动态链接器所在目录，缺则 execl ENOENT
  mkdir("/tmp/usr", 0755);
  mkdir("/tmp/etc", 0755);

  // 5. bind-mount 系统目录（只读）
  mount("/bin", "/tmp/bin", nullptr, MS_BIND | MS_RDONLY, nullptr);
  mount("/lib", "/tmp/lib", nullptr, MS_BIND | MS_RDONLY, nullptr);
  mount("/lib64", "/tmp/lib64", nullptr, MS_BIND | MS_RDONLY, nullptr);
  mount("/usr", "/tmp/usr", nullptr, MS_BIND | MS_RDONLY, nullptr);
  mount("/etc", "/tmp/etc", nullptr, MS_BIND | MS_RDONLY, nullptr);

  // 6. 宿主 /tmp 工作区 bind 进沙箱 /host_tmp（可写，跨调用持久）
  mkdir("/tmp/host_tmp", 0777);
  mount(kTmpAnchor, "/tmp/host_tmp", nullptr, MS_BIND | MS_REC, nullptr);

  // 7. pivot_root 到临时根
  mkdir("/tmp/.oldroot", 0755);
  if (syscall(SYS_pivot_root, "/tmp", "/tmp/.oldroot") != 0) {
    if (::chroot("/tmp") != 0) {}  // v0.53.16: 消费返回值（失败=降级非致命）
  }
  if (::chdir("/") != 0) {}  // v0.53.16: 同上

  // 8. 卸载旧根（含锚点）
  umount2("/.oldroot", MNT_DETACH);

  // 9. 挂载 proc / dev
  mkdir("/proc", 0755);
  mkdir("/dev", 0755);
  mount("proc", "/proc", "proc", 0, nullptr);
  if (mount("devtmpfs", "/dev", "devtmpfs", 0, nullptr) != 0) {
    // v0.45.6: devtmpfs 挂载失败（部分 WSL2 内核）→ 手动创建基本设备
    // 否则 /dev/null 缺失导致大量命令（重定向）失败
    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/zero", S_IFCHR | 0666, makedev(1, 5));
    mknod("/dev/random", S_IFCHR | 0666, makedev(1, 8));
    mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9));
    mknod("/dev/tty", S_IFCHR | 0666, makedev(5, 0));
  }

  // 10. stdout/stderr 重定向
  dup2(pipe_fd, STDOUT_FILENO);
  dup2(pipe_fd, STDERR_FILENO);
  close(pipe_fd);

  // 11. 执行
  execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
  _exit(127);
  return 127;
}

SandboxExecutor::Result SandboxExecutor::execute_linux(const std::string& command,
                                       const SandboxExecutor::Config& config) {
  std::string cgroup_path;  // v0.53.67: cgroup 清收用
  bool used_cgroup = false;  // v0.53.67
  SandboxExecutor::Result result;
  int pipefd[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pipefd) < 0) {
    return execute_fallback(command, config);
  }

  // 256KB 栈（clone 需要独立栈）
  static constexpr size_t kStackSize = 256 * 1024;
  void* stack = mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  if (stack == MAP_FAILED) {
    close(pipefd[0]); close(pipefd[1]);
    return execute_fallback(command, config);
  }

  int clone_flags = SIGCHLD | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWIPC;
  if (!config.allow_network) {
    clone_flags |= CLONE_NEWNET;
  }

  auto cmd_str = command;  // 可修改副本
  std::pair<std::string, int> clone_arg = {cmd_str, pipefd[1]};

  int64_t start = now_ms();
  pid_t pid = clone(linux_child_entry, (char*)stack + kStackSize,
                    clone_flags, &clone_arg);

  if (pid < 0) {
    munmap(stack, kStackSize);
    close(pipefd[0]); close(pipefd[1]);
    return execute_fallback(command, config);
  }

  close(pipefd[1]);

  // ── 尝试 cgroups v2 内存限制 ──
  if (config.max_memory_mb > 0) {
    cgroup_path = "/sys/fs/cgroup/thin_agent_sandbox_" + std::to_string(pid);
    used_cgroup = (mkdir(cgroup_path.c_str(), 0755) == 0);  // v0.53.67
    std::ofstream(cgroup_path + "/cgroup.procs") << pid;
    std::ofstream(cgroup_path + "/memory.max")
        << (static_cast<int64_t>(config.max_memory_mb) * 1024 * 1024);
    std::ofstream(cgroup_path + "/memory.high")
        << (static_cast<int64_t>(config.max_memory_mb) * 1024 * 1024 * 80 / 100);
    // 注册清理
    // TODO: 子进程退出后 rmdir cgroup_path
  }

  // 读取输出
  result.output = drain_fd(pipefd[0], config.max_output_bytes, config.timeout_ms);
  close(pipefd[0]);

  // 等待子进程
  int64_t waited = now_ms() - start;
  int remaining_ms = config.timeout_ms - static_cast<int>(waited);
  if (remaining_ms < 0) remaining_ms = 0;

  int status = 0;
  bool exited = false;
  while (remaining_ms > 0) {
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) { exited = true; break; }
    if (w < 0 && errno != EINTR) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    remaining_ms -= 100;
  }

  if (!exited) {
    result.timed_out = true;
    kill_process_group(pid);
    // v0.53.64: SIGKILL 后有限收割——无限期 waitpid(0) 在子进程
    /// D 状态(不可中断 IO)时永挂父线程;2s 重试窗后放弃(僵尸由
    /// init 收,WNOHANG 非阻塞不留悬挂)
    for (int i = 0; i < 20; ++i) {
      if (waitpid(pid, &status, WNOHANG) == pid) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    result.output += "\n...[timeout]";
  }

  // v0.53.67: cgroup 清收——进程已收割(reap),先移出再删目录;
  /// 此前每次执行泄漏一个 thin_agent_sandbox_* 目录(长跑堆积)
  if (used_cgroup) {
    std::ofstream(cgroup_path + "/cgroup.procs") << 0;  // 移回 root cgroup
    ::rmdir(cgroup_path.c_str());  // 空(无进程)时成功;残留=下次同名复用
  }

  munmap(stack, kStackSize);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  result.elapsed_ms = static_cast<int>(now_ms() - start);
  if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL && !result.timed_out) {
    result.killed_by_oom = true;
  }

  return result;
}
#endif  // __linux__

// ═══════════════════════════════════════════════════
// macOS 原生沙箱 (sandbox-exec)
// ═══════════════════════════════════════════════════

#ifdef __APPLE__
SandboxExecutor::Result SandboxExecutor::execute_macos(const std::string& command,
                                       const SandboxExecutor::Config& config) {
  SandboxExecutor::Result result;
  int pipefd[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pipefd) < 0) {
    return execute_fallback(command, config);
  }

  // 构建 sandbox-exec profile
  std::ostringstream profile;
  profile << "(version 1)\n";
  profile << "(allow default)\n";
  profile << "(deny file-write*)\n";
  profile << "(allow file-write* (subpath \"/tmp\"))\n";
  profile << "(allow file-write* (subpath \"/dev/null\"))\n";
  if (!config.allow_network) {
    profile << "(deny network*)\n";
  }
  if (!config.writable_paths.empty()) {
    for (const auto& p : config.writable_paths) {
      profile << "(allow file-write* (subpath \"" << p << "\"))\n";
    }
  }

  // 写入临时文件
  std::string profile_path = "/tmp/thin_agent_sandbox_" +
                             std::to_string(getpid()) + ".sb";
  {
    std::ofstream ofs(profile_path);
    ofs << profile.str();
  }

  int64_t start = now_ms();
  pid_t pid = fork();

  if (pid < 0) {
    result.error = "fork_failed";
    close(pipefd[0]); close(pipefd[1]);
    unlink(profile_path.c_str());
    return result;
  }

  if (pid == 0) {
    // ── 子进程 ──
    close(pipefd[0]);
    setsid();

    // 资源限制
    if (config.max_memory_mb > 0) {
      struct rlimit rl;
      rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(config.max_memory_mb) * 1024 * 1024;
      setrlimit(RLIMIT_AS, &rl);
      setrlimit(RLIMIT_DATA, &rl);
    }
    {
      struct rlimit rl;
      rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(config.timeout_ms / 1000 + 2);
      setrlimit(RLIMIT_CPU, &rl);
    }

    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    // sandbox-exec 执行
    execl("/usr/bin/sandbox-exec", "sandbox-exec",
          "-f", profile_path.c_str(),
          "sh", "-c", command.c_str(), nullptr);
    _exit(127);
  }

  // ── 父进程 ──
  close(pipefd[1]);
  result.output = drain_fd(pipefd[0], config.max_output_bytes, config.timeout_ms);
  close(pipefd[0]);

  int status = 0;
  int64_t waited = now_ms() - start;
  int remaining_ms = config.timeout_ms - static_cast<int>(waited);
  if (remaining_ms < 0) remaining_ms = 0;

  bool exited = false;
  while (remaining_ms > 0) {
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) { exited = true; break; }
    if (w < 0 && errno != EINTR) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    remaining_ms -= 100;
  }

  if (!exited) {
    result.timed_out = true;
    kill_process_group(pid);
    // v0.53.64: SIGKILL 后有限收割——无限期 waitpid(0) 在子进程
    /// D 状态(不可中断 IO)时永挂父线程;2s 重试窗后放弃(僵尸由
    /// init 收,WNOHANG 非阻塞不留悬挂)
    for (int i = 0; i < 20; ++i) {
      if (waitpid(pid, &status, WNOHANG) == pid) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    result.output += "\n...[timeout]";
  }

  unlink(profile_path.c_str());
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  result.elapsed_ms = static_cast<int>(now_ms() - start);
  return result;
}
#endif  // __APPLE__

// ═══════════════════════════════════════════════════
// Windows 原生沙箱 (Job Object)
// ═══════════════════════════════════════════════════

#ifdef _WIN32
SandboxExecutor::Result SandboxExecutor::execute_windows(const std::string& command,
                                         const SandboxExecutor::Config& config) {
  SandboxExecutor::Result result;

  // 1. 创建管道
  HANDLE hRead, hWrite;
  SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
  if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
    return execute_fallback(command, config);
  }
  SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

  // 2. 创建 Job Object
  HANDLE hJob = CreateJobObject(nullptr, nullptr);
  if (hJob) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
                                            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (config.max_memory_mb > 0) {
      jeli.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
      jeli.JobMemoryLimit = static_cast<SIZE_T>(config.max_memory_mb) * 1024 * 1024;
    }
    SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
                            &jeli, sizeof(jeli));
  }

  // 3. 启动进程
  PROCESS_INFORMATION pi = {};
  STARTUPINFO si = {sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = hWrite;
  si.hStdError = hWrite;

  std::string cmdline = "cmd.exe /c " + command;

  int64_t start = now_ms();
  BOOL ok = CreateProcess(
      nullptr, const_cast<char*>(cmdline.c_str()),
      nullptr, nullptr, TRUE,
      CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
      nullptr, nullptr, &si, &pi);

  CloseHandle(hWrite);

  if (!ok) {
    result.error = "CreateProcess_failed";
    CloseHandle(hRead);
    if (hJob) CloseHandle(hJob);
    return result;
  }

  // 4. 分配到 Job Object
  if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);

  // 5. 读取输出（带超时）
  DWORD timeout = config.timeout_ms > 0 ? config.timeout_ms : INFINITE;
  std::string output;
  char buf[4096];

  while (true) {
    DWORD avail = 0;
    if (!PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
      if (WaitForSingleObject(pi.hProcess, 100) != WAIT_TIMEOUT) break;
      continue;
    }
    DWORD nread = 0;
    if (ReadFile(hRead, buf, sizeof(buf) - 1, &nread, nullptr) && nread > 0) {
      buf[nread] = '\0';
      output += buf;
      if (static_cast<int>(output.size()) >= config.max_output_bytes) {
        output = output.substr(0, config.max_output_bytes) + "\n...[truncated]";
        break;
      }
    }
  }

  // 6. 等待进程退出或超时
  // v0.53.65: Job 生效接线——创建后必须 AssignProcessToJobObject,
  /// 否则内存限额/KILL_ON_JOB_CLOSE 全不生效=沙箱形同虚设
  if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);

  // v0.53.65: 管道读取(此前从未 ReadFile——output 恒空!)
  /// 轮询读+超时双条件,退出后再 drain 尾部
  std::string out_buf;
  {
    DWORD navail = 0;
    int64_t deadline = now_ms() + timeout;
    for (;;) {
      BOOL alive = WaitForSingleObject(pi.hProcess, 50) == WAIT_TIMEOUT;
      // 非阻塞探测量
      if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &navail, nullptr) && navail > 0) {
        std::array<char, 4096> pb{};
        DWORD nread = 0;
        if (ReadFile(hRead, pb.data(), static_cast<DWORD>(
                std::min<DWORD>(navail, static_cast<DWORD>(pb.size()))),
                &nread, nullptr) && nread > 0) {
          out_buf.append(pb.data(), nread);
          if (out_buf.size() >= static_cast<size_t>(config.max_output_bytes > 0
                  ? config.max_output_bytes : 65536)) break;
        }
      } else if (!alive) {
        // 进程已退+管道空=drain 完
        if (navail == 0) break;
      }
      if (!alive && navail == 0) break;
      if (now_ms() > deadline) {
        result.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
        out_buf += "\n...[timeout]";
        break;
      }
    }
    // 终局再探一次(竞态尾部)
    DWORD n2 = 0;
    while (PeekNamedPipe(hRead, nullptr, 0, nullptr, &navail, nullptr) && navail > 0) {
      std::array<char, 4096> pb{};
      if (!ReadFile(hRead, pb.data(),
                    static_cast<DWORD>(std::min<DWORD>(navail, 4096)), &n2, nullptr) || n2 == 0) break;
      out_buf.append(pb.data(), n2);
    }
  }

  if (result.timed_out) {
    // 已在循环内标记
  }
  DWORD wait_result = WaitForSingleObject(pi.hProcess, 5000);
  if (wait_result == WAIT_TIMEOUT) {
    result.timed_out = true;
    TerminateProcess(pi.hProcess, 1);
    out_buf += "\n...[hard_timeout]";
  }

  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  result.exit_code = static_cast<int>(exit_code);
  result.output = out_buf;
  result.elapsed_ms = static_cast<int>(now_ms() - start);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(hRead);
  if (hJob) CloseHandle(hJob);

  return result;
}
#endif  // _WIN32

// ═══════════════════════════════════════════════════
// 公共接口
// ═══════════════════════════════════════════════════

bool SandboxExecutor::native_sandbox_available() {
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
  return true;
#else
  return false;
#endif
}

SandboxExecutor::Result SandboxExecutor::execute(
    const std::string& command, const SandboxExecutor::Config& config) {
#if defined(__linux__)
  return execute_linux(command, config);
#elif defined(__APPLE__) && defined(__MACH__)
  return execute_macos(command, config);
#elif defined(_WIN32)
  return execute_windows(command, config);
#else
  return execute_fallback(command, config);
#endif
}

}  // namespace thin_agent
