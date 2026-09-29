/// v0.52.29: HookSystem 实现 —— 注册表+分发（回调同步/shell 子进程限时）。

#include "thin_agent/core/HookSystem.h"

// v0.54.17: shell 钩子的执行原语两平台差异极大（POSIX fork/pipe/select/waitpid vs Win32
// CreateProcess/管道/WaitForSingleObject）⇒ 按本仓约定**文件内隔离**（不跨文件封装）。
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <signal.h>
#include <cstdio>
#include <string>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

namespace thin_agent {

struct HookSystem::Impl {
  std::mutex mu;
  std::vector<HookRegistration> hooks;
  std::atomic<uint64_t> next_id{1};
};

HookSystem& HookSystem::instance() {
  static HookSystem inst;
  return inst;
}

std::string HookSystem::register_hook(HookRegistration reg) {
  if (!impl_) impl_.reset(new Impl());
  std::string id = "hook_" + std::to_string(
      impl_->next_id.fetch_add(1));
  reg.id = id;
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->hooks.push_back(std::move(reg));
  return id;
}

bool HookSystem::unregister_hook(const std::string& id) {
  if (!impl_) return false;
  std::lock_guard<std::mutex> lk(impl_->mu);
  for (auto it = impl_->hooks.begin(); it != impl_->hooks.end(); ++it) {
    if (it->id == id) {
      impl_->hooks.erase(it);
      return true;
    }
  }
  return false;
}

void HookSystem::clear() {
  if (!impl_) return;
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->hooks.clear();
}

size_t HookSystem::size() const {
  if (!impl_) return 0;
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->hooks.size();
}

std::vector<nlohmann::json> HookSystem::enumerate() const {
  std::vector<nlohmann::json> out;
  if (!impl_) return out;
  std::lock_guard<std::mutex> lk(impl_->mu);
  for (const auto& h : impl_->hooks) {
    nlohmann::json j;
    j["id"] = h.id;
    j["event"] = hook_event_name(h.event);
    if (!h.tool_filter.empty()) j["tool_filter"] = h.tool_filter;
    if (!h.shell_cmd.empty()) j["shell_cmd"] = h.shell_cmd;
    if (h.callback) j["callback"] = "cpp";
    j["timeout_ms"] = h.shell_timeout_ms;
    out.push_back(std::move(j));
  }
  return out;
}

/// shell 钩子：payload 走 stdin，限时等待，exit_code 判定。
static int run_shell_hook(const std::string& cmd, const std::string& stdin_json,
                          int timeout_ms, std::string* stderr_out) {
#ifdef _WIN32
  // v0.54.17: Windows 分支。语义对齐 POSIX 版：payload 走 **stdin**、捕获 **stderr**、stdout 丢弃、
  // 限时（10ms 步进）等待，超时杀子进程并返回 -2、否则返回子进程退出码。
  // 实现差异（如实标注）：POSIX 用 `pipe+fork+exec /bin/sh -c`，这里用
  // `CreatePipe`（stderr）+ **临时文件**（stdin：钩子 payload 很小，且避免与 stderr 抢读）
  // + `CreateProcessA("cmd.exe /c ...")` + `PeekNamedPipe` 非阻塞读 + `WaitForSingleObject` 限时
  // + `TerminateProcess` 超时收割。
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE err_rd = nullptr, err_wr = nullptr;
  if (!CreatePipe(&err_rd, &err_wr, &sa, 0)) return -1;
  SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);  // 读端留给父进程，不继承

  char tmp_dir[MAX_PATH] = {0};
  char in_path[MAX_PATH] = {0};
  if (GetTempPathA(MAX_PATH, tmp_dir) == 0 ||
      GetTempFileNameA(tmp_dir, "hkh", 0, in_path) == 0) {
    CloseHandle(err_rd);
    CloseHandle(err_wr);
    return -1;
  }
  HANDLE h_in = CreateFileA(in_path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_TEMPORARY, nullptr);
  HANDLE h_null = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h_in == INVALID_HANDLE_VALUE || h_null == INVALID_HANDLE_VALUE) {
    if (h_in != INVALID_HANDLE_VALUE) CloseHandle(h_in);
    if (h_null != INVALID_HANDLE_VALUE) CloseHandle(h_null);
    CloseHandle(err_rd);
    CloseHandle(err_wr);
    DeleteFileA(in_path);
    return -1;
  }
  if (!stdin_json.empty()) {
    DWORD written = 0;
    WriteFile(h_in, stdin_json.data(), static_cast<DWORD>(stdin_json.size()), &written, nullptr);
  }
  SetFilePointer(h_in, 0, nullptr, FILE_BEGIN);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = h_in;
  si.hStdOutput = h_null;
  si.hStdError = err_wr;
  PROCESS_INFORMATION pi{};
  std::string cmdline = "cmd.exe /c " + cmd;
  BOOL started = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(err_wr);
  CloseHandle(h_null);
  CloseHandle(h_in);
  if (!started) {
    CloseHandle(err_rd);
    DeleteFileA(in_path);
    return -1;
  }
  CloseHandle(pi.hThread);

  std::string err;
  char buf[1024];
  int waited = 0;
  bool exited = false;
  DWORD code = 0;
  while (waited <= timeout_ms) {
    DWORD avail = 0;
    while (PeekNamedPipe(err_rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      DWORD got = 0;
      if (!ReadFile(err_rd, buf, sizeof(buf), &got, nullptr) || got == 0) break;
      if (err.size() < 4096) err.append(buf, static_cast<size_t>(got));
    }
    if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) {
      exited = true;
      break;
    }
    waited += 10;
  }
  // 收尾：把管道里残留的 stderr 读干净（子进程已退出 ⇒ 管道写端已关，读完即 EOF）
  DWORD final_avail = 0;
  while (PeekNamedPipe(err_rd, nullptr, 0, nullptr, &final_avail, nullptr) && final_avail > 0) {
    DWORD got = 0;
    if (!ReadFile(err_rd, buf, sizeof(buf), &got, nullptr) || got == 0) break;
    if (err.size() < 4096) err.append(buf, static_cast<size_t>(got));
  }
  CloseHandle(err_rd);
  DeleteFileA(in_path);
  if (!exited) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    if (stderr_out) *stderr_out = "hook timeout";
    return -2;  // 超时码（与 POSIX 分支一致）
  }
  if (!GetExitCodeProcess(pi.hProcess, &code)) code = 1;
  CloseHandle(pi.hProcess);
  if (stderr_out) *stderr_out = err;
  return static_cast<int>(code);
