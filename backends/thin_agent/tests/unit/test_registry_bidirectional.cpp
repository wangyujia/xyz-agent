// test_registry_bidirectional：v0.52.24 注册表双向同步回归（#9 方案 B）
//
// 背景：两套注册表（SkillRegistry=主 FC 循环 / ToolRegistry=子代理）
// 经 v0.52.14/19 桥接（Skill→Tool）单向缝合。#9 方案 B 收敛为双向
// 互补：ToolRegistry 独有注册（如 delegate_task）回流 SkillRegistry，
// 主循环经 dispatch_cpp 也能调用。
//
// 验证（复刻双向同步语义——与 sync_skill_to_tool_registry 同式）：
// 1. 正向：skill 注册的工具经桥接进 ToolRegistry（已有语义回归）
// 2. 反向：ToolRegistry 独有工具回流 SkillRegistry（dispatch 可达）
// 3. 反向字段适配：{ok,result}→{success,...平铺}
// 4. 幂等：重复双向同步零新增
// 5. 双向一致性：一轮同步后两表名集互通（同名在两表皆可达）
#include "test_macros.h"

#include "thin_agent/agent/ToolRegistry.h"
#include "thin_agent/core/SkillRegistry.h"

using namespace thin_agent;

int main() {
  SkillRegistry skill_reg;
  auto& tr = agent::ToolRegistry::instance();

  // ToolRegistry 独有工具（模拟 delegate_task）
  {
    agent::ToolSchema s;
    s.name = "delegate_task";
    s.description = "tool-only registration";
    s.execute = [](const nlohmann::json& p) -> nlohmann::json {
      return {{"ok", true}, {"result", {{"answer", p.value("q", "?") + "-done"}}}};
    };
    tr.register_tool(std::move(s));
  }

  // 1) 正向：skill 注册 → 桥接进 Tool
  skill_reg.register_cpp_handler("skill_side_tool",
      [](const nlohmann::json&) { return nlohmann::json{{"success", true}}; });
  {
    auto schemas = skill_reg.build_tools_schema();
    for (const auto& t : schemas) {
      std::string name = t.value("function", nlohmann::json::object())
                             .value("name", "");
      if (name.empty() || tr.find(name) != nullptr) continue;
      agent::ToolSchema ts;
      ts.name = name;
      ts.execute = [&skill_reg, name](const nlohmann::json& p) -> nlohmann::json {
        auto raw = skill_reg.dispatch_cpp(name, p);
        return {{"ok", raw.value("ok", raw.value("success", false))}};
      };
      tr.register_tool(std::move(ts));
    }
  }
  ASSERT_TRUE("正向：skill 工具进 ToolRegistry",
              tr.find("skill_side_tool") != nullptr);

  // 2) 反向：Tool 独有 → 回流 Skill
  int backflowed = 0;
  for (const auto& t : tr.all_tools()) {
    if (skill_reg.find_action(t.name) != nullptr) continue;
    skill_reg.register_cpp_handler(t.name,
        [&tr, name = t.name](const nlohmann::json& p) -> nlohmann::json {
          auto raw = tr.call(name, p, 0);
          nlohmann::json out;
          out["success"] = raw.ok;
          if (raw.result.is_object() && !raw.result.empty()) {
            for (auto it = raw.result.begin(); it != raw.result.end(); ++it)
              out[it.key()] = it.value();
          }
          return out;
        });
    ++backflowed;
  }
  ASSERT_TRUE("反向：Tool 独有工具回流 Skill", backflowed >= 1 &&
              skill_reg.find_action("delegate_task") != nullptr);

  // 3) 反向字段适配
  {
    auto r = skill_reg.dispatch_cpp("delegate_task", {{"q", "hello"}});
    ASSERT_TRUE("success 字段适配", r.value("success", false));
    ASSERT_TRUE("result 平铺", r.value("answer", "") == "hello-done");
  }

  // 4) 幂等（重复双向零新增）
  {
    int again = 0;
    for (const auto& t : tr.all_tools()) {
      if (skill_reg.find_action(t.name) != nullptr) continue;
      ++again;
    }
    ASSERT_TRUE("重复同步零新增", again == 0);
  }

  // 5) 双向一致：两表对同名皆可达
  {
    bool ok_skill = skill_reg.find_action("skill_side_tool") != nullptr;
    bool ok_tool = tr.find("delegate_task") != nullptr;
    ASSERT_TRUE("双向一致性：同名两表皆可达", ok_skill && ok_tool);
  }

  tr.clear();
  return TEST_REPORT();
}
