// unit_log_event：LogEvent 结构化日志测试（v0.50.7）
// 验证核心不变量：
//   1. 输出格式：[HH:MM:SS.mmm] [LEVEL] [component] message {json}
//   2. 无字段时不输出 {} 段
//   3. 字段值 JSON 转义（引号/反斜杠/中文）
//   4. 走 std::cerr C++ 流——rdbuf 重定向可捕获（fwrite(stderr) 会绕过，回归防线）
//   5. 时间戳格式合法（正则 [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}）

#include <iostream>
#include <sstream>
#include <string>
#include <streambuf>

#include "thin_agent/log/LogEvent.h"

#include "test_macros.h"

int main() {
  using namespace thin_agent;

  // ── 1. rdbuf 重定向捕获（核心：必须走 C++ cerr 流）──
  {
    std::ostringstream captured;
    auto* old = std::cerr.rdbuf(captured.rdbuf());
    log_event("fc-loop", LogLevel::Info, "iter start",
              {{"iter", 2}, {"tool_calls", 3}});
    std::cerr.rdbuf(old);  // 恢复

    std::string out = captured.str();
    ASSERT_TRUE("captured non-empty (goes through cerr stream)", !out.empty());
    ASSERT_TRUE("has [INFO]", out.find("[INFO]") != std::string::npos);
    ASSERT_TRUE("has [fc-loop]", out.find("[fc-loop]") != std::string::npos);
    ASSERT_TRUE("has message", out.find("iter start") != std::string::npos);
    ASSERT_TRUE("has json fields",
                out.find("{\"iter\":2,\"tool_calls\":3}") != std::string::npos);
    ASSERT_TRUE("ends with newline", !out.empty() && out.back() == '\n');
  }

  // ── 2. 时间戳格式 ──
  {
    std::ostringstream captured;
    auto* old = std::cerr.rdbuf(captured.rdbuf());
    log_event("t", LogLevel::Debug, "m");
    std::cerr.rdbuf(old);
    // 找 [xx:xx:xx.xxx]
    auto p = captured.str().find('[');
    ASSERT_TRUE("timestamp present at line start", p == 0);
    std::string ts = captured.str().substr(1, 12);
    bool ok = ts.size() == 12 && ts[2] == ':' && ts[5] == ':' && ts[8] == '.';
    for (char c : ts) {
      if (c != ':' && c != '.') ok = ok && (c >= '0' && c <= '9');
    }
    ASSERT_TRUE("timestamp format HH:MM:SS.mmm", ok);
  }

  // ── 3. 级别名 ──
  {
    for (auto [lv, name] : std::initializer_list<std::pair<LogLevel, const char*>>{
             {LogLevel::Debug, "DEBUG"}, {LogLevel::Info, "INFO"},
             {LogLevel::Warn, "WARN"}, {LogLevel::Error, "ERROR"}}) {
      std::ostringstream captured;
      auto* old = std::cerr.rdbuf(captured.rdbuf());
      log_event("t", lv, "m");
      std::cerr.rdbuf(old);
      ASSERT_TRUE(std::string("level ") + name,
                  captured.str().find(name) != std::string::npos);
    }
  }

  // ── 4. 无字段：不输出 {} 段 ──
  {
    std::ostringstream captured;
    auto* old = std::cerr.rdbuf(captured.rdbuf());
    log_event("t", LogLevel::Warn, "bare message");
    std::cerr.rdbuf(old);
    ASSERT_TRUE("no json segment when fields empty",
                captured.str().find('{') == std::string::npos);
    ASSERT_TRUE("bare message present",
                captured.str().find("bare message") != std::string::npos);
  }

  // ── 5. 字段转义（引号/反斜杠/中文 UTF-8）──
  {
    std::ostringstream captured;
    auto* old = std::cerr.rdbuf(captured.rdbuf());
    log_event("t", LogLevel::Error, "escaped",
              {{"path", std::string("a\"b\\c")}, {"zh", std::string("你好")}});
    std::cerr.rdbuf(old);
    ASSERT_TRUE("quote escaped",
                captured.str().find("\\\"") != std::string::npos);
    ASSERT_TRUE("backslash escaped",
                captured.str().find("\\\\") != std::string::npos);
    ASSERT_TRUE("utf-8 chinese intact",
                captured.str().find("你好") != std::string::npos);
    ASSERT_TRUE("valid json tail",
                captured.str().find("{\"path\":\"a\\\"b\\\\c\",\"zh\":\"你好\"}")
                    != std::string::npos);
  }

  return TEST_REPORT();
}
