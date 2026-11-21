#include "thin_agent/core/LocalLlmSidecar.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "thin_agent/llm/CurlHttpClient.h"

#include <nlohmann/json.hpp>

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
  return path;
}

std::string default_server_path() {
  if (const char* env = std::getenv("THIN_AGENT_LLAMA_SERVER")) {
    if (env && *env) return env;
  }
#ifdef _WIN32
  const char* cand[] = {
      R"(E:\deps-thin-agent\llama.cpp\bin\llama-server.exe)",
      R"(E:/deps-thin-agent/llama.cpp/bin/llama-server.exe)",
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

bool parse_host_port_from_api_base(const std::string& api_base, std::string* host,
                                   int* port) {
  if (!host || !port || api_base.empty()) return false;
  std::string s = api_base;
  const auto scheme = s.find("://");
  if (scheme != std::string::npos) s = s.substr(scheme + 3);
  const auto slash = s.find('/');
  if (slash != std::string::npos) s = s.substr(0, slash);
  const auto colon = s.rfind(':');
  if (colon == std::string::npos) return false;
  *host = s.substr(0, colon);
  try {
    *port = std::stoi(s.substr(colon + 1));
  } catch (...) {
    return false;
  }
  return !host->empty() && *port > 0;
}

}  // namespace

LocalLlmSidecar::~LocalLlmSidecar() { stop(); }

std::string LocalLlmSidecar::make_api_base(const DemoConfigCompat& cfg) {
  if (!cfg.api_base.empty()) return trim_slash(cfg.api_base);
  std::string host;
  int port = 0;
  resolve_listen(cfg, &host, &port);
  return "http://" + host + ":" + std::to_string(port) + "/v1";
}

void LocalLlmSidecar::resolve_listen(const DemoConfigCompat& cfg, std::string* host,
                                     int* port) {
  // api_base 与监听地址单一来源：有 api_base 时优先从其 URL 拆 host/port，
  // 避免客户端打 A、进程听 B。
  std::string h;
  int p = 0;
  if (!cfg.api_base.empty() && parse_host_port_from_api_base(cfg.api_base, &h, &p)) {
    if (host) *host = h;
    if (port) *port = p;
    return;
  }
  h = cfg.sidecar_host.empty() ? "127.0.0.1" : cfg.sidecar_host;
  p = cfg.sidecar_port > 0 ? cfg.sidecar_port : 1234;
  if (host) *host = h;
  if (port) *port = p;
}

std::string LocalLlmSidecar::identity_of(const DemoConfigCompat& cfg) {
  std::ostringstream oss;
  oss << make_api_base(cfg) << "|"
      << (cfg.sidecar_model_path.empty() ? "models/gguf/aurum-brain-q4_k_m.gguf"
                                         : cfg.sidecar_model_path)
      << "|" << cfg.model_name << "|n_ctx=" << cfg.sidecar_n_ctx;
  return oss.str();
}

bool LocalLlmSidecar::probe(const std::string& api_base, int timeout_ms,
                            const std::string& expect_model) {
  std::string base =
      api_base.empty() ? "http://127.0.0.1:1234/v1" : trim_slash(api_base);
  const std::string url = base + "/models";
  CurlHttpClient http(timeout_ms > 0 ? timeout_ms : 2000);
  auto resp = http.get(url, "");
  if (!(resp.ok && resp.status_code >= 200 && resp.status_code < 300)) return false;
  if (expect_model.empty()) return true;
  return resp.body.find(expect_model) != std::string::npos;
}

int LocalLlmSidecar::fetch_n_ctx(const std::string& api_base, int timeout_ms) {
  std::string base =
      api_base.empty() ? "http://127.0.0.1:1234/v1" : trim_slash(api_base);
  CurlHttpClient http(timeout_ms > 0 ? timeout_ms : 2500);

  auto try_models = [&]() -> int {
    auto resp = http.get(base + "/models", "");
    if (!(resp.ok && resp.status_code >= 200 && resp.status_code < 300)) return 0;
    try {
      auto j = nlohmann::json::parse(resp.body);
      auto dig = [](const nlohmann::json& node) -> int {
        if (!node.is_object()) return 0;
        if (node.contains("meta") && node["meta"].is_object()) {
          const int v = node["meta"].value("n_ctx", 0);
          if (v > 0) return v;
        }
        return node.value("n_ctx", 0);
      };
      if (j.contains("data") && j["data"].is_array() && !j["data"].empty()) {
        const int v = dig(j["data"].front());
        if (v > 0) return v;
      }
      if (j.contains("models") && j["models"].is_array() && !j["models"].empty()) {
        const int v = dig(j["models"].front());
        if (v > 0) return v;
      }
    } catch (...) {
    }
    return 0;
  };

  if (const int v = try_models(); v > 0) return v;

  // /props 在 /v1 同级的上一层：http://host:port/props
  std::string root = base;
  if (root.size() >= 3 && root.compare(root.size() - 3, 3, "/v1") == 0)
    root = root.substr(0, root.size() - 3);
  auto resp = http.get(root + "/props", "");
  if (!(resp.ok && resp.status_code >= 200 && resp.status_code < 300)) return 0;
  try {
    auto j = nlohmann::json::parse(resp.body);
    if (j.contains("default_generation_settings") &&
        j["default_generation_settings"].is_object()) {
      const int v = j["default_generation_settings"].value("n_ctx", 0);
      if (v > 0) return v;
    }
    return j.value("n_ctx", 0);
  } catch (...) {
  }
  return 0;
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
                                   const std::string& expect_model, uint64_t gen,
                                   std::string* err) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (cancel_gen_.load() != gen) {
      if (err) *err = "sidecar cancelled";
      return false;
    }
    if (probe(api_base, 1500, expect_model)) return true;
#ifdef _WIN32
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (process_handle_) {
        DWORD code = STILL_ACTIVE;
        if (GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code) &&
            code != STILL_ACTIVE) {
          if (err)
            *err = "llama-server exited early (code=" + std::to_string(code) + ")";
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

  std::string host;
  int port = 0;
  resolve_listen(cfg, &host, &port);

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
  if (hJob) {
    if (!AssignProcessToJobObject(hJob, pi.hProcess)) {
      std::cerr << "[LocalLlmSidecar] warn: AssignProcessToJobObject failed ("
                << GetLastError() << ")\n";
    }
  }
  CloseHandle(pi.hThread);

  {
    std::lock_guard<std::mutex> lk(mu_);
    process_handle_ = pi.hProcess;
    job_handle_ = hJob;
    pid_ = pi.dwProcessId;
    port_ = port;
    api_base_ = make_api_base(cfg);
    identity_ = identity_of(cfg);
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
    identity_ = identity_of(cfg);
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

  const uint64_t gen = cancel_gen_.load();
  const std::string want_id = identity_of(cfg);
  const std::string base = make_api_base(cfg);
  const std::string expect_model = cfg.model_name;
  {
    std::lock_guard<std::mutex> lk(mu_);
    api_base_ = base;
  }

  auto cancelled = [&]() { return cancel_gen_.load() != gen; };

  // 已有健康且身份匹配的端点 → 复用（外部或本进程上次拉起）
  if (probe(base, 2000, expect_model)) {
    std::string cur_id;
    {
      std::lock_guard<std::mutex> lk(mu_);
      cur_id = identity_;
    }
    const int live_ctx = fetch_n_ctx(base);
    const bool ctx_ok =
        cfg.sidecar_n_ctx <= 0 || live_ctx <= 0 || live_ctx == cfg.sidecar_n_ctx;
    if (ctx_ok && (cur_id.empty() || cur_id == want_id)) {
      std::lock_guard<std::mutex> lk(mu_);
      identity_ = want_id;
      std::cerr << "[LocalLlmSidecar] reuse healthy endpoint " << base
                << (live_ctx > 0 ? (" n_ctx=" + std::to_string(live_ctx)) : "")
                << std::endl;
      return true;
    }
    // 同端口但身份 / n_ctx 变了：我们拥有则重建；外部则报错
    if (!owned_.load()) {
      if (err) {
        if (!ctx_ok) {
          *err = "endpoint n_ctx=" + std::to_string(live_ctx) +
                 " but profile wants " + std::to_string(cfg.sidecar_n_ctx) +
                 " (stop the process on " + base + " and retry)";
        } else {
          *err = "endpoint healthy but model identity mismatch (external process on " +
                 base + ")";
        }
      }
      return false;
    }
  }

  if (cancelled()) {
    if (err) *err = "sidecar cancelled";
    return false;
  }

  // 已持有匹配身份的进程但尚未就绪 → 继续等
  if (owned_.load() && is_running()) {
    std::string cur_id;
    {
      std::lock_guard<std::mutex> lk(mu_);
      cur_id = identity_;
    }
    if (cur_id == want_id) {
      const int wait_ms =
          cfg.sidecar_ready_timeout_ms > 0 ? cfg.sidecar_ready_timeout_ms : 120000;
      return wait_healthy(base, wait_ms, expect_model, gen, err);
    }
  }

  // 身份变化或无进程：只杀托管进程（不 bump cancel），再拉起
  kill_owned();
  if (cancelled()) {
    if (err) *err = "sidecar cancelled";
    return false;
  }
  if (!spawn(cfg, err)) return false;
  if (cancelled()) {
    kill_owned();
    if (err) *err = "sidecar cancelled";
    return false;
  }

  const int wait_ms =
      cfg.sidecar_ready_timeout_ms > 0 ? cfg.sidecar_ready_timeout_ms : 120000;
  if (!wait_healthy(base, wait_ms, expect_model, gen, err)) {
    kill_owned();
    return false;
  }
  std::cerr << "[LocalLlmSidecar] ready at " << base << std::endl;
  return true;
}

void LocalLlmSidecar::kill_owned() {
  if (!owned_.exchange(false)) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(mu_);
    if (process_handle_) {
      CloseHandle(static_cast<HANDLE>(process_handle_));
      process_handle_ = nullptr;
    }
    if (job_handle_) {
      CloseHandle(static_cast<HANDLE>(job_handle_));
      job_handle_ = nullptr;
    }
    pid_ = 0;
    identity_.clear();
#else
    std::lock_guard<std::mutex> lk(mu_);
    pid_ = -1;
    identity_.clear();
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
    identity_.clear();
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
    identity_.clear();
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

void LocalLlmSidecar::stop() {
  cancel_gen_.fetch_add(1, std::memory_order_acq_rel);
  kill_owned();
}

}  // namespace thin_agent
