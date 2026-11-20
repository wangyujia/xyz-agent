#include "thin_agent/core/LocalLlmSidecar.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "thin_agent/llm/CurlHttpClient.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace thin_agent {
namespace {

std::string trim_slash(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

std::string quote_arg(const std::string& s) {
  if (s.find_first_of(" \t\"") == std::string::npos) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += "\\\"";
    else out += c;
  }
  out += '"';
  return out;
}

std::string resolve_existing(const std::string& path) {
  namespace fs = std::filesystem;
  if (path.empty()) return {};
  std::error_code ec;
  if (fs::exists(path, ec)) return path;
  const fs::path alt = fs::path("..") / path;
  if (fs::exists(alt, ec)) return alt.string();
  return path;  // 交给启动失败报错
}

std::string default_server_path() {
  if (const char* env = std::getenv("THIN_AGENT_LLAMA_SERVER")) {
    if (env && *env) return env;
  }
#ifdef _WIN32
  const char* cand[] = {
      R"(C:\deps-thin-agent\llama.cpp\bin\llama-server.exe)",
      R"(C:/deps-thin-agent/llama.cpp/bin/llama-server.exe)",
      "llama-server.exe",
  };
#else
  const char* cand[] = {
      "/opt/llama.cpp/bin/llama-server",
      "llama-server",
  };
#endif
  namespace fs = std::filesystem;
  for (const char* p : cand) {
    std::error_code ec;
    if (fs::exists(p, ec)) return p;
  }
  return cand[0];
}

}  // namespace

LocalLlmSidecar::~LocalLlmSidecar() { stop(); }

std::string LocalLlmSidecar::make_api_base(const DemoConfigCompat& cfg) {
  if (!cfg.api_base.empty()) return trim_slash(cfg.api_base);
  const std::string host =
      cfg.sidecar_host.empty() ? "127.0.0.1" : cfg.sidecar_host;
  const int port = cfg.sidecar_port > 0 ? cfg.sidecar_port : 1234;
  return "http://" + host + ":" + std::to_string(port) + "/v1";
}

bool LocalLlmSidecar::probe(const std::string& api_base, int timeout_ms) {
  std::string base =
      api_base.empty() ? "http://127.0.0.1:1234/v1" : trim_slash(api_base);
  const std::string url = base + "/models";
  CurlHttpClient http(timeout_ms > 0 ? timeout_ms : 2000);
  auto resp = http.get(url, "");
  return resp.ok && resp.status_code >= 200 && resp.status_code < 300;
}

std::string LocalLlmSidecar::api_base() const {
  std::lock_guard<std::mutex> lk(mu_);
  return api_base_;
}

bool LocalLlmSidecar::is_running() const {
  std::lock_guard<std::mutex> lk(mu_);
#ifdef _WIN32
  if (!process_handle_) return false;
  DWORD code = STILL_ACTIVE;
  if (!GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code)) return false;
  return code == STILL_ACTIVE;
#else
  if (pid_ <= 0) return false;
  int status = 0;
  const pid_t r = waitpid(pid_, &status, WNOHANG);
  if (r == 0) return true;
  return false;
#endif
}

bool LocalLlmSidecar::wait_healthy(const std::string& api_base, int timeout_ms,
                                   std::string* err) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (stopping_.load()) {
      if (err) *err = "sidecar stop requested";
      return false;
    }
    if (probe(api_base, 1500)) return true;
#ifdef _WIN32
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (process_handle_) {
        DWORD code = STILL_ACTIVE;
        if (GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code) &&
            code != STILL_ACTIVE) {
          if (err) *err = "llama-server exited early (code=" + std::to_string(code) + ")";
          return false;
        }
      }
    }
#else
    if (!is_running() && owned_.load()) {
      if (err) *err = "llama-server exited early";
      return false;
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  if (err) *err = "llama-server health timeout (" + std::to_string(timeout_ms) + "ms)";
  return false;
}

bool LocalLlmSidecar::spawn(const DemoConfigCompat& cfg, std::string* err) {
  std::string server = cfg.sidecar_server_path.empty()
                           ? default_server_path()
                           : resolve_existing(cfg.sidecar_server_path);
  std::string model = cfg.sidecar_model_path.empty()
                          ? "models/gguf/aurum-brain-q4_k_m.gguf"
                          : cfg.sidecar_model_path;
  model = resolve_existing(model);

  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::exists(server, ec)) {
    if (err) *err = "llama-server not found: " + server;
    return false;
  }
  if (!fs::exists(model, ec)) {
    if (err) *err = "sidecar model not found: " + model;
    return false;
  }

  const std::string host =
      cfg.sidecar_host.empty() ? "127.0.0.1" : cfg.sidecar_host;
  const int port = cfg.sidecar_port > 0 ? cfg.sidecar_port : 1234;

  std::ostringstream cmd;
  cmd << quote_arg(server) << " -m " << quote_arg(model) << " --host " << host
      << " --port " << port << " --jinja";
  if (!cfg.model_name.empty())
    cmd << " --alias " << quote_arg(cfg.model_name);
  if (cfg.sidecar_n_ctx > 0) cmd << " -c " << cfg.sidecar_n_ctx;
  if (cfg.sidecar_n_gpu_layers >= 0)
    cmd << " -ngl " << cfg.sidecar_n_gpu_layers;

  const std::string cmdline = cmd.str();
  std::cerr << "[LocalLlmSidecar] start: " << cmdline << std::endl;

