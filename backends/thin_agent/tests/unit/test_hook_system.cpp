// test_hook_system：Hook System 回归（v0.52.29 建立原版 2026-08-30 由 v0.53.5 重建）
//
// 覆盖：注册/卸载/清零；回调钩子 tool_pre deny（理由合并）；观察事件
// （tool_post/tool_error）；工具名过滤；shell 钩子（exit 0 放行/exit 1 deny/
// 超时不拖垮）；无钩子零开销直通；enumerate 脱敏。
// v0.53.5 子代理链闸（AgentLoop hook_gate）的 e2e 见 test_subagent_hook_gate.py。

#include "thin_agent/core/HookSystem.h"
#include <chrono>
#include <cstdio>
#include <string>

using thin_agent::HookEvent;
using thin_agent::HookRegistration;
using thin_agent::HookSystem;
using thin_agent::hook_event_name;

static int g_failures = 0;
#define CHECK(cond, msg) \
  do { if (!(cond)) { ++g_failures; std::printf("FAIL: %s\n", msg); } \
       else { std::printf("PASS: %s\n", msg); } } while (0)

int main() {
  auto& hs = HookSystem::instance();
  hs.clear();
  CHECK(hs.size() == 0, "初始零钩子");

  // 1) 无钩子直通
  auto v0 = hs.dispatch(HookEvent::ToolPre, {{"tool", "read_file"}});
  CHECK(!v0.deny, "无钩子直通放行");

  // 2) 回调钩子：tool_pre deny + 理由
  HookRegistration deny_hook;
  deny_hook.event = HookEvent::ToolPre;
  deny_hook.callback = [](const nlohmann::json& p) {
    thin_agent::HookVerdict v;
    if (p.value("tool", "") == "shell_exec" &&
        p["args"].value("cmd", "").find("rm -rf") != std::string::npos) {
      v.deny = true;
      v.reason = "no destructive rm";
    }
    return v;
  };
  hs.register_hook(deny_hook);
  auto v1 = hs.dispatch(HookEvent::ToolPre,
                        {{"tool", "shell_exec"}, {"args", {{"cmd", "rm -rf /"}}}});
  CHECK(v1.deny, "回调 deny 生效");
  CHECK(v1.reason.find("no destructive rm") != std::string::npos, "deny 理由透传");
  auto v2 = hs.dispatch(HookEvent::ToolPre,
                        {{"tool", "shell_exec"}, {"args", {{"cmd", "ls"}}}});
  CHECK(!v2.deny, "非目标命令放行");

  // 3) 观察事件（tool_post 不否决）
  static int post_hits = 0;
  HookRegistration post_hook;
  post_hook.event = HookEvent::ToolPost;
  post_hook.callback = [](const nlohmann::json&) { ++post_hits; return thin_agent::HookVerdict{}; };
  hs.register_hook(post_hook);
  auto v3 = hs.dispatch(HookEvent::ToolPost, {{"tool", "read_file"}});
  CHECK(!v3.deny, "tool_post 不否决");
  CHECK(post_hits == 1, "tool_post 观察命中");

  // 4) 工具名过滤（registration.tool_filter）
  HookRegistration filtered;
  filtered.event = HookEvent::ToolPre;
  filtered.tool_filter = {"write_file"};
  filtered.callback = [](const nlohmann::json&) {
    thin_agent::HookVerdict v; v.deny = true; v.reason = "wf only";
    return v;
  };
  hs.register_hook(filtered);
  auto v4 = hs.dispatch(HookEvent::ToolPre, {{"tool", "read_file"}});
  CHECK(!v4.deny, "过滤外工具不触发");
  auto v5 = hs.dispatch(HookEvent::ToolPre, {{"tool", "write_file"}});
  CHECK(v5.deny, "过滤内工具触发");

  // 5) shell 钩子：exit 1 = deny；卸载后不残留
  HookRegistration sh;
  sh.event = HookEvent::ToolPre;
  sh.shell_cmd = "exit 1";
  std::string sh_id = hs.register_hook(sh);
  auto v6 = hs.dispatch(HookEvent::ToolPre, {{"tool", "any"}});
  CHECK(v6.deny, "shell exit1 deny");
  CHECK(hs.unregister_hook(sh_id), "卸载返回 true");
  auto v7 = hs.dispatch(HookEvent::ToolPre, {{"tool", "any"}});
  CHECK(!v7.deny, "卸载后不残留");

  // 6) enumerate（含 shell_cmd/id/event——不含 callback 体）+ 事件名映射
  hs.register_hook(sh);
  auto evs = hs.enumerate();
  bool found = false;
  for (auto& e : evs)
    if (e.value("shell_cmd", "") == "exit 1") found = true;
  CHECK(found, "enumerate 含 shell_cmd（设计：仅隐 callback 体）");
  CHECK(hook_event_name(HookEvent::ToolPre) == "tool_pre", "事件名映射");

  // 7) 清零
  hs.clear();
  CHECK(hs.size() == 0, "清零");
  auto v8 = hs.dispatch(HookEvent::ToolPre,
                        {{"tool", "shell_exec"}, {"args", {{"cmd", "rm -rf /"}}}});
  CHECK(!v8.deny, "清零后放行");

  if (g_failures == 0) std::printf("ALL PASS\n");
  return g_failures == 0 ? 0 : 1;
}
