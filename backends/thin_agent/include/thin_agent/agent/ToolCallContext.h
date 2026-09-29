#pragma once

#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>

namespace thin_agent {
namespace agent {

/// v0.52.23: 工具调用上下文——协作式取消基础设施（#6 方案 A）。
///
/// 背景：v0.52.11 超时语义=放弃等待（detached 线程自然退出）。
/// 长跑工具（fs 遍历/popen/curl）在超时时长内线程堆积（病态负载
/// ASAN 实测 5000+；exit 拆卸期 UAF）。协作式取消：调用方超时
/// 放弃后置位 cancel 标志，长跑工具在关键循环点检查并提前退出。
///
/// 兼容性：ToolSchema::execute 保持 `json(params)` 签名不变（63 处
/// 注册点零改动）——协作工具改查 thread_local 当前调用上下文
///（g_current_ctx 由 ToolRegistry::call 进/出置设），非协作工具
/// 行为=现状（不劣化）。
struct ToolCallContext {
  std::shared_ptr<std::atomic<bool>> cancel =
      std::make_shared<std::atomic<bool>>(false);

  bool cancelled() const { return cancel->load(std::memory_order_relaxed); }
  void cancel_now() { cancel->store(true, std::memory_order_relaxed); }
};

/// 当前线程的工具调用上下文（协作工具查询入口）。
/// nullptr=无取消语义（直调/非 ToolRegistry 路径）。
inline thread_local ToolCallContext* g_current_tool_ctx = nullptr;

inline ToolCallContext* current_tool_ctx() {
  return g_current_tool_ctx;
}

}  // namespace agent
}  // namespace thin_agent
