#pragma once
// LogEvent — 轻量结构化日志（v0.50.7）
//
// 设计目标（嵌入式约束：零外部依赖、~100 行、不加线程开销）：
//   - 输出格式：[HH:MM:SS.mmm] [LEVEL] [component] message {"k":v,...}
//     （人类可读前缀 + JSON 尾段：tail 直接看，jq/Loki 机器可解析）
//   - 写入 std::cerr 流（cerr.write）—— 必须走 C++ ostream 而非 fwrite(stderr)：
//     ws_agent_main 用 std::cerr.rdbuf(&log_buf) 重定向到 RotatingLogBuf，
//     C 级 stderr FILE* 会绕过 rdbuf 直接打到终端。单次 write 保证行不撕裂。
//   - 字段值自动 JSON 转义（nlohmann dump 自带 UTF-8/控制字符处理）
//
// 用法：
//   #include "thin_agent/log/LogEvent.h"
//   thin_agent::log_event("fc-loop", thin_agent::LogLevel::Info,
//       "iter start", {{"iter", iter}, {"tool_calls", n}});
//   // [12:34:56.789] [INFO] [fc-loop] iter start {"iter":2,"tool_calls":3}
//
// 渐进迁移：v0.50.7 只替换高频路径（fc-loop/fc-tool 族），
// 其余 cerr 调用点保持原样——两种格式并存，均正常落盘。

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace thin_agent {

enum class LogLevel : int {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3,
};

namespace log_detail {

inline const char* level_name(LogLevel lv) {
  switch (lv) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
  }
  return "INFO";
}

/// 毫秒时间戳 [HH:MM:SS.mmm]（本地时间，与 RotatingLogBuf 同款格式）
inline std::string timestamp_now() {
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()).count() % 1000;
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf{};
  // v0.54.16: `localtime_r` 是 POSIX 名，**MSVC 没有**（只有 `localtime_s`，且参数顺序相反）。
  // 该头被全仓大量 TU 包含 ⇒ 此前 Windows 侧**整仓都编不过**（交叉编译实测：
  // error: 'localtime_r' was not declared in this scope; did you mean 'localtime_s'?）。
#ifdef _WIN32
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                static_cast<int>(ms));
  return buf;
}

}  // namespace log_detail

/// 结构化日志事件：一次性构造完整行，单次 cerr.write 输出。
/// @param component 组件标签（如 "fc-loop" / "fc-tool" / "cron"）
/// @param level     级别
/// @param message   人类可读消息（自由文本，不参与 JSON 转义）
/// @param fields    结构化字段（nlohmann json object；空/非对象时不输出 {} 段）
inline void log_event(std::string_view component, LogLevel level,
                      std::string_view message,
                      const nlohmann::json& fields = nlohmann::json()) {
  std::string line;
  line.reserve(96 + message.size());
  line += '[';
  line += log_detail::timestamp_now();
  line += "] [";
  line += log_detail::level_name(level);
  line += "] [";
  line += component;
  line += "] ";
  line += message;
  if (fields.is_object() && !fields.empty()) {
    line += ' ';
    // v0.53.2: replace 错误处理——字段值含非法 UTF-8（截断切半等）时
    // 以 U+FFFD 替代而非抛异常打崩服务（生产实证 patrol 链路崩溃）
    line += fields.dump(-1, ' ', false,
                        nlohmann::detail::error_handler_t::replace);
  }
  line += '\n';
  std::cerr.write(line.data(), static_cast<std::streamsize>(line.size()));
}

}  // namespace thin_agent
