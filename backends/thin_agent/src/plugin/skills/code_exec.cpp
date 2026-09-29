/// libskill_code_exec.so — 多语言代码执行插件（解释器类白名单）
///
/// 注册的 handler:
///   code_exec  — 执行代码片段（子进程，安全隔离；language 选语言）
///
/// 仅在 --dev 模式下加载。纯计算模式（A1）：
///   - 不提供工具回调（A2 待需求决定）
///   - 可调用语言标准库（Python: json/re/os/sys/math/csv/datetime/collections）
///   - 写临时文件 → 子进程执行解释器 → 捕获 stdout/stderr
///
/// v0.53.6 语言类白名单（设计原则：能力按"类"而非语言特例——
/// 新语言只改词表不动代码）：
///   - 词表来源：chat_policy.json 的 interpreters 段（init2 经 ctx 配置
///     通道注入；核心侧注入键 = "code_exec"）
///   - 每项：{bin, ext, args}——bin=解释器可执行名，ext=临时文件扩展名，
///     args=传给解释器的前置参数（如 python3 的 -u）
///   - 未注册语言 → unsupported_language 明确报错（拒绝任意解释器）
///   - 内置兜底词表：python / node（无配置时保底可用）

#include "thin_agent/plugin/PluginContext.h"
#include "thin_agent/plugin/PluginInterface.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <thread>

// v0.54.19: 代码执行的两平台差异集中在此（POSIX 双重 fork+进程组 vs Win32 CreateProcess+Job Object）。
// 按本仓约定**文件内隔离**（不跨文件封装）。
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef _WIN32
namespace {
/// argv → Windows 命令行（CreateProcess 解析规则：反斜杠仅在其后紧跟引号时加倍）
std::string code_exec_win_quote(const std::string& a) {
  if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
  std::string out = "\"";
  int bs = 0;
  for (char c : a) {
    if (c == '\\') { ++bs; continue; }
    if (c == '"') {
      out.append(static_cast<size_t>(bs * 2 + 1), '\\');
      out.push_back('"');
      bs = 0;
      continue;
    }
    out.append(static_cast<size_t>(bs), '\\');
    bs = 0;
    out.push_back(c);
  }
  out.append(static_cast<size_t>(bs * 2), '\\');
  out.push_back('"');
  return out;
}
}  // namespace
#endif

