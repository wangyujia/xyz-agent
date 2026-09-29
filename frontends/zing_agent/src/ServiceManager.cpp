// zing_agent P2: 本机 thin_agent 服务代管（实现）
#include "ServiceManager.h"

#ifdef _WIN32
// Windows: 探活走 WinSock2（_WIN32_WINNT 先于 winsock2 定义）
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
using socklen_t = int;

// v4: winsock 全局初始化（首次 probe 前惰性 WSAStartup——探活/启动前置）
struct WsaInit {
  bool ok = false;
  WsaInit() {
    WSADATA d;
    ok = (WSAStartup(MAKEWORD(2, 2), &d) == 0);
  }
  ~WsaInit() { if (ok) WSACleanup(); }
};
static WsaInit g_wsa;  // 静态对象——进程级一次
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include <vector>

namespace zing {

// ── 探活：TCP 连 port → GET /stats，HTTP/1.x 200 即健康 ──────────
#ifdef _WIN32
static void close_sock(SOCKET fd) { ::closesocket(fd); }
#else
static void close_sock(int fd) { ::close(fd); }
#endif

bool ServiceManager::probe(int timeout_ms) const {
#ifdef _WIN32
  // Windows: 同语义（u_long 非阻塞/ioctlsocket/WSAGetLastError）
  SOCKET fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) return false;
  u_long nb = 1;
  ::ioctlsocket(fd, FIONBIO, &nb);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u_short>(cfg_.port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  const int r = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (r != 0 && WSAGetLastError() != WSAEWOULDBLOCK) { close_sock(fd); return false; }
  if (r != 0) {
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (::select(static_cast<int>(fd) + 1, nullptr, &wset, nullptr, &tv) <= 0) {
      close_sock(fd);
      return false;
    }
    char err = 0;
    int elen = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) { close_sock(fd); return false; }
  }
  return probe_http(fd);
#else
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  // 非阻塞连接
  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(cfg_.port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  const int r = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (r != 0 && errno != EINPROGRESS) { ::close(fd); return false; }
  if (r != 0) {
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (::select(fd + 1, nullptr, &wset, nullptr, &tv) <= 0) {
      ::close(fd);
      return false;
    }
    int err = 0;
    socklen_t elen = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) { ::close(fd); return false; }
  }
  return probe_http(fd);
#endif
}

// v3: 公共收尾——HTTP 探测（两平台共用；fd 类型经模板/宏隔离）
#ifdef _WIN32
bool ServiceManager::probe_http(SOCKET fd) const {
  // v17: /stats → /health——v0.53.32 鉴权后 /stats 无 token 返回 401 假阴性
  const std::string req = "GET /health HTTP/1.0\r\nHost: 127.0.0.1\r\n"
                          "Connection: close\r\n\r\n";
  if (::send(fd, req.c_str(), static_cast<int>(req.size()), 0) < 0) { close_sock(fd); return false; }
  fd_set rset;
  FD_ZERO(&rset);
  FD_SET(fd, &rset);
  struct timeval tv{1, 0};
  if (::select(static_cast<int>(fd) + 1, &rset, nullptr, nullptr, &tv) <= 0) {
    close_sock(fd);
    return false;
  }
  std::string buf(256, '\0');
  const int n = ::recv(fd, &buf[0], static_cast<int>(buf.size()) - 1, 0);
  close_sock(fd);
  if (n <= 0) return false;
  buf.resize(static_cast<size_t>(n));
  last_healthy_ = buf.compare(0, 7, "HTTP/1.") == 0 && buf.find(" 200 ") != std::string::npos;
  return last_healthy_;
}
#else
bool ServiceManager::probe_http(int fd) const {
  // v17: /stats → /health——同上,鉴权豁免探针
  const std::string req = "GET /health HTTP/1.0\r\nHost: 127.0.0.1\r\n"
                          "Connection: close\r\n\r\n";
  if (::write(fd, req.c_str(), req.size()) < 0) { ::close(fd); return false; }
  // 等可读（socket 仍是非阻塞——v0.53.29 实测 read 立即 EAGAIN 假阴性）
  {
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(fd, &rset);
    timeval tv{1, 0};
    if (::select(fd + 1, &rset, nullptr, nullptr, &tv) <= 0) {
      ::close(fd);
      return false;
    }
  }
  std::string buf;
  buf.resize(256);
  const ssize_t n = ::read(fd, &buf[0], buf.size() - 1);
  ::close(fd);
  if (n <= 0) return false;
  buf.resize(static_cast<size_t>(n));
  last_healthy_ = buf.compare(0, 7, "HTTP/1.") == 0 && buf.find(" 200 ") != std::string::npos;
  return last_healthy_;
}
#endif


