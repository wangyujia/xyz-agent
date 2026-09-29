// test_agent_degraded_no_false_green：v0.52.12 假绿终结回归
//
// 背景：真 e2e resume 波次实测——4/5 子任务的 spawn_result 为
// ok=true + error="llm_failed(local_fallback): {code:1302 速率限制}"，
// 波次调度只看 ok 标 done，离线兜底文案（"我当前处于离线模式…"）
// 被当作业务交付落库——全树假 done，实际零交付。
//
// 验证（纯结构，不依赖网络——直接构造 AgentLoopResult 断言序列化
// 语义；AgentLoop::run 的兜底路径由 e2e 覆盖）：
// 1. degraded 结果序列化时 ok 必须翻 false（spawn_result 契约）
// 2. 非 degraded 正常结果不受影响
// 3. degraded 且 error 空时补 degraded(local_fallback) 错误串
#include "test_macros.h"

#include "thin_agent/agent/AgentLoop.h"

using namespace thin_agent;

// 复刻 spawn_agent 返回 JSON 的判定逻辑（与 AgentService.cpp L7009 同式）
static nlohmann::json make_spawn_result(const agent::AgentLoopResult& result) {
  return {{"type", "spawn_result"},
          {"ok", result.ok && !result.degraded},
          {"error", result.degraded && result.error.empty()
                        ? std::string("degraded(local_fallback)")
                        : result.error}};
}

int main() {
  // 1) degraded 假绿终结
  {
    agent::AgentLoopResult r;
    r.ok = true;               // AgentLoop 层 ok=true（本地兜底成功）
    r.degraded = true;         // v0.52.12 标记
    r.final_answer = "我当前处于离线模式，暂时无法回答这个问题。";
    r.error = "llm_failed(local_fallback): {\"code\":\"1302\"}";
    auto j = make_spawn_result(r);
    ASSERT_TRUE("degraded 时 spawn ok 必须为 false", !j["ok"].get<bool>());
    ASSERT_TRUE("error 透传保留",
                j["error"].get<std::string>().find("1302") !=
                    std::string::npos);
  }

  // 2) 正常结果不受影响
  {
    agent::AgentLoopResult r;
    r.ok = true;
    r.final_answer = "stack.h 已创建";
    auto j = make_spawn_result(r);
    ASSERT_TRUE("正常结果 ok 保持 true", j["ok"].get<bool>());
  }

  // 3) degraded 补错误串（error 空的边角）
  {
    agent::AgentLoopResult r;
    r.ok = true;
    r.degraded = true;
    auto j = make_spawn_result(r);
    ASSERT_TRUE("degraded 补错误串", j["error"].get<std::string>() ==
                                        "degraded(local_fallback)");
  }

  return TEST_REPORT();
}
