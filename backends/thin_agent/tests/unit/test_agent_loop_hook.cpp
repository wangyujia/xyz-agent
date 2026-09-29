// test_agent_loop_hook：v0.53.5 子代理工具钩子闸（AgentLoop::hook_gate）
//
// 背景：主链 execute_one_tool 自 v0.52.29 过 tool_pre；子代理链
// （AgentLoop→registry_.call）此前绕过——deny 对子代理失效。
// 本测直证闸逻辑：deny→error 含理由；放行→error 空；工具过滤精准。

#include "thin_agent/agent/AgentLoop.h"
#include "thin_agent/core/HookSystem.h"
#include <cstdio>

using thin_agent::HookEvent;
using thin_agent::HookRegistration;
using thin_agent::HookSystem;

static int g_failures = 0;
#define CHECK(cond, msg) \
  do { if (!(cond)) { ++g_failures; std::printf("FAIL: %s\n", msg); } \
       else { std::printf("PASS: %s\n", msg); } } while (0)

int main() {
  auto& hs = HookSystem::instance();
  hs.clear();

  thin_agent::DemoConfigCompat cfg;
  thin_agent::agent::AgentLoopConfig lc;
  thin_agent::agent::AgentLoop loop(cfg, "", thin_agent::agent::ToolRegistry::instance(), lc);

  // 1) 无钩子：放行
  auto p1 = loop.hook_gate("read_file", {{"path", "/tmp"}});
  CHECK(p1.error.empty(), "子代理闸:无钩子放行");

  // 2) 条件 deny：只拒 read_file
  HookRegistration deny;
  deny.event = HookEvent::ToolPre;
  deny.tool_filter = {"read_file"};
  deny.callback = [](const nlohmann::json&) {
    thin_agent::HookVerdict v;
    v.deny = true;
    v.reason = "subagent gate unit";
    return v;
  };
  hs.register_hook(deny);

  auto p2 = loop.hook_gate("read_file", {{"path", "/etc/hostname"}});
  CHECK(!p2.error.empty() &&
        p2.error.find("subagent gate unit") != std::string::npos,
        "子代理闸:deny拦截+理由透传");
  CHECK(p2.error.find("tool_pre deny") != std::string::npos,
        "子代理闸:deny语义标记");

  auto p3 = loop.hook_gate("search_files", {{"pattern", "x"}});
  CHECK(p3.error.empty(), "子代理闸:过滤外工具放行");

  hs.clear();
  auto p4 = loop.hook_gate("read_file", {{"path", "/tmp"}});
  CHECK(p4.error.empty(), "子代理闸:清零后放行");

  if (g_failures == 0) std::printf("ALL PASS\n");
  return g_failures == 0 ? 0 : 1;
}