// v2: PID 落盘——CLI 独立进程间共享"这是 zing 拉起的服务"（/tmp/zing_svc.pid）
static long read_pid_file(const zing::ServiceConfig& cfg) {
  if (cfg.pid_file.empty()) return -1;
  std::ifstream in(cfg.pid_file);
  long v = -1;
  in >> v;
  return in ? v : -1;
}
static void write_pid_file(const zing::ServiceConfig& cfg, long pid) {
  if (cfg.pid_file.empty()) return;
  if (pid <= 0) { ::remove(cfg.pid_file.c_str()); return; }
  std::ofstream out(cfg.pid_file);
  out << pid << "\n";
}

bool ServiceManager::is_our_child_alive() const {
  long pid = pid_.load();
  if (pid <= 0) pid = read_pid_file(cfg_);  // CLI 跨进程：读 pidfile
  if (pid <= 0) return false;
#ifdef _WIN32
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                         static_cast<DWORD>(pid));
  if (!h) return false;
  DWORD code = 0;
  GetExitCodeProcess(h, &code);
  CloseHandle(h);
  return code == STILL_ACTIVE;
#else
  if (::kill(static_cast<pid_t>(pid), 0) != 0) return false;
  int status = 0;
  const pid_t r = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
  return r == 0;  // 0=仍在运行（未收割）
#endif
}

std::map<std::string, std::string> ServiceManager::parse_env_file() const {
  std::map<std::string, std::string> env;
  if (cfg_.env_file.empty()) return env;
  std::ifstream in(cfg_.env_file);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto pos = line.find('=');
    if (pos == std::string::npos) continue;
    env[line.substr(0, pos)] = line.substr(pos + 1);
  }
  return env;
}

std::string ServiceManager::status_json() const {
  nlohmann::json j;
  j["running"] = is_our_child_alive() || probe(800);
  j["healthy"] = last_healthy_.load();
  long pid_show = pid_.load();
  if (pid_show <= 0) pid_show = read_pid_file(cfg_);
  j["pid"] = pid_show;
  j["port"] = cfg_.port;
  j["autostart"] = watching_.load();
  j["binary"] = cfg_.binary;
  return j.dump();
}

