// test_dynamic_bridge：v0.52.19 桥接可重入（动态同步）回归
//
// 背景：v0.52.14 桥接是构造期一次性的——MCP 晚连/运行中热注册的
// 工具永远进不了子代理工具区（ToolRegistry）。改为可重入增量同步
// sync_skill_to_tool_registry()：注册源变化后调一次即增量进表。
//
// 验证（复刻同步函数逻辑——与 AgentService::sync_skill_to_tool_registry 同式）：
// 1. 首次同步：skill 注册的 action 进 ToolRegistry
// 2. 幂等：重复同步不产生重复/不覆盖真实注册
// 3. 增量：同步后新注册的 action，再同步一次即进表（动态桥接核心）
// 4. 真实注册保护：ToolRegistry 已有同名时 skill 版本不覆盖
#include "test_macros.h"

#include "thin_agent/agent/ToolRegistry.h"
#include "thin_agent/core/SkillRegistry.h"

using namespace thin_agent;

// 与生产同式的同步逻辑（独立 SkillRegistry 实例模拟热注册）
static int sync_once(SkillRegistry& skill_reg) {
  auto schemas = skill_reg.build_tools_schema();
  int bridged = 0;
  for (const auto& t : schemas) {
    if (!t.contains("function")) continue;
    const auto& fn = t["function"];
    std::string name = fn.value("name", "");
    if (name.empty()) continue;
    if (agent::ToolRegistry::instance().find(name) != nullptr) continue;
    agent::ToolSchema ts;
    ts.name = name;
    ts.description = fn.value("description", name);
    ts.execute = [&skill_reg, name](const nlohmann::json& p) -> nlohmann::json {
      auto raw = skill_reg.dispatch_cpp(name, p);
      nlohmann::json out;
      out["ok"] = raw.value("ok", raw.value("success", false));
      nlohmann::json rest = nlohmann::json::object();
      for (auto it = raw.begin(); it != raw.end(); ++it) {
        if (it.key() == "ok" || it.key() == "success" || it.key() == "error") continue;
        rest[it.key()] = it.value();
      }
      out["result"] = std::move(rest);
      out["error"] = raw.value("error", "");
      return out;
    };
    agent::ToolRegistry::instance().register_tool(std::move(ts));
    ++bridged;
  }
  return bridged;
}

int main() {
  SkillRegistry skill_reg;
  auto& tr = agent::ToolRegistry::instance();

  // 预置真实注册（保护验证用）
  {
    agent::ToolSchema s;
    s.name = "spawn_agent";
    s.description = "real";
    s.execute = [](const nlohmann::json&) { return nlohmann::json{{"ok", true}}; };
    tr.register_tool(std::move(s));
  }

  // 1) 首次同步
  skill_reg.register_cpp_handler("mcp_late_tool",
      [](const nlohmann::json& p) { return nlohmann::json{{"success", true}, {"v", p.value("v", 0)}}; });
  int n1 = sync_once(skill_reg);
  ASSERT_TRUE("首次同步注册", n1 >= 1 && tr.find("mcp_late_tool") != nullptr);

  // 2) 幂等
  int n2 = sync_once(skill_reg);
  ASSERT_TRUE("重复同步零新增", n2 == 0);

  // 3) 动态增量（核心：晚注册后再同步即进表）
  skill_reg.register_cpp_handler("mcp_hot_tool2",
      [](const nlohmann::json&) { return nlohmann::json{{"success", true}}; });
  int n3 = sync_once(skill_reg);
  ASSERT_TRUE("晚注册工具增量进表", n3 == 1 && tr.find("mcp_hot_tool2") != nullptr);
  // 且可执行
  auto r = tr.call("mcp_hot_tool2", {}, 3000);
  ASSERT_TRUE("晚注册工具可执行", r.ok);

  // 4) 真实注册保护
  const auto* sa = tr.find("spawn_agent");
  ASSERT_TRUE("真实注册不被覆盖", sa && sa->description == "real");

  tr.clear();
  return TEST_REPORT();
}
