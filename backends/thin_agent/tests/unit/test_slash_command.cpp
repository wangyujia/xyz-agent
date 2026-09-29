// v0.53.21: 斜杠命令单测——词表命中/别名归一/词表外穿透/带参数/默认表
#//（AgentService 侧会话态操作由 e2e 真网验证）
#include <cstdio>
#include <string>
#include "thin_agent/core/slash_commands.h"

using namespace thin_agent;

int fails = 0;
void check(bool ok, const char* name) {
  if (!ok) { std::printf("FAIL: %s\n", name); ++fails; }
  else std::printf("PASS: %s\n", name);
}

int main() {
  const auto& a = slash_alias_defaults();
  // 1) 规范命令
  check(slash_parse("/new", a) == "/new", "/new 命中");
  check(slash_parse("/reset", a) == "/reset", "/reset 命中");
  check(slash_parse("/help", a) == "/help", "/help 命中");
  check(slash_parse("/status", a) == "/status", "/status 命中");
  // 2) 别名归一
  check(slash_parse("/cls", a) == "/clear", "/cls → /clear");
  check(slash_parse("/start", a) == "/new", "/start → /new");
  // 3) 带参数
  check(slash_parse("/new 任务：写编译器", a) == "/new", "带参数命中首词");
  // 4) 穿透（词表外/非斜杠/空）
  check(slash_parse("/nosuch", a).empty(), "词表外穿透");
  check(slash_parse("普通文本", a).empty(), "非斜杠穿透");
  check(slash_parse("", a).empty(), "空文本穿透");
  check(slash_parse("/ne", a).empty(), "前缀不完整穿透（/ne≠/new）");
  // 4b) B 档命令
  check(slash_parse("/stop", a) == "/stop", "/stop 命中");
  check(slash_parse("/halt", a) == "/stop", "/halt → /stop 别名");
  check(slash_parse("/model", a) == "/model", "/model 命中");
  check(slash_parse("/models", a) == "/model", "/models → /model 别名");
  check(slash_parse("/tools", a) == "/tools", "/tools 命中");
  // 4c) C 档命令
  check(slash_parse("/compact", a) == "/compact", "/compact 命中");
  check(slash_parse("/retry", a) == "/retry", "/retry 命中");
  check(slash_parse("/sessions", a) == "/sessions", "/sessions 命中");
  check(slash_parse("/switch", a).empty(), "/switch 不设词表（换 chat_id 即切）");
  // 5) 自定义表（配置驱动）
  std::map<std::string, std::string> custom = {{"/go", "/new"}};
  check(slash_parse("/go", custom) == "/new", "自定义别名表生效");
  check(slash_parse("/new", custom).empty(), "自定义表不含默认项（覆盖语义）");
  std::printf(fails ? "%d FAILED\n" : "ALL PASS\n", fails);
  return fails ? 1 : 0;
}
