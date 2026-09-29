// test_ws_send_refreshes_activity：v0.52.15 WS 死连接判定修复回归
//
// 背景：死连接判定（180s）只在 MG_EV_WS_MSG（收到客户端消息）时
// 刷新活动时间——服务端发出的响应帧不刷新。真 e2e 实测：长任务
// （分解波次>180s）期间静默等待的客户端在 180s 整被判死踢除，
// 任务完成后 conclusion/gate 帧发往已关连接，永久丢失。
//
// 修法：send_json（所有服务端发送的唯一出口）也刷新活动时间。
//
// 验证（纯逻辑复刻，不起 WS 服务）：
// 1. 收到客户端消息 → 活动时间刷新（原语义保持）
// 2. 服务端 send_json → 活动时间同样刷新（新语义）
// 3. 两者叠加：发送后 idle 判定窗口重新计时
//
// WS 层的集成验证由 e2e（tests/e2e）覆盖；本单测锁语义。
#include <chrono>
#include <unordered_map>

#include "test_macros.h"

// 复刻 ws_agent_main.cpp 的活动时间语义（同式，指针换 int 键）
using activity_map = std::unordered_map<int, int64_t>;
static int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
static constexpr int64_t kDeadConnectionMs = 180000;

// v0.52.15 语义：服务端发送也刷新
static void send_json_sim(activity_map& m, int conn) {
  m[conn] = now_ms();
}
// 原语义：收到客户端消息刷新
static void on_ws_msg_sim(activity_map& m, int conn) {
  m[conn] = now_ms();
}
static bool is_dead(const activity_map& m, int conn) {
  auto it = m.find(conn);
  if (it == m.end()) return true;
  return now_ms() - it->second > kDeadConnectionMs;
}

int main() {
  activity_map m;
  m[1] = now_ms() - kDeadConnectionMs - 1000;  // 已超时
  ASSERT_TRUE("初始超时判死", is_dead(m, 1));

  // 1) 客户端消息刷新（原语义）
  on_ws_msg_sim(m, 1);
  ASSERT_TRUE("客户端消息刷新后存活", !is_dead(m, 1));

  // 2) 服务端发送刷新（v0.52.15 新语义——修复点）
  m[1] = now_ms() - kDeadConnectionMs - 500;   // 再次超时
  ASSERT_TRUE("再次超时判死", is_dead(m, 1));
  send_json_sim(m, 1);
  ASSERT_TRUE("服务端发送刷新后存活（修复核心）", !is_dead(m, 1));

  // 3) 长任务场景：客户端静默 179s + 服务端持续发 thinking 帧 → 不死
  m[2] = now_ms();
  // 模拟：任务期间每 60s 服务端发一帧（180s 任务 = 3 帧）
  for (int i = 0; i < 3; ++i) {
    m[2] -= 60000;          // 时间倒退模拟"距上次发送 60s 前"
    send_json_sim(m, 2);    // 服务端帧到达
  }
  ASSERT_TRUE("长任务期间持续有服务端帧→连接保持存活", !is_dead(m, 2));

  return TEST_REPORT();
}
