// unit_gateway_send_queue：网关发送队列有界策略（v0.53.86）
//
// 背景：im_gateway_v2 的 g_pending_replies 在 v0.53.74 加了 1024 上限，但同文件的
// g_send_queue 一直没有上限（修一漏一）→ 平台变慢/回复洪峰时无界增长。本测试锁住
// "满了先丢最旧"的裁决契约（纯函数），防止上限被删除或语义被反转成"拒绝新消息"。
#include <cstddef>
#include <iostream>

#include "test_macros.h"
#include "thin_agent/gateway/SendQueuePolicy.h"

using thin_agent::gateway::kSendQueueCap;
using thin_agent::gateway::send_queue_needs_drop;

int main() {
  ASSERT_EQ("默认上限 512（与 pending 上限同量级，内存 <1MB）", kSendQueueCap, std::size_t(512));

  ASSERT_TRUE("空队列不需要丢弃", !send_queue_needs_drop(0));
  ASSERT_TRUE("未满不丢", !send_queue_needs_drop(kSendQueueCap - 1));
  ASSERT_TRUE("正好满 → 丢最旧（新消息永远优先）", send_queue_needs_drop(kSendQueueCap));
  ASSERT_TRUE("超限仍要丢", send_queue_needs_drop(kSendQueueCap + 10));
  ASSERT_TRUE("自定义上限生效", send_queue_needs_drop(4, 4));
  ASSERT_TRUE("自定义上限未满不丢", !send_queue_needs_drop(3, 4));

  return TEST_REPORT();
}
