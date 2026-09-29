// zing_agent P2: 本机 thin_agent 服务代管
// 用户感知不到服务——客户端负责探活/拉起/停止/守护。
// 纯 C++ 无 GUI 依赖：CLI（--svcctl）与 webview bind 共用。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace zing {

struct ServiceConfig {
  std::string binary;      // thin_agent 可执行文件
  std::string config_yaml; // --config
  std::string profile;     // --profile（可空）
  int port = 8765;
  std::string env_file;    // 环境变量注入（API key 等，key=value 行）
  std::string log_path;    // 服务日志重定向
  std::string pid_file;    // PID 落盘（CLI 跨进程共享亲缘关系）
  bool autostart_watch = true;  // 守护线程开关
};

class ServiceManager {
 public:
  explicit ServiceManager(ServiceConfig cfg) : cfg_(std::move(cfg)) {}

  ~ServiceManager() { stop_watch(); }

  // 探活：TCP 连 port + HTTP GET /stats（收到 200 即健康）
  bool probe(int timeout_ms = 1500) const;

  // 状态快照（JSON 字符串——bind/CLI 共用）
  // {"running":bool,"healthy":bool,"pid":int,"port":int,"autostart":bool}
  std::string status_json() const;

  // 拉起服务（已健康则跳过）。成功=true。
  bool start();

  // 停止服务（SIGTERM→SIGKILL 兜底）
  bool stop();

  // 守护线程：每 5s 探活，挂了自动拉起（托管模式核心）
  void start_watch();
  void stop_watch();

  const ServiceConfig& config() const { return cfg_; }

 private:
#ifdef _WIN32
  bool probe_http(SOCKET fd) const;
#else
  bool probe_http(int fd) const;
#endif
  ServiceConfig cfg_;
  std::atomic<long> pid_{-1};
  std::atomic<bool> watching_{false};
  std::thread watch_thread_;
  mutable std::atomic<bool> last_healthy_{false};

  bool is_our_child_alive() const;
  std::map<std::string, std::string> parse_env_file() const;
};

}  // namespace zing
