// unit_skill_dispatch_exception：技能/插件调用的异常隔离（v0.53.90）
//
// dispatch_cpp 是**全仓所有技能/插件调用的唯一咽喉点**（AgentServiceUtil/Tools/Ws、
// McpServer 都经它）。修复前它直接 `return it->second(params)`：handler 抛出的异常
// 原样穿出——内置 handler 的 nlohmann/std::filesystem 误用、以及**跨 .so 边界的插件
// 异常**都会穿到 agent loop / WS worker（调用点无兜底时 std::terminate = 服务崩溃），
// 且失败零可观测。
//
// 本测试锁住"崩溃降级为结构化失败"的契约，并守住**成功路径零改动**：
//   T1 std::exception（含 what）→ success=false + error 前缀 handler_exception + 带 action
//   T2 非 std 异常（throw 42）→ 同样降级（不能漏网）
//   T3 正常 handler 原样返回（回归守卫：包装不能改语义）
//   T4 未注册 action → 仍返回 no_cpp_handler（错误语义不被覆盖）
//   T5 连续多次异常后注册表仍可用（异常不腐蚀状态）
#include <iostream>
#include <stdexcept>
#include <string>

#include "test_macros.h"
#include "thin_agent/core/SkillRegistry.h"

using namespace thin_agent;

int main() {
  SkillRegistry reg;
  reg.register_cpp_handler("boom_std", [](const nlohmann::json&) -> nlohmann::json {
    throw std::runtime_error("device exploded");
  });
  reg.register_cpp_handler("boom_unknown", [](const nlohmann::json&) -> nlohmann::json {
    throw 42;  // 非 std::exception
  });
  reg.register_cpp_handler("fine", [](const nlohmann::json& p) -> nlohmann::json {
    return {{"success", true}, {"output", "ok"}, {"echo", p.value("x", 0)}};
  });

  // T1
  auto r1 = reg.dispatch_cpp("boom_std", nlohmann::json::object());
  ASSERT_TRUE("T1 异常被隔离（success=false）", r1.value("success", true) == false);
  const std::string e1 = r1.value("error", "");
  ASSERT_TRUE("T1 error 前缀 handler_exception", e1.rfind("handler_exception:", 0) == 0);
  ASSERT_TRUE("T1 保留 what() 信息（可诊断）", e1.find("device exploded") != std::string::npos);
  ASSERT_EQ("T1 带 action 字段（定位到哪个技能）", r1.value("action", ""), std::string("boom_std"));

  // T2
  auto r2 = reg.dispatch_cpp("boom_unknown", nlohmann::json::object());
  ASSERT_TRUE("T2 非 std 异常同样被隔离", r2.value("success", true) == false);
  ASSERT_EQ("T2 unknown 标记", r2.value("error", ""), std::string("handler_exception:unknown"));

  // T3
  nlohmann::json p = {{"x", 7}};
  auto r3 = reg.dispatch_cpp("fine", p);
  ASSERT_TRUE("T3 正常 handler 仍 success", r3.value("success", false));
  ASSERT_EQ("T3 输出原样透传", r3.value("output", ""), std::string("ok"));
  ASSERT_EQ("T3 参数原样透传", r3.value("echo", 0), 7);

  // T4
  auto r4 = reg.dispatch_cpp("nope", nlohmann::json::object());
  ASSERT_EQ("T4 未注册语义不变", r4.value("error", ""), std::string("no_cpp_handler:nope"));

  // T5
  auto r5 = reg.dispatch_cpp("boom_std", nlohmann::json::object());
  auto r6 = reg.dispatch_cpp("fine", p);
  ASSERT_TRUE("T5 反复异常后注册表仍可用", r5.value("success", true) == false &&
                                              r6.value("success", false));

  return TEST_REPORT();
}
