// test_fanout_limits：单请求内扇出上限（v0.54.1, R88）
//
// 修复前：agent_debate 的 roles/rounds 全由请求体决定且**无上限** → roles×rounds 个
// std::async 各跑一遍完整 spawn_agent 管线（max_turns=6）＝客户端可驱动的资源耗尽。
#include <iostream>
#include <string>

#include "thin_agent/core/FanoutLimits.h"

static int g_fail = 0;
#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) { std::cout << "FAIL: " << msg << std::endl; ++g_fail; }     \
    else { std::cout << "PASS: " << msg << std::endl; }                       \
  } while (0)

int main() {
  using thin_agent::clamp_debate_fanout;
  using thin_agent::clamp_fanout_count;      // v0.54.2 (R89)
  using thin_agent::clamp_subtask_budget;    // v0.54.2 (R89)
  {
    auto c = clamp_debate_fanout(3, 2, 8, 5);
    CHECK(c.roles == 3 && c.rounds == 2 && !c.truncated, "正常请求原样通过（3 角色 × 2 轮）");
  }
  {
    auto c = clamp_debate_fanout(1000, 1000, 8, 5);
    CHECK(c.roles == 8 && c.rounds == 5, "超限请求被钳制到 8 角色 × 5 轮");
    CHECK(c.truncated && c.roles_requested == 1000 && c.rounds_requested == 1000,
          "截断标志与原始请求值如实回报（不静默丢）");
  }
  {
    auto c = clamp_debate_fanout(1000, 2, 8, 5);
    CHECK(c.roles == 8 && c.rounds == 2 && c.truncated, "只角色超限：仅角色被钳制且 truncated");
  }
  {
    auto c = clamp_debate_fanout(2, 99, 8, 5);
    CHECK(c.roles == 2 && c.rounds == 5 && c.truncated, "只轮数超限：仅轮数被钳制且 truncated");
  }
  {
    auto c = clamp_debate_fanout(0, 2, 8, 5);
    CHECK(c.roles == 0 && !c.truncated, "空角色列表不视为截断（上层用默认三角）");
  }
  {
    auto c = clamp_debate_fanout(3, -7, 8, 5);
    CHECK(c.rounds == 0, "负轮数按 0（不执行任何轮）");
  }
  {
    auto c = clamp_debate_fanout(3, 2, 0, 0);
    CHECK(c.roles == 1 && c.rounds == 1, "上限非法(0)时退化为 1×1 而非无限");
  }
  // ── v0.54.2 (R89): 工具参数驱动的扇出（delegate_task 同款形状）──
  {
    auto c = clamp_fanout_count(3, 8);
    CHECK(c.used == 3 && !c.truncated, "delegate_task 3 项 ≤ 上限 8：原样");
  }
  {
    auto c = clamp_fanout_count(1000, 8);
    CHECK(c.used == 8 && c.truncated && c.requested == 1000,
          "delegate_task 1000 项被钳到 8 且如实回报 requested=1000");
  }
  {
    auto b = clamp_subtask_budget(6, 150000, 0, 0);   // 0,0 = 用默认上限 12 轮 / 300s
    CHECK(b.max_turns == 6 && b.timeout_ms == 150000 && !b.truncated,
          "子任务预算在上限内原样（6 轮 / 150s）");
  }
  {
    auto b = clamp_subtask_budget(999, 999999, 0, 0);
    CHECK(b.max_turns == 12 && b.timeout_ms == 300000 && b.truncated,
          "子任务预算超限被钳到 12 轮 / 300s（原值可被任务 JSON 放大）");
  }
  {
    auto b = clamp_subtask_budget(0, 0, 0, 0);
    CHECK(b.max_turns == 1 && b.timeout_ms == 1000, "预算为 0\u002f负值时不退化为无限制");
  }

  std::cout << (g_fail == 0 ? "ALL PASS" : "FAILED") << std::endl;
  return g_fail == 0 ? 0 : 1;
}
