#include "thin_agent/core/ProactiveMonitor.h"

#include <sys/stat.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <cctype>
#include <thread>

namespace thin_agent {

ProactiveMonitor::ProactiveMonitor() {}

ProactiveMonitor::~ProactiveMonitor() { stop(); }

void ProactiveMonitor::watch_file(const std::string& path,
                                   const std::vector<std::string>& error_patterns) {
  std::lock_guard<std::mutex> lk(mu_);
  FileWatch fw;
  fw.path = path;
  fw.error_patterns = error_patterns.empty()
      ? std::vector<std::string>{"error:", "FAILED", "BUILD FAILED",
                                  "FAILURE", "fatal:", "Traceback"}
      : error_patterns;

  // 读取初始状态
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    fw.last_size = st.st_size;
    fw.last_mtime = st.st_mtime;
  }
  file_watches_.push_back(fw);
}

void ProactiveMonitor::watch_http(const std::string& url,
                                   const std::string& name,
                                   int timeout_sec) {
  std::lock_guard<std::mutex> lk(mu_);
  http_watches_.push_back({url, name.empty() ? url : name, timeout_sec});
}

void ProactiveMonitor::start(int interval_ms) {
  if (running_) return;
  interval_ms_ = interval_ms;
  running_ = true;
  poll_thread_ = std::make_unique<std::thread>(&ProactiveMonitor::poll_loop, this);
}

void ProactiveMonitor::stop() {
  running_ = false;
  if (poll_thread_ && poll_thread_->joinable()) poll_thread_->join();
  poll_thread_.reset();
}

void ProactiveMonitor::poll_loop() {
  while (running_) {
    poll_once();
    // v0.53.43: 分片睡眠响应停止——整段 sleep 会让 stop() 多等整个间隔
    for (int slept = 0; running_ && slept < interval_ms_; slept += 250) {
      std::this_thread::sleep_for(std::chrono::milliseconds(
          std::min(250, interval_ms_ - slept)));
    }
  }
}

nlohmann::json ProactiveMonitor::poll_once() {
  nlohmann::json alerts = nlohmann::json::array();

  // v0.53.70: 两段式(与 Cron v0.53.46 同款)——锁内拷贝 watches 快照,
  /// 放锁后探测。此前 check_http(网络 IO,每 watch 最多 timeout_sec)
  /// 与 check_file(读文件)全在锁内:多 watch 串行叠加=注册面/stop
  /// 全堵(Cron 同款修过此处漏网)
  std::vector<FileWatch> files;
  std::vector<HttpWatch> https;
  AlertCallback alert_cb_copy;
  {
    std::lock_guard<std::mutex> lk(mu_);
    files = file_watches_;
    https = http_watches_;
    alert_cb_copy = alert_cb_;
  }
  for (auto& fw : files) {
    auto alert = check_file(fw.path, fw.error_patterns);
    if (!alert.is_null()) {
      alerts.push_back(alert);
      if (alert_cb_copy) alert_cb_copy(alert);
    }
  }
  for (const auto& hw : https) {
    auto alert = check_http(hw.url, hw.name, hw.timeout_sec);
    if (!alert.is_null()) {
      alerts.push_back(alert);
      if (alert_cb_copy) alert_cb_copy(alert);
    }
  }

  return alerts;
}

nlohmann::json ProactiveMonitor::check_file(
    const std::string& path,
    const std::vector<std::string>& patterns) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return {};

  // 检查文件是否有变化
  for (auto& fw : file_watches_) {
    if (fw.path != path) continue;
    if (st.st_size == fw.last_size && st.st_mtime == fw.last_mtime) return {};

    // 读取新增内容
    std::ifstream file(path);
    if (!file) return {};

    int64_t new_bytes = st.st_size - fw.last_size;
    if (new_bytes <= 0 || new_bytes > 100000) {
      fw.last_size = st.st_size;
      fw.last_mtime = st.st_mtime;
      return {};
    }

    file.seekg(fw.last_size);
    std::string chunk(new_bytes, '\0');
    file.read(&chunk[0], new_bytes);
    chunk.resize(file.gcount());

    fw.last_size = st.st_size;
    fw.last_mtime = st.st_mtime;

    // 匹配错误模式
    std::string matched;
    for (const auto& pat : patterns) {
      if (chunk.find(pat) != std::string::npos) {
        if (!matched.empty()) matched += ", ";
        matched += pat;
      }
    }

    if (!matched.empty()) {
      return {{"type", "file_alert"},
              {"source", "file"},
              {"path", path},
              {"matched_patterns", matched},
              {"new_bytes", new_bytes},
              {"snippet", chunk.substr(0, 200)}};
    }
  }

  return {};
}

nlohmann::json ProactiveMonitor::check_http(
    const std::string& url,
    const std::string& name,
    int timeout_sec) {
  // v0.53.43 安全修复:URL 直接拼 shell 是命令注入面(watch URL 来自
  // patrol_config,LLM 可配)——白名单字符校验拒绝 shell 元字符;
  // curl 补 -m 总超时(--connect-timeout 只限连接,慢传输无限拖);
  // stoi 防御非数字输出(代理错误页等)。
  const auto url_safe = [](const std::string& u) {
    for (char c : u) {
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == ':' ||
            c == '/' || c == '-' || c == '_' || c == '?' || c == '&' || c == '=' ||
            c == '%' || c == '#')) {
        return false;
      }
    }
    return !u.empty();
  };
  if (!url_safe(url)) {
    return {{"type", "http_alert"},
            {"source", "http"},
            {"name", name},
            {"url", url},
            {"error", "invalid_url_rejected"}};
  }
  std::string cmd = "curl -s -o /dev/null -w '%{http_code}' --connect-timeout " +
                    std::to_string(timeout_sec) + " -m " +
                    std::to_string(timeout_sec + 5) + " " + url + " 2>/dev/null";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return {};

  char buf[32] = {};
  if (::fread(buf, 1, sizeof(buf) - 1, pipe) == 0 && ferror(pipe)) {}  // v0.53.16: 消费返回值
  pclose(pipe);

  std::string code(buf);
  if (code.empty()) {
    return {{"type", "http_alert"},
            {"source", "http"},
            {"name", name},
            {"url", url},
            {"error", "no_response"}};
  }

  int http_code = 0;
  try {
    http_code = std::stoi(code);
  } catch (...) {
    http_code = 0;  // v0.53.43: 非数字输出(代理错误页等)按不可达处理
  }
  if (http_code >= 400 || http_code == 0) {
    return {{"type", "http_alert"},
            {"source", "http"},
            {"name", name},
            {"url", url},
            {"http_code", http_code}};
  }

  return {};
}

nlohmann::json ProactiveMonitor::status() const {
  std::lock_guard<std::mutex> lk(mu_);
  nlohmann::json files = nlohmann::json::array();
  for (const auto& fw : file_watches_)
    files.push_back({{"path", fw.path}, {"last_size", fw.last_size}});
  nlohmann::json https = nlohmann::json::array();
  for (const auto& hw : http_watches_)
    https.push_back({{"url", hw.url}, {"name", hw.name}});
  return {{"running", running_.load()},
          {"interval_ms", interval_ms_},
          {"file_watches", files},
          {"http_watches", https}};
}

}  // namespace thin_agent
