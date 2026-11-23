#include "thin_agent/core/LocalLlmSidecar.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// 最后一个可写固定盘上的约定安装位置。盘符不写死。
std::string llama_server_on_drive(const std::string& drive) {
  return drive + "\\deps-thin-agent\\llama.cpp\\bin\\llama-server.exe";
}

std::vector<std::string> writable_fixed_drives() {
  std::vector<std::string> drives;
#ifdef _WIN32
  const DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if ((mask & (1u << i)) == 0) continue;
    const std::string root = std::string(1, static_cast<char>('A' + i)) + ":\\";
    if (GetDriveTypeA(root.c_str()) != DRIVE_FIXED) continue;
    DWORD flags = 0;
    if (!GetVolumeInformationA(root.c_str(), nullptr, 0, nullptr, nullptr, &flags,
                               nullptr, 0)) {
      continue;
    }
    if (flags & FILE_READ_ONLY_VOLUME) continue;
    drives.push_back(std::string(1, static_cast<char>('A' + i)) + ":");
  }
#endif
  return drives;
}

std::string default_server_path() {
  if (const char* env = std::getenv("THIN_AGENT_LLAMA_SERVER")) {
    if (env && *env) return env;
  }
  namespace fs = std::filesystem;
#ifdef _WIN32
  const std::vector<std::string> drives = writable_fixed_drives();
  for (auto it = drives.rbegin(); it != drives.rend(); ++it) {
    const std::string path = llama_server_on_drive(*it);
    std::error_code ec;
    if (fs::exists(path, ec)) return path;
  }
  std::error_code ec;
  if (fs::exists("llama-server.exe", ec)) return "llama-server.exe";
  if (!drives.empty()) return llama_server_on_drive(drives.back());
  return "llama-server.exe";
#else
  const char* cand[] = {
      "/opt/llama.cpp/bin/llama-server",
      "llama-server",
  };
  for (const char* p : cand) {
    std::error_code ec;
    if (fs::exists(p, ec)) return p;
  }
  return cand[0];
#endif
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

namespace {

struct RouterSpec {
  const char* id;
  const char* file;
  const char* alias;
  int n_ctx;
};

// 段名是文件名去掉扩展名，和 ModelPool / models-dir 的 id 一致。
// alias 不能等于另一个模型 id。14B 的请求名就是它自己的 id。
const RouterSpec kRouterSpecs[] = {
    {"aurum-brain-q4_k_m", "aurum-brain-q4_k_m.gguf", "aurum-brain", 32768},
    {"qwen2.5-coder-7b-instruct-q4_k_m", "qwen2.5-coder-7b-instruct-q4_k_m.gguf",
     "", 16384},
    {"qwen2.5-coder-14b-instruct-q4_k_m", "qwen2.5-coder-14b-instruct-q4_k_m.gguf",
     "", 16384},
    {"qwen2.5-0.5b-instruct-q2_k", "qwen2.5-0.5b-instruct-q2_k.gguf", "", 32768},
    {"gemma-3-1b-it-q4_k_m", "gemma-3-1b-it-q4_k_m.gguf", "", 32768},
};

const RouterSpec* find_router_spec(const std::string& name) {
  for (const auto& spec : kRouterSpecs) {
    if (name == spec.id) return &spec;
    if (spec.alias && spec.alias[0] && name == spec.alias) return &spec;
  }
  return nullptr;
}

std::filesystem::path resolve_models_dir() {
  namespace fs = std::filesystem;
  const fs::path cands[] = {
      fs::path("models") / "gguf",
      fs::path("..") / "models" / "gguf",
      fs::path("backends") / "thin_agent" / "models" / "gguf",
  };
  for (const auto& cand : cands) {
    std::error_code ec;
    if (fs::is_directory(cand, ec)) return fs::absolute(cand, ec);
  }
  return {};
}

bool write_router_preset(const std::filesystem::path& dir,
                         const std::filesystem::path& preset, int n_gpu_layers,
                         std::string* err) {
  std::ofstream out(preset.string(), std::ios::binary | std::ios::trunc);
  if (!out) {
    if (err) *err = "cannot write router preset: " + preset.string();
    return false;
  }
  int written = 0;
  for (const auto& spec : kRouterSpecs) {
    const auto file = dir / spec.file;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) continue;
    out << "[" << spec.id << "]\n";
    out << "ctx-size = " << spec.n_ctx << "\n";
    out << "model = " << file.string() << "\n";
    if (spec.alias && spec.alias[0]) out << "alias = " << spec.alias << "\n";
    if (n_gpu_layers >= 0) out << "n-gpu-layers = " << n_gpu_layers << "\n";
    out << "\n";
    ++written;
  }
  if (written == 0) {
    if (err) *err = "no GGUF for router preset in " + dir.string();
    return false;
  }
  return static_cast<bool>(out);
}

bool endpoint_is_router(const std::string& api_base) {
  std::string root = api_base;
  while (!root.empty() && root.back() == '/') root.pop_back();
  if (root.size() >= 3 && root.compare(root.size() - 3, 3, "/v1") == 0)
    root = root.substr(0, root.size() - 3);
  CurlHttpClient http(2000);
  auto resp = http.get(root + "/props", "");
  if (!(resp.ok && resp.status_code >= 200 && resp.status_code < 300)) return false;
  return resp.body.find("\"role\":\"router\"") != std::string::npos ||
         resp.body.find("\"role\": \"router\"") != std::string::npos;
}

}  // namespace

