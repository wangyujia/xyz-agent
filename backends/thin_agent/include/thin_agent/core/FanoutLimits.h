// v0.54.1 (R88): **单请求内扇出上限**——纯策略、无 IO、可单测（单点收口）。
//
// 背景（实测/代码）: `agent_debate` 的 roles 与 rounds **全部取自请求体且无上限**：
//   for (round < rounds) { for (role : roles) { std::async(... 完整 spawn_agent 管线, max_turns=6) } }
// ⇒ 客户端可用 `roles=[...1000]` × `rounds=1000` 驱动**百万级** async 全流程（线程/内存/
// LLM 配额耗尽＝请求即可打垮服务），且请求本身要等全部轮次跑完才返回。
// 与 R86 的 goal_auto_reason（批量**串行**无上限）同族——这里"界"是**扇出**。
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>   // v0.54.16: 本头用 int64_t 却没 include（Linux 侧靠传递包含侥幸编过；

namespace thin_agent {

struct FanoutCap {
  size_t roles{0};                // 实际采用的角色数
  int    rounds{0};               // 实际采用的轮数
  size_t roles_requested{0};
  int    rounds_requested{0};
  bool   truncated{false};        // 任一维度被截断
};

/// 把请求方的 (roles, rounds) 钳制到 (max_roles, max_rounds) 之内。
/// 负轮数按 0 处理（不再做无意义的钳到 1）；截断信息返回给调用方**如实回报**。
inline FanoutCap clamp_debate_fanout(size_t roles_requested, int rounds_requested,
                                     size_t max_roles, int max_rounds) {
  FanoutCap c;
  c.roles_requested = roles_requested;
  c.rounds_requested = rounds_requested;
  const size_t mr = max_roles == 0 ? 1 : max_roles;
  const int mmr = max_rounds <= 0 ? 1 : max_rounds;
  c.roles = std::min(roles_requested, mr);
  c.rounds = std::max(0, std::min(rounds_requested, mmr));
  c.truncated = (roles_requested > c.roles) || (rounds_requested > c.rounds);
  return c;
}

/// v0.54.2 (R89): **工具参数/请求体驱动的扇出** —— 通用数量钳制 + 每项预算钳制。
/// 背景：`delegate_task` 工具把 LLM 给的 `tasks` 数组**逐个 std::async 同时启动**（无数量、
/// 无并发、无单项预算上限；`max_turns`/`timeout_ms` 也取自任务 JSON，默认 timeout 150s）
/// ⇒ 一次调用即可起 N 个完整 AgentLoop（线程 × N、LLM 配额）。
struct CountCap {
  size_t used{0};
  size_t requested{0};
  bool   truncated{false};
};

inline CountCap clamp_fanout_count(size_t requested, size_t max_allowed) {
  CountCap c;
  c.requested = requested;
  const size_t m = max_allowed == 0 ? 1 : max_allowed;
  c.used = std::min(requested, m);
  c.truncated = requested > c.used;
  return c;
}

struct BudgetCap {
  int  max_turns{0};
  int64_t timeout_ms{0};
  int  turns_requested{0};
  int64_t timeout_requested{0};
  bool truncated{false};
};

/// 每项子任务预算钳制（上限为 0 时取默认 12 轮 / 300s，避免"无限预算"）
inline BudgetCap clamp_subtask_budget(int turns_requested, int64_t timeout_requested,
                                      int max_turns_allowed, int64_t max_timeout_allowed) {
  BudgetCap b;
  const int mt = max_turns_allowed > 0 ? max_turns_allowed : 12;
  const int64_t mto = max_timeout_allowed > 0 ? max_timeout_allowed : 300000;
  b.turns_requested = turns_requested;
  b.timeout_requested = timeout_requested;
  b.max_turns = std::max(1, std::min(turns_requested, mt));
  b.timeout_ms = std::max<int64_t>(1000, std::min(timeout_requested, mto));
  b.truncated = (turns_requested > b.max_turns) || (timeout_requested > b.timeout_ms);
  return b;
}

}  // namespace thin_agent