#ifdef _WIN32
  HANDLE hJob = CreateJobObjectA(nullptr, nullptr);
  if (hJob) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli,
                            sizeof(jeli));
  }

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::string mutable_cmd = cmdline;
  BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
                           nullptr, &si, &pi);
  if (!ok) {
    if (hJob) CloseHandle(hJob);
    if (err) *err = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
  CloseHandle(pi.hThread);

  {
    std::lock_guard<std::mutex> lk(mu_);
    process_handle_ = pi.hProcess;
    job_handle_ = hJob;
    pid_ = pi.dwProcessId;
    port_ = port;
    api_base_ = make_api_base(cfg);
  }
  owned_.store(true);
  return true;
#else
  const pid_t pid = fork();
  if (pid < 0) {
    if (err) *err = "fork failed";
    return false;
  }
  if (pid == 0) {
    // child
    std::vector<std::string> args_store = {server, "-m", model, "--host", host,
                                           "--port", std::to_string(port), "--jinja"};
    if (!cfg.model_name.empty()) {
      args_store.push_back("--alias");
      args_store.push_back(cfg.model_name);
    }
    if (cfg.sidecar_n_ctx > 0) {
      args_store.push_back("-c");
      args_store.push_back(std::to_string(cfg.sidecar_n_ctx));
    }
    if (cfg.sidecar_n_gpu_layers >= 0) {
      args_store.push_back("-ngl");
      args_store.push_back(std::to_string(cfg.sidecar_n_gpu_layers));
    }
    std::vector<char*> argv;
    argv.reserve(args_store.size() + 1);
    for (auto& s : args_store) argv.push_back(s.data());
    argv.push_back(nullptr);
    execv(server.c_str(), argv.data());
    _exit(127);
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    pid_ = static_cast<int>(pid);
    port_ = port;
    api_base_ = make_api_base(cfg);
  }
  owned_.store(true);
  return true;
#endif
}

bool LocalLlmSidecar::ensure_running(const DemoConfigCompat& cfg, std::string* err) {
  std::lock_guard<std::mutex> ensure_lk(ensure_mu_);
  if (!cfg.sidecar_enabled) {
    if (err) *err = "sidecar_disabled";
    return false;
  }
  stopping_.store(false);

  const std::string base = make_api_base(cfg);
  {
    std::lock_guard<std::mutex> lk(mu_);
    api_base_ = base;
  }

  // 已有健康端点（手动 llama-server / 上次拉起仍活着）→ 复用
  if (probe(base, 2000)) {
    std::cerr << "[LocalLlmSidecar] reuse healthy endpoint " << base << std::endl;
    return true;
  }

  // 我们已持有进程但尚未就绪 → 继续等
  if (owned_.load() && is_running()) {
    const int wait_ms =
        cfg.sidecar_ready_timeout_ms > 0 ? cfg.sidecar_ready_timeout_ms : 120000;
    return wait_healthy(base, wait_ms, err);
  }

  // 清理残留句柄后重新拉起
  stop();
  if (!spawn(cfg, err)) return false;

  const int wait_ms =
      cfg.sidecar_ready_timeout_ms > 0 ? cfg.sidecar_ready_timeout_ms : 120000;
  if (!wait_healthy(base, wait_ms, err)) {
    stop();
    return false;
  }
  std::cerr << "[LocalLlmSidecar] ready at " << base << std::endl;
  return true;
}

void LocalLlmSidecar::stop() {
  stopping_.store(true);
  if (!owned_.exchange(false)) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(mu_);
    // 未托管时仍清理可能残留的句柄字段（不杀外部进程）
    if (process_handle_) {
      // 仅关闭句柄，不 Terminate
      CloseHandle(static_cast<HANDLE>(process_handle_));
      process_handle_ = nullptr;
    }
    if (job_handle_) {
      CloseHandle(static_cast<HANDLE>(job_handle_));
      job_handle_ = nullptr;
    }
    pid_ = 0;
#else
    std::lock_guard<std::mutex> lk(mu_);
    pid_ = -1;
#endif
    return;
  }

#ifdef _WIN32
  HANDLE proc = nullptr;
  HANDLE job = nullptr;
  {
    std::lock_guard<std::mutex> lk(mu_);
    proc = static_cast<HANDLE>(process_handle_);
    job = static_cast<HANDLE>(job_handle_);
    process_handle_ = nullptr;
    job_handle_ = nullptr;
    pid_ = 0;
  }
  if (proc) {
    TerminateProcess(proc, 1);
    WaitForSingleObject(proc, 5000);
    CloseHandle(proc);
  }
  if (job) CloseHandle(job);
#else
  int pid = -1;
  {
    std::lock_guard<std::mutex> lk(mu_);
    pid = pid_;
    pid_ = -1;
  }
  if (pid > 0) {
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {
      int st = 0;
      if (waitpid(pid, &st, WNOHANG) != 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (waitpid(pid, nullptr, WNOHANG) == 0) {
      kill(pid, SIGKILL);
      waitpid(pid, nullptr, 0);
    }
  }
#endif
  std::cerr << "[LocalLlmSidecar] stopped" << std::endl;
}

}  // namespace thin_agent