namespace thin_agent {
namespace code_exec {

// ── v0.53.6 语言类白名单 ──
struct InterpreterSpec {
  std::string bin;
  std::string ext;
  std::vector<std::string> args;
};

/// 词表（init2 时从配置注入；无配置时用内置兜底）
static std::mutex g_interp_mu;  // v0.53.71: 词表并发写防护(init2 热注入 vs 工具并发查)
static std::map<std::string, InterpreterSpec> g_interpreters = {
    {"python", {"python3", ".py", {"-u"}}},
    {"node", {"node", ".js", {}}},
};

static bool interp_known(const std::string& lang) {
  return g_interpreters.count(lang) > 0;
}

/// 生成唯一临时文件名
static std::string make_temp_path(const std::string& ext) {
#ifdef _WIN32
  // v0.54.19: Windows 无 `mkstemp()`、也无 `/tmp` ⇒ `GetTempPathA` + **CREATE_NEW 原子抢名**
  // （同一语义：名字被占则换名重试，绝不覆盖他人文件）。
  char tdir[MAX_PATH] = {0};
  if (GetTempPathA(MAX_PATH, tdir) == 0) return std::string();
  const std::string base = std::string(tdir) + "ta_exec_" +
                           std::to_string(GetCurrentProcessId()) + "_";
  for (int i = 0; i < 1000; ++i) {
    const std::string cand = base + std::to_string(i) + ext;
    HANDLE h = CreateFileA(cand.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      CloseHandle(h);
      return cand;
    }
    if (GetLastError() != ERROR_FILE_EXISTS) break;
  }
  return std::string();
#else
  char tmpl[] = "/tmp/ta_exec_XXXXXX";
  int fd = ::mkstemp(tmpl);
  if (fd >= 0) {
    ::close(fd);
    // v0.53.6: 扩展名按语言词表（.py/.js/...——解释器与工具按后缀识别）
    std::string path(tmpl);
    std::string full = path + ext;
    ::rename(path.c_str(), full.c_str());
    return full;
  }
  // 降级: 使用时间戳
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return "/tmp/ta_exec_" + std::to_string(now) + ext;
#endif
}

/// 读取文件全部内容
static std::string read_all(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) return "";
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/// 安全删除文件
static void safe_unlink(const std::string& path) {
  ::unlink(path.c_str());
}

// ── Handler: code_exec ──
// params: code (必需, Python 代码), timeout (可选, 默认 30s)
// 返回: { success, output (stdout + stderr), exit_code, elapsed_ms }

nlohmann::json handle_code_exec(const nlohmann::json& params) {
  std::string code = params.value("code", "");
  if (code.empty())
    return {{"success", false}, {"error", "code_required"}};

  // v0.53.6: 语言类白名单——未注册语言明确拒绝（可用列表一并返回）
  std::string lang = params.value("language", "python");
  // v0.54.3 (R90): **锁只护查表**。此前 `std::lock_guard lk(g_interp_mu);` 写在函数开头，
  // 注释却写"v0.53.71: 拷贝后放锁使用"——**lock_guard 没有 unlock()**，锁实际被持有到函数
  // 结束（本函数 200+ 行，含子进程执行与 `sleep_for(timeout_sec + 1)`，timeout 上限 120s）
  // ⇒ 一次 code_exec 独占互斥量两分钟，并发的 code_exec/词典访问全被阻塞
  // （与 R73 的 trigger_now 同族：锁跨重活；且属"注释与实现不符"）。
  InterpreterSpec spec;
  bool lang_known = false;
  {
    std::lock_guard<std::mutex> lk(g_interp_mu);
    auto it = g_interpreters.find(lang);
    if (it != g_interpreters.end()) {
      spec = it->second;
      lang_known = true;
    } else {
      std::string known;
      for (const auto& [name, _] : g_interpreters) {
        if (!known.empty()) known += ", ";
        known += name;
      }
      return {{"success", false},
              {"error", "unsupported_language"},
              {"language", lang},
              {"supported", known}};
    }
  }
  if (!lang_known) return {{"success", false}, {"error", "unsupported_language"}};
  if (spec.bin.empty())
    return {{"success", false},
            {"error", "interpreter_misconfigured"},
            {"language", lang}};

  int timeout_sec = params.value("timeout", 30);
  if (timeout_sec < 1) timeout_sec = 1;
  if (timeout_sec > 120) timeout_sec = 120;

  // 写临时文件
  std::string tmp_path = make_temp_path(spec.ext);
  {
    std::ofstream out(tmp_path);
    if (!out.is_open())
      return {{"success", false}, {"error", "cannot_create_temp_file"}};
    out << code;
    if (!code.empty() && code.back() != '\n')
      out << '\n';
    out.close();
  }

  // fork + exec python3
  auto start = std::chrono::steady_clock::now();

#ifdef _WIN32
  // v0.54.19: Windows 分支。POSIX 的双重 fork（中间进程当组长 + 孙进程执行 + `.result` 文件回传）
  // 在 Windows **不需要**：**Job Object 直接表达"整树收割"**（`TerminateJobObject` ≡ `kill(-pgid)`，
  // 且无需"中间进程当组长"的竞态技巧）；父进程自己用 `PeekNamedPipe` 排水，最后按**同一 `.result`
  // 格式**写回 ⇒ 下游解析/清理路径**零改动**（与 POSIX 分支行为对齐）。
  HANDLE job = CreateJobObjectA(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jli{};
    jli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jli, sizeof(jli));
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;
  HANDLE out_rd = nullptr, out_wr = nullptr, err_rd = nullptr, err_wr = nullptr;
  bool pipes_ok = CreatePipe(&out_rd, &out_wr, &sa, 0) && CreatePipe(&err_rd, &err_wr, &sa, 0);
  if (!pipes_ok) {
    if (out_rd) CloseHandle(out_rd);
    if (out_wr) CloseHandle(out_wr);
    if (err_rd) CloseHandle(err_rd);
    if (err_wr) CloseHandle(err_wr);
    if (job) CloseHandle(job);
    safe_unlink(tmp_path);
    return {{"success", false}, {"error", "pipe_failed"}};
  }
  SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul_h = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = (nul_h == INVALID_HANDLE_VALUE) ? nullptr : nul_h;
  si.hStdOutput = out_wr;
  si.hStdError = err_wr;

  // 命令行 = bin + args + 脚本路径（解释器来自词表，Windows 侧写 .exe/py 等；按引用规则转义）
  std::string cmdline = code_exec_win_quote(spec.bin);
  for (const auto& a : spec.args) cmdline += " " + code_exec_win_quote(a);
  cmdline += " " + code_exec_win_quote(tmp_path);

  PROCESS_INFORMATION pi{};
  BOOL started = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(out_wr);
  CloseHandle(err_wr);
  if (nul_h != INVALID_HANDLE_VALUE) CloseHandle(nul_h);
  if (!started) {
    CloseHandle(out_rd);
    CloseHandle(err_rd);
    if (job) CloseHandle(job);
    safe_unlink(tmp_path);
    return {{"success", false}, {"error", "spawn_failed"}};
  }
  CloseHandle(pi.hThread);
  if (job) AssignProcessToJobObject(job, pi.hProcess);   // ⇒ 后代全在 job 内，超时可整树收割

  std::string win_output;
  char win_buf[4096];
  bool out_open = true, err_open = true;
  bool exited = false;
  const int64_t deadline_ms = static_cast<int64_t>(timeout_sec) * 1000;
  int64_t waited_ms = 0;
  while (waited_ms <= deadline_ms) {
    if (out_open) {
      DWORD avail = 0;
      if (PeekNamedPipe(out_rd, nullptr, 0, nullptr, &avail, nullptr)) {
        if (avail > 0) {
          DWORD got = 0;
          if (ReadFile(out_rd, win_buf, sizeof(win_buf), &got, nullptr) && got > 0)
            win_output.append(win_buf, static_cast<size_t>(got));
        }
      } else {
        out_open = false;   // ERROR_BROKEN_PIPE = 写端已关（EOF）
      }
    }
    if (err_open) {
      DWORD avail = 0;
      if (PeekNamedPipe(err_rd, nullptr, 0, nullptr, &avail, nullptr)) {
        if (avail > 0) {
          DWORD got = 0;
          ReadFile(err_rd, win_buf, sizeof(win_buf), &got, nullptr);   // 排掉防阻塞；与原实现一致不入 output
        }
      } else {
        err_open = false;
      }
    }
    if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
      exited = true;
      break;
    }
    waited_ms += 50;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  auto end = std::chrono::steady_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

  if (!exited) {
    // 超时：整树收割（Job Object ⇒ 中间/孙/后代一并带走，等价 POSIX 的 kill(-pid)）
    if (job) {
      TerminateJobObject(job, 1);
    } else {
      TerminateProcess(pi.hProcess, 1);
    }
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(out_rd);
    CloseHandle(err_rd);
    if (job) CloseHandle(job);
    safe_unlink(tmp_path);
    safe_unlink(tmp_path + ".result");
    return {{"success", false}, {"error", "timeout"}, {"elapsed_ms", elapsed_ms}};
  }

  // 收尾把剩余输出读净（子进程已退出 ⇒ 管道写端已关，读完即 EOF）
  for (int pass = 0; pass < 2; ++pass) {
    if (!out_open) break;
    DWORD avail = 0;
    if (!PeekNamedPipe(out_rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
    DWORD got = 0;
    if (ReadFile(out_rd, win_buf, sizeof(win_buf), &got, nullptr) && got > 0)
      win_output.append(win_buf, static_cast<size_t>(got));
  }

  DWORD win_exit_code = 0;
  if (!GetExitCodeProcess(pi.hProcess, &win_exit_code)) win_exit_code = 1;
  CloseHandle(pi.hProcess);
  CloseHandle(out_rd);
  CloseHandle(err_rd);
  if (job) CloseHandle(job);   // KILL_ON_JOB_CLOSE：若仍有后代存活，此处一并清理

  // 按与 POSIX 子进程**完全相同的格式**写 `.result` ⇒ 下游解析零改动
  {
    std::string result_path = tmp_path + ".result";
    std::ofstream rf(result_path);
    rf << "EXIT:" << static_cast<int>(win_exit_code) << "\n";
    rf << "OUTPUT:" << win_output.size() << "\n" << win_output;
  }
#else

  pid_t pid = ::fork();
  if (pid < 0) {
    safe_unlink(tmp_path);
    return {{"success", false}, {"error", "fork_failed"}};
  }

  if (pid == 0) {
    // 子进程: 重定向 stdout/stderr → 管道，执行 Python
    // 使用 popen 替代方案：通过 dup2 重定向
    // v0.52.10: 自立进程组（父侧超时 kill(-pid) 的前提；与父侧
    // kill(pid) 兜底配合容忍 setpgid 竞态窗口）。
    ::setpgid(0, 0);
    int pipe_out[2], pipe_err[2];
    ::pipe(pipe_out);
    ::pipe(pipe_err);

    pid_t child = ::fork();
    if (child < 0) ::_exit(1);

    if (child == 0) {
      // 孙进程: 执行 python3
      // v0.52.10 修正：孙进程【不】setpgid——留在中间进程的组 M 内。
      // 父进程超时 kill(-pid) 一次收割整树（中间+孙+孙派生的后代）。
      // 若孙进程自立组，会逃出父亲的组射程；而中间进程体内的超时
      // 线程（timeout+1s 才醒）在父亲 timeout 杀中间时已陪葬，孙进程
      // 成为无主孤儿（真 e2e 实测 22 核打满的根因形态）。
      ::dup2(pipe_out[1], STDOUT_FILENO);
      ::dup2(pipe_err[1], STDERR_FILENO);
      ::close(pipe_out[0]); ::close(pipe_out[1]);
      ::close(pipe_err[0]); ::close(pipe_err[1]);

      // v0.53.6: 通用解释器执行——bin+args 来自词表（execvp 按 PATH 找）
      {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(spec.bin.c_str()));
        for (const auto& a : spec.args)
          argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(const_cast<char*>(tmp_path.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
      }
      ::_exit(127);  // execvp 失败
    }

    // 子进程: 读管道 + 等待孙进程
    ::close(pipe_out[1]);
    ::close(pipe_err[1]);

    // 超时监控线程（中间进程体内的兜底：父亲侧收割失败时补刀）
    // v0.52.10: kill(-child)——孙进程留在本组（未 setpgid），整组收割。
    std::thread timeout_thread([child, timeout_sec]() {
      std::this_thread::sleep_for(std::chrono::seconds(timeout_sec + 1));
      ::kill(-child, SIGKILL);
      ::kill(child, SIGKILL);
    });
    timeout_thread.detach();

    // v0.52.7: 先排干管道再 waitpid——原次序在输出 > 管道容量(64KB)
    // 时孙进程阻塞在 write、父端阻塞在 waitpid 形成死锁，被超时线程
    // 误杀（chatty 程序正确性缺陷）。非阻塞读循环+EOF(pclose 端关闭)
    // 后再收尸。
    char buf[4096];
    std::string output;
    bool out_open = true, err_open = true;
    while (out_open || err_open) {
      bool progressed = false;
      if (out_open) {
        ssize_t n = ::read(pipe_out[0], buf, sizeof(buf));
        if (n > 0) { output.append(buf, static_cast<size_t>(n)); progressed = true; }
        else if (n == 0) out_open = false;        // EOF
        else if (errno == EAGAIN || errno == EINTR) { /* 稍后再试 */ }
        else out_open = false;                     // EBADF 等
      }
      if (err_open) {
        ssize_t n = ::read(pipe_err[0], buf, sizeof(buf));
        if (n > 0) { output.append(buf, static_cast<size_t>(n)); progressed = true; }
        else if (n == 0) err_open = false;
        else if (errno == EAGAIN || errno == EINTR) { /* 稍后再试 */ }
        else err_open = false;
      }
      if (!progressed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    int status;
    ::waitpid(child, &status, 0);

    ::close(pipe_out[0]);
    ::close(pipe_err[0]);

    // 写结果到临时文件（父子进程间通信）
    std::string result_path = tmp_path + ".result";
    {
      std::ofstream rf(result_path);
      if (WIFEXITED(status))
        rf << "EXIT:" << WEXITSTATUS(status) << "\n";
      else if (WIFSIGNALED(status))
        rf << "SIG:" << WTERMSIG(status) << "\n";
      else
        rf << "EXIT:-1\n";
      rf << "OUTPUT:" << output.size() << "\n" << output;
    }

    ::_exit(0);
  }

  // 父进程: 等待子进程，超时控制
  int status;
  int wait_result = 0;

  // 超时轮询（elapsed 以 100ms 为单位，timeout_sec 以秒为单位）
  int elapsed = 0;  // 单位: 100ms 滴答
  int max_ticks = timeout_sec * 10;  // 每秒 10 个滴答
  while (elapsed < max_ticks) {
    wait_result = ::waitpid(pid, &status, WNOHANG);
    if (wait_result > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ++elapsed;
  }

  auto end = std::chrono::steady_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

  if (wait_result <= 0) {
    // 超时: 杀进程树
    // v0.52.10: 中间子进程也要自立进程组（kill(-pid) 才有效）——
    // 否则 kill(pid) 只杀中间层，孙进程孤儿化残留。
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    safe_unlink(tmp_path);
    safe_unlink(tmp_path + ".result");
    return {
      {"success", false},
      {"error", "timeout"},
      {"elapsed_ms", elapsed_ms}
    };
  }

#endif  // _WIN32

  // 读结果文件
  std::string result_path = tmp_path + ".result";
  std::string result_text = read_all(result_path);

  // 清理
  safe_unlink(tmp_path);
  safe_unlink(result_path);

  // 解析结果（带异常保护，防止损坏的结果文件崩溃）
  int exit_code = -1;
  bool killed = false;
  std::string output;

  std::istringstream rs(result_text);
  std::string line;
  while (std::getline(rs, line)) {
    if (line.rfind("EXIT:", 0) == 0) {
      try { exit_code = std::stoi(line.substr(5)); }
      catch (...) { exit_code = -1; }
    } else if (line.rfind("SIG:", 0) == 0) {
      killed = true;
      exit_code = -1;
    } else if (line.rfind("OUTPUT:", 0) == 0) {
      try {
        int output_size = std::stoi(line.substr(7));
        if (output_size > 0 && output_size < 10 * 1024 * 1024) {  // 上限 10MB
          output.resize(static_cast<size_t>(output_size));
          rs.read(&output[0], output_size);
        }
      } catch (...) { /* partial write — output stays empty */ }
    }
  }

  // 截断过长输出
  constexpr int kMaxOutput = 50000;
  if (static_cast<int>(output.size()) > kMaxOutput) {
    output.resize(kMaxOutput);
    output += "\n... [output truncated at 50KB]";
  }

  bool success = !killed && exit_code == 0;

  return {
    {"success", success},
    {"output", output},
    {"exit_code", killed ? -1 : exit_code},
    {"elapsed_ms", elapsed_ms},
    {"killed", killed}
  };
}

}  // namespace code_exec
}  // namespace thin_agent

// ── 插件入口 ──

extern "C" const char* thin_agent_plugin_init(
    thin_agent::SkillRegistry& registry) {

  registry.register_cpp_handler("code_exec", thin_agent::code_exec::handle_code_exec);

  return "code_exec";
}

// v0.53.6: v2 入口——interpreters 词表经 ctx 配置通道注入
//（chat_policy.json 的 interpreters 段；核心侧注入键 = "code_exec"）。
// 词表形态：{"interpreters": {"ruby": {"bin": "ruby", "ext": ".rb", "args": []}}}
// 合并语义：配置项覆盖内置同名项、追加新语言；内置 python/node 永远保底。
extern "C" const char* thin_agent_plugin_init2(
    thin_agent::SkillRegistry& registry, thin_agent::PluginContext& ctx) {

  // ctx.config("code_exec") 直接返回 interpreters 段（{lang: {bin,ext,args}}）
  auto interp_cfg = ctx.config("code_exec");
  if (interp_cfg.is_object()) {
    for (auto it = interp_cfg.begin();
         it != interp_cfg.end(); ++it) {
      if (it.key() == "_doc") continue;  // 词表文档键跳过
      if (!it.value().is_object()) continue;
      thin_agent::code_exec::InterpreterSpec spec;
      spec.bin = it.value().value("bin", "");
      spec.ext = it.value().value("ext", ".txt");
      if (it.value().contains("args") && it.value()["args"].is_array())
        for (const auto& a : it.value()["args"])
          if (a.is_string()) spec.args.push_back(a.get<std::string>());
      if (!spec.bin.empty()) {
        std::lock_guard<std::mutex> lk2(thin_agent::code_exec::g_interp_mu);
        thin_agent::code_exec::g_interpreters[it.key()] = std::move(spec);
      }
    }
  }

  registry.register_cpp_handler("code_exec", thin_agent::code_exec::handle_code_exec);

  return "code_exec";
}
