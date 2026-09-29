// test_skill_tool_bridge：v0.52.14 SkillRegistry→ToolRegistry 桥接回归
//
// 背景：AgentLoop（spawn 子代理）只从 ToolRegistry 取工具，而插件/
// MCP/内置 action 全部注册在 SkillRegistry——子代理工具声明区块只有
// spawn_agent，永远无法写文件（真 e2e 零交付的根本原因）。
//
// 验证：
// 1. SkillRegistry 注册 cpp_handler 后，桥接函数在 ToolRegistry 生成
//    同名工具（schema：名称/描述/参数 required 正确重建）
// 2. 桥接工具可执行（dispatch_cpp 转发生效）
// 3. ToolRegistry 已有同名工具时不覆盖（spawn_agent 保护）
#include "test_macros.h"

#include "thin_agent/agent/ToolRegistry.h"
#include "thin_agent/core/SkillRegistry.h"

using namespace thin_agent;

// 复刻 AgentService 桥接逻辑（与 AgentService.cpp v0.52.14 同式）
static int bridge(SkillRegistry& skill_reg) {
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
    if (fn.contains("parameters") && fn["parameters"].is_object()) {
      const auto& params = fn["parameters"];
      if (params.contains("properties") && params["properties"].is_object()) {
        std::unordered_set<std::string> required;
        if (params.contains("required") && params["required"].is_array()) {
          for (const auto& rq : params["required"])
            if (rq.is_string()) required.insert(rq.get<std::string>());
        }
        for (auto pit = params["properties"].begin();
             pit != params["properties"].end(); ++pit) {
          agent::ToolParameter tp;
          tp.name = pit.key();
          tp.type = pit.value().value("type", "string");
          tp.description = pit.value().value("description", "");
          tp.required = required.count(pit.key()) > 0;
          ts.parameters.push_back(tp);
        }
      }
    }
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
  auto& tool_reg = agent::ToolRegistry::instance();

  // 前置：预注册 spawn_agent（模拟已存在，验证不覆盖）
  {
    agent::ToolSchema s;
    s.name = "spawn_agent";
    s.description = "pre-existing";
    s.execute = [](const nlohmann::json&) {
      return nlohmann::json{{"ok", true}, {"pre", true}};
    };
    tool_reg.register_tool(std::move(s));
  }

  // 1) 注册 skill action + 桥接
  skill_reg.register_cpp_handler(
      "code_write_file",
      [](const nlohmann::json& p) {
        return nlohmann::json{{"success", true},
                              {"path", p.value("path", "")}};
      });
  int n = bridge(skill_reg);
  ASSERT_TRUE("桥接生成工具", n >= 1);

  const auto* ts = tool_reg.find("code_write_file");
  ASSERT_TRUE("ToolRegistry 可查到桥接工具", ts != nullptr);

  // 2) 桥接工具执行（dispatch 转发）
  auto r = tool_reg.call("code_write_file", {{"path", "/tmp/x.h"}}, 3000);
  ASSERT_TRUE("桥接执行成功", r.ok);
  ASSERT_TRUE("结果转发自 skill handler",
              r.result.value("path", "") == "/tmp/x.h");

  // 3) 已存在不覆盖
  const auto* sa = tool_reg.find("spawn_agent");
  ASSERT_TRUE("spawn_agent 保持预注册版本",
              sa && sa->description == "pre-existing");

  // 清理（避免污染其他单测的全局单例）
  tool_reg.clear();
  // 重新注册 spawn_agent（保持与其他测试的隔离假设无关——clear 后各自重注册）
  {
    agent::ToolSchema s;
    s.name = "spawn_agent";
    s.execute = [](const nlohmann::json&) { return nlohmann::json{{"ok", true}}; };
    tool_reg.register_tool(std::move(s));
  }

  return TEST_REPORT();
}
