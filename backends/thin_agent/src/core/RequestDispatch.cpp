// RequestDispatch.cpp — 默认异步 + 即时白名单（判据方向反转，见头文件注释）
#include "thin_agent/core/RequestDispatch.h"

#include <cstddef>
#include <cstring>

namespace thin_agent {
namespace {

// 即时白名单：**毫秒级 + 纯内存/本地轻量读 + 长任务期间客户端/探针必须拿到**
// 的四类请求。加入此表前必须自问：
//   ① 该 handler 内有没有 LLM / 网络 / 子进程 / 模型加载 / 大文件或大表扫描？
//   ② 它是不是"长任务期间必须活着的活性探针"（探针类才值得破例）？
//   ③ 它有没有可能在无界数据上做 O(n) 全扫（=挂住事件循环的另一种形态）？
enum : std::size_t { kInstantCount = 7 };
const char* const kInstantTypes[kInstantCount] = {
    "ping",         // 活性探针（debug 客户端/看门狗依赖）
    "status",       // 状态探针（status_payload 纯内存组装）
    "chat_abort",   // 中断必须与正在跑的 chat 并发生效——入队即失效
    "metrics",      // 指标拉取（Prometheus 侧另有 HTTP /metrics）
    "cache_stats",  // 纯计数器读取
    "usage_stats",  // 纯计数器读取
    "event_recent", // 环形事件窗读取（event_window_ 有界）
};

}  // namespace

bool is_instant_ws_request(const std::string& type) {
  for (std::size_t i = 0; i < kInstantCount; ++i) {
    if (type == kInstantTypes[i]) return true;
  }
  return false;  // 未知/空类型同样入队（fail-safe）
}

std::size_t pick_unblocked_job(const std::vector<std::string>& queued_sids,
                               const std::map<std::string, int>& inflight) {
  for (std::size_t i = 0; i < queued_sids.size(); ++i) {
    auto it = inflight.find(queued_sids[i]);
    if (it == inflight.end() || it->second <= 0) return i;
  }
  return kNoPickableJob;
}

const char* const* instant_ws_request_types(std::size_t* count) {
  if (count) *count = kInstantCount;
  return kInstantTypes;
}

}  // namespace thin_agent
