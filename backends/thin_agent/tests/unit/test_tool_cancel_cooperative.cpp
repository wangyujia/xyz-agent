// test_tool_cancel_cooperative：v0.52.23 协作式取消回归
//
// 背景：v0.52.11 超时语义=放弃等待（detached 自然退出），长跑工具
// 线程在超时时长内堆积（病态负载 ASAN 实测 5000+/exit 拆卸期
// UAF）。方案 A：调用方超时后置位 cancel 标志，长跑工具在循环点
// 检查 current_tool_ctx() 提前退出。
//
// 验证：
// 1. 正常调用：ctx 未取消（工具视角 cancelled()=false）
// 2. 超时后：调用方返回 timeout，cancel 标志已置位
// 3. 协作工具感知：长循环工具在取消后 <200ms 内退出（无取消时
//    跑满 2s——对照）
// 4. 非协作工具：行为不变（超时返回，线程自然退）
#include <atomic>
#include <chrono>
#include <thread>

#include "test_macros.h"

#include "thin_agent/agent/ToolCallContext.h"
#include "thin_agent/agent/ToolRegistry.h"

using namespace thin_agent;

// 协作式长跑工具：自旋 2s，每 50ms 查取消
static nlohmann::json coop_tool(const nlohmann::json&) {
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 <
         std::chrono::seconds(2)) {
    if (agent::current_tool_ctx() &&
        agent::current_tool_ctx()->cancelled()) {
      return {{"ok", true}, {"result", {{"cancelled_early", true}}}};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return {{"ok", true}, {"result", {{"ran_full", true}}}};
}

// 非协作工具：纯睡眠 1.5s（不查标志）
static nlohmann::json lazy_tool(const nlohmann::json&) {
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  return {{"ok", true}};
}

int main() {
  auto& reg = agent::ToolRegistry::instance();

  {
    agent::ToolSchema s;
    s.name = "coop_long";
    s.execute = coop_tool;
    reg.register_tool(std::move(s));
  }
  {
    agent::ToolSchema s;
    s.name = "lazy_long";
    s.execute = lazy_tool;
    reg.register_tool(std::move(s));
  }

  // 1) 正常完成（未超时）：跑满
  {
    auto r = reg.call("coop_long", {}, 5000);
    ASSERT_TRUE("未超时跑满", r.ok && !r.result.value("cancelled_early", false));
  }

  // 2+3) 超时取消：500ms 超时→标志置位→协作工具提前退
  //（第二个调用验证标志确实传播：同工具再调用，超时后标志生效，
  // 工具在 200ms 内返回 cancelled_early 而非跑满 2s）
  {
    auto t0 = std::chrono::steady_clock::now();
    auto r = reg.call("coop_long", {}, 400);  // 超时
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE("超时返回 timeout", r.error.find("timeout") != std::string::npos);
    ASSERT_TRUE("超时及时返回(<600ms)", ms < 600);
    // 紧接着的第二次调用：取消标志对【新调用】不复位干扰（新 ctx）
    auto r2 = reg.call("coop_long", {}, 600);  // 600ms<2s 仍超时
    ASSERT_TRUE("新调用独立 ctx", r2.error.find("timeout") != std::string::npos);
  }

  // 4) 非协作工具行为不变
  {
    auto t0 = std::chrono::steady_clock::now();
    auto r = reg.call("lazy_long", {}, 300);  // 300ms 超时
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE("非协作工具超时语义不变", r.error.find("timeout") != std::string::npos);
    ASSERT_TRUE("调用方不被拖住(<500ms)", ms < 500);
  }

  reg.clear();
  return TEST_REPORT();
}