#else
  // 简单双向管道：写 stdin 后读等待。用 fork+exec 手工管道（popen 单向不够）。
  int in_pipe[2], err_pipe[2];
  if (pipe(in_pipe) != 0) return -1;
  if (pipe(err_pipe) != 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

  pid_t pid = fork();
  if (pid < 0) {
    close(in_pipe[0]); close(in_pipe[1]);
    close(err_pipe[0]); close(err_pipe[1]);
    return -1;
  }
  if (pid == 0) {
    // 子进程：stdin←in_pipe 读端，stderr→err_pipe 写端，stdout 丢弃
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(in_pipe[1]); close(err_pipe[0]);
    close(err_pipe[1]);
    execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
    _exit(127);
  }
  // 父进程
  close(in_pipe[0]);
  close(err_pipe[1]);
  if (!stdin_json.empty()) {
    ssize_t n = write(in_pipe[1], stdin_json.data(), stdin_json.size());
    (void)n;
  }
  close(in_pipe[1]);  // EOF

  std::string err;
  char buf[1024];
  // 带超时轮询（10ms 步进）——观察者不得拖垮执行链。
  // EOF（read=0）≠ 失败：stderr 写端全关即 EOF，此时子进程通常已/将退出，
  // 继续走 waitpid 探测而非误判超时。
  const int deadline = timeout_ms;
  int waited = 0;
  bool exited = false;
  bool pipe_eof = false;
  int status = -1;
  while (waited <= deadline) {
    if (!pipe_eof) {
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(err_pipe[0], &rfds);
      struct timeval tv {0, 10000};  // 10ms
      if (select(err_pipe[0] + 1, &rfds, nullptr, nullptr, &tv) > 0) {
        ssize_t k = read(err_pipe[0], buf, sizeof(buf));
        if (k <= 0) pipe_eof = true;          // EOF：停读但继续等退出
        else if (err.size() < 4096) err.append(buf, static_cast<size_t>(k));
      }
    } else {
      usleep(10000);  // EOF 后纯等待，不再 select
    }
    // 探测子进程退出
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) { exited = true; break; }
    if (w < 0) break;  // 子进程已收割（不应发生，防御）
    waited += 10;
  }
  close(err_pipe[0]);
  if (!exited) {
    // 超时：杀掉（只杀直接子进程；钩子脚本应自行设计短平快）
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    if (stderr_out) *stderr_out = "hook timeout";
    return -2;  // 超时码
  }
  if (stderr_out) *stderr_out = err;
  return WEXITSTATUS(status);
#endif  // _WIN32
}

HookVerdict HookSystem::dispatch(HookEvent event, const nlohmann::json& payload) {
  HookVerdict verdict;
  if (!impl_) return verdict;  // 零钩子=零开销直通

  // 快照匹配集（锁内取，锁外执行——回调不得持锁）
  std::vector<HookRegistration> matched;
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    const std::string tool = payload.value("tool", "");
    for (const auto& h : impl_->hooks) {
      if (h.event != event) continue;
      if (!h.tool_filter.empty() &&
          std::find(h.tool_filter.begin(), h.tool_filter.end(), tool)
              == h.tool_filter.end())
        continue;
      matched.push_back(h);
    }
  }
  if (matched.empty()) return verdict;

  const std::string ev_name = hook_event_name(event);
  for (const auto& h : matched) {
    if (h.callback) {
      // C++ 回调：同步执行；tool_pre 的 deny 合并
      auto v = h.callback(payload);
      if (event == HookEvent::ToolPre && v.deny) {
        verdict.deny = true;
        if (!v.reason.empty())
          verdict.reason = verdict.reason.empty() ? v.reason
              : verdict.reason + "; " + v.reason;
      }
    } else if (!h.shell_cmd.empty()) {
      nlohmann::json p = payload;
      p["hook_event"] = ev_name;
      std::string err;
      int rc = run_shell_hook(h.shell_cmd, p.dump(), h.shell_timeout_ms, &err);
      // shell 钩子异常（超时/启动失败）只记 stderr，不影响裁决
      // ——观察者失效不应拖垮主链。tool_pre 的 exit!=0 → deny。
      if (event == HookEvent::ToolPre && rc > 0) {
        verdict.deny = true;
        std::string reason = err.substr(0, 200);
        verdict.reason = verdict.reason.empty() ? reason
            : verdict.reason + "; " + reason;
      }
    }
  }
  return verdict;
}

}  // namespace thin_agent
