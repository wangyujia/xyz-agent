#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {

/// 主动监控器：检测文件变化 / 进程状态 / HTTP 端点，触发自主行动。
///
/// 监控源:
///   - FileWatcher: inotify 监听文件变化（Linux）
///   - HttpPoller: 定时 GET 端点检测
///
/// 用法:
///   ProactiveMonitor mon;
///   mon.watch_file("/var/log/build.log");
///   mon.on_alert([](const std::string& msg) { ... });
///   mon.start(5000);  // 每 5 秒轮询
class ProactiveMonitor {
 public:
  using AlertCallback = std::function<void(const nlohmann::json& alert)>;

  ProactiveMonitor();
  ~ProactiveMonitor();

  ProactiveMonitor(const ProactiveMonitor&) = delete;
  ProactiveMonitor& operator=(const ProactiveMonitor&) = delete;

  /// 监控文件变化。
  void watch_file(const std::string& path,
                  const std::vector<std::string>& error_patterns = {});

  /// 监控 HTTP 端点。
  void watch_http(const std::string& url,
                  const std::string& name = "",
                  int timeout_sec = 10);

  /// 设置告警回调。
  void on_alert(AlertCallback cb) { alert_cb_ = std::move(cb); }

  /// 启动监控线程。
  void start(int interval_ms = 5000);

  /// 停止监控线程。
  void stop();

  /// 手动轮询一次（测试用）。
  nlohmann::json poll_once();

  /// 状态查询。
  nlohmann::json status() const;

 private:
  void poll_loop();
  nlohmann::json check_file(const std::string& path,
                             const std::vector<std::string>& patterns);
  nlohmann::json check_http(const std::string& url,
                             const std::string& name,
                             int timeout_sec);

  struct FileWatch {
    std::string path;
    std::vector<std::string> error_patterns;
    int64_t last_size{0};
    int64_t last_mtime{0};
  };

  struct HttpWatch {
    std::string url;
    std::string name;
    int timeout_sec{10};
  };

  std::vector<FileWatch> file_watches_;
  std::vector<HttpWatch> http_watches_;
  AlertCallback alert_cb_;
  std::unique_ptr<std::thread> poll_thread_;
  std::atomic<bool> running_{false};
  int interval_ms_{5000};
  mutable std::mutex mu_;
};

}  // namespace thin_agent