std::string LocalLlmSidecar::identity_of(const DemoConfigCompat& cfg) {
  std::ostringstream oss;
  oss << make_api_base(cfg) << "|router|max=1";
  if (cfg.sidecar_n_gpu_layers >= 0)
    oss << "|ngl=" << cfg.sidecar_n_gpu_layers;
  return oss.str();
}

int LocalLlmSidecar::router_context_tokens(const std::string& name) {
  if (const auto* spec = find_router_spec(name)) return spec->n_ctx;
  return 0;
}

std::string LocalLlmSidecar::router_request_model(const std::string& pool_name) {
  if (const auto* spec = find_router_spec(pool_name)) {
    if (spec->alias && spec->alias[0]) return spec->alias;
    return spec->id;
  }
  return pool_name;
}

std::string LocalLlmSidecar::router_pool_name(const std::string& name) {
  if (const auto* spec = find_router_spec(name)) return spec->id;
  return name;
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

  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::exists(server, ec)) {
    if (err) *err = "llama-server not found: " + server;
    return false;
  }
  const fs::path models_dir = resolve_models_dir();
  if (models_dir.empty()) {
    if (err) *err = "models/gguf directory not found";
    return false;
  }
  const fs::path preset = models_dir / "router-presets.ini";
  if (!write_router_preset(models_dir, preset, cfg.sidecar_n_gpu_layers, err))
    return false;

  std::string host;
  int port = 0;
  resolve_listen(cfg, &host, &port);

  // router：进程只起一次。--models-max 1 保证内存里同时只有一份权重。
  // 上下文写在 preset 里，不在命令行上放全局 -c，也不在启动时 -m 加载。
  std::vector<std::string> args = {
      server,
      "--models-dir",
      models_dir.string(),
      "--models-preset",
      preset.string(),
      "--models-max",
      "1",
      "--host",
      host,
      "--port",
      std::to_string(port),
      "--jinja",
  };
  std::ostringstream cmd;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) cmd << ' ';
    cmd << quote_arg(args[i]);
  }
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
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& s : args) argv.push_back(s.data());
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

  // 已有 router 且身份匹配 → 复用。当前模型的 n_ctx 在 preset 里，
  // 换模型不会改身份，也不按 sidecar_n_ctx 重建进程。
  if (probe(base, 2000, expect_model)) {
    const bool router_live = endpoint_is_router(base);
    std::string cur_id;
    {
      std::lock_guard<std::mutex> lk(mu_);
      cur_id = identity_;
    }
    if (router_live && (cur_id.empty() || cur_id == want_id)) {
      std::lock_guard<std::mutex> lk(mu_);
      identity_ = want_id;
      std::cerr << "[LocalLlmSidecar] reuse router " << base << std::endl;
      return true;
    }
    // 外部的单模型进程不杀。我们自己拉起的旧进程落到下面重建。
    if (!owned_.load()) {
      if (err) {
        if (!router_live) {
          *err = "端口上是单模型 llama-server，请先停下它，再由 sidecar 以 router 启动 (" +
                 base + ")";
        } else {
          *err = "endpoint healthy but router identity mismatch (external process on " +
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
