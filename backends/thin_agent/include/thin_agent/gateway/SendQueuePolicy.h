#pragma once
// SendQueuePolicy.h — 网关发送队列的**有界**策略（v0.53.86）
//
// 背景（无界队列/容器堆积律，修一漏一实例）：
// im_gateway_v2 的 g_pending_replies 在 v0.53.74 加了 1024 上限，但**同文件的
// g_send_queue（转发消息到平台的发送队列）一直没有上限**——agent 侧回复洪峰或
// 平台侧变慢（HTTP 超时 10s/条）时，队列只增不减，内存无界增长；且丢弃行为
// 不可见（本仓既有"fire-and-forget 零可观测"律：丢消息可容忍，丢了没人知道不可）。
//
// 语义（与 g_pending_replies 的 1024 上限一致，取"丢最旧"而不是"拒绝新"）：
//   * 新消息永远优先入队（用户最近的话最可能要回复）；
//   * 溢出时丢队首（最旧）并记一次可观测事件；
//   * 上限取 512：每条 ~百字节级 → 峰值内存 < 1MB，远离"队列吃光内存"。
#include <cstddef>

namespace thin_agent {
namespace gateway {

/// 发送队列硬上限（条）。
inline constexpr std::size_t kSendQueueCap = 512;

/// 入队前的裁决：true = 需要先丢队首（丢弃最旧一条）再入队。
inline bool send_queue_needs_drop(std::size_t size, std::size_t cap = kSendQueueCap) {
  return size >= cap;
}

}  // namespace gateway
}  // namespace thin_agent