#ifdef _WIN32
bool ServiceManager::start() {
  // Windows: CreateProcess 拉起（P2 Windows 侧实现——swprintf 环境块由
  // env 文件注入；日志重定向 CreateFile）
  if (probe(1200)) return true;
  if (cfg_.binary.empty()) return false;
  STARTUPINFOA si{};
  PROCESS_INFORMATION pi{};
  si.cb = sizeof(si);
  std::string cmd = "\"" + cfg_.binary + "\" --port " + std::to_string(cfg_.port) +
                    " --config \"" + cfg_.config_yaml + "\"";
  if (!cfg_.profile.empty()) cmd += " --profile " + cfg_.profile;
  // 日志重定向（子进程句柄继承）
  // v4: hLog 打开失败时【不】带 STARTF_USESTDHANDLES——传 INVALID_HANDLE
  // 进句柄表会让 CreateProcess 直接失败（Windows 实测行为）
  HANDLE hLog = INVALID_HANDLE_VALUE;
  if (!cfg_.log_path.empty()) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    hLog = CreateFileA(cfg_.log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_WRITE,
                       &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog != INVALID_HANDLE_VALUE) {
      si.dwFlags |= STARTF_USESTDHANDLES;
      si.hStdOutput = hLog;
      si.hStdError = hLog;
    }
  }
  // 环境变量（继承父进程+env 文件注入——env 文件解析后拼块）
  std::string env_block;
  {
    // 简化：把 env 文件逐行 setenv 到自身再传 NULL（继承父环境）
    for (const auto& [k, v] : parse_env_file()) ::SetEnvironmentVariableA(k.c_str(), v.c_str());
  }
  const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
  if (!ok) return false;
  pid_ = static_cast<long>(pi.dwProcessId);
  write_pid_file(cfg_, pid_.load());
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  for (int i = 0; i < 40; ++i) {
    if (probe(600)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return probe(1000);
}
#else
bool ServiceManager::start() {
  if (probe(1200)) return true;  // 已在跑（可能是用户手起的）——接管探活即可
  if (cfg_.binary.empty() || ::access(cfg_.binary.c_str(), X_OK) != 0) return false;

  const pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    // 子进程：新会话+日志重定向+环境注入
    ::setsid();
    if (!cfg_.log_path.empty()) {
      const int logfd = ::open(cfg_.log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (logfd >= 0) {
        ::dup2(logfd, STDOUT_FILENO);
        ::dup2(logfd, STDERR_FILENO);
        ::close(logfd);
      }
    }
    for (const auto& [k, v] : parse_env_file()) ::setenv(k.c_str(), v.c_str(), 1);
    std::vector<std::string> args = {cfg_.binary,
                                     "--port", std::to_string(cfg_.port),
                                     "--config", cfg_.config_yaml};
    if (!cfg_.profile.empty()) args.push_back("--profile"), args.push_back(cfg_.profile);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    ::execv(argv[0], argv.data());
    _exit(127);  // exec 失败
  }
  pid_ = pid;
  write_pid_file(cfg_, pid);  // v2: 落盘共享
  // 等健康（最多 8s——服务启动含模型配置加载）
  for (int i = 0; i < 40; ++i) {
    if (probe(600)) return true;
    if (!is_our_child_alive() && !probe(200)) return false;  // 子进程死了且端口无服务
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return probe(1000);
}

#endif  // _WIN32 / POSIX start

bool ServiceManager::stop() {
  stop_watch();
  long pid = pid_.load();
  if (pid <= 0) pid = read_pid_file(cfg_);  // v2: CLI 跨进程
  if (pid > 0) {
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (h) {
      TerminateProcess(h, 0);
      WaitForSingleObject(h, 3000);
      CloseHandle(h);
    }
    const bool dead = !is_our_child_alive();
#else
    ::kill(static_cast<pid_t>(pid), SIGTERM);
    for (int i = 0; i < 20; ++i) {
      if (!is_our_child_alive()) {
        write_pid_file(cfg_, 0);  // 清 pidfile
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (i == 9) ::kill(static_cast<pid_t>(pid), SIGKILL);  // 1s 后升级
    }
    const bool dead = !is_our_child_alive();
#endif
    if (dead) write_pid_file(cfg_, 0);
    return dead;
  }
  return probe(400) ? false : true;  // 非我们拉起的：只报状态不强杀
}

void ServiceManager::start_watch() {
  if (watching_.exchange(true)) return;
  watch_thread_ = std::thread([this]() {
    while (watching_.load()) {
      if (!probe(1500)) {
        // 不健康→尝试拉起（限频：60s 内不重复拉）
        static thread_local auto last_pull = std::chrono::steady_clock::now() - std::chrono::seconds(60);
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_pull).count() >= 60) {
          last_pull = now;
          start();
        }
      }
      for (int i = 0; i < 50 && watching_.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 5s 周期
    }
  });
}

void ServiceManager::stop_watch() {
  watching_.store(false);
  if (watch_thread_.joinable()) watch_thread_.join();
}

}  // namespace zing
