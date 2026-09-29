#include "DebugLogger.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <filesystem>
#include <sstream>

DebugLogger& DebugLogger::instance() {
  static DebugLogger logger;
  return logger;
}

DebugLogger::~DebugLogger() { close(); }

void DebugLogger::init(const std::string& log_dir) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!log_dir.empty()) {
    std::filesystem::create_directories(log_dir + "/logs");
  }
  log_path_ = log_dir + "/logs/agent_tool.log";
  bak_path_ = log_dir + "/logs/agent_tool_bak.log";

  rotate_locked();

  if (file_) {
    auto t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::fprintf(static_cast<std::FILE*>(file_), "[%s] -- DebugLogger 初始化 --\n", buf);
    std::fflush(static_cast<std::FILE*>(file_));
  }
}

void DebugLogger::write(const std::string& msg) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!file_) return;

  try {
    if (std::filesystem::exists(log_path_) &&
        static_cast<long long>(std::filesystem::file_size(log_path_)) >= kMaxSize) {
      rotate_locked();
    }
  } catch (...) {}

  if (file_) {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%H:%M:%S", std::localtime(&time_t_now));
    std::fprintf(static_cast<std::FILE*>(file_), "[%s.%03lld] %s\n",
                 ts_buf, static_cast<long long>(ms.count()), msg.c_str());
    std::fflush(static_cast<std::FILE*>(file_));
  }
}

void DebugLogger::close() {
  std::lock_guard<std::mutex> lock(mu_);
  if (file_) {
    std::fclose(static_cast<std::FILE*>(file_));
    file_ = nullptr;
  }
}

void DebugLogger::rotate_locked() {
  if (file_) {
    std::fclose(static_cast<std::FILE*>(file_));
    file_ = nullptr;
  }
  if (!bak_path_.empty() && std::filesystem::exists(bak_path_)) {
    std::filesystem::remove(bak_path_);
  }
  if (!log_path_.empty() && !bak_path_.empty() && std::filesystem::exists(log_path_)) {
    try {
      std::filesystem::rename(log_path_, bak_path_);
    } catch (...) {
      std::filesystem::remove(log_path_);
    }
  }
  file_ = std::fopen(log_path_.c_str(), "a");
}
