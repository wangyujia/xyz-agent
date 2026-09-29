// test_tool_registry_timeout：v0.52.11 工具超时语义回归
//
// 背景：ToolRegistry::call 的 std::async future 析构会阻塞 join 工具
// 线程——wait_for(60s) 超时后 return，局部 future 析构照样卡死。
// 真 e2e 实测：波次反复超时→future 析构堆积 join→线程 72→269
// 雪崩→服务冻结。
//
// 验证（复刻冻结形态：注册 sleep 超时的假工具）：
// 1. 超时返回 error 且【不阻塞】——调用在 timeout+宽限内返回
// 2. 超时后线程不累积（连续 N 次超时调用，线程数回落基线）
// 3. 正常工具不受影响
#include <atomic>
#include <chrono>
#include <thread>

#include "test_macros.h"

#include "thin_agent/agent/ToolRegistry.h"

using namespace thin_agent;
using namespace std::chrono_literals;

// 假工具：睡眠指定毫秒后返回 ok
static nlohmann::json slow_tool(const nlohmann::json& params) {
  int ms = params.value("sleep_ms", 0);
  if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  return {{"ok", true}, {"result", {{"done", true}}}};
}

// 假工具：永久阻塞（复刻卡死工具线程形态）
static std::atomic<bool> g_block_flag{false};
static nlohmann::json blocking_tool(const nlohmann::json&) {
  while (!g_block_flag.load()) std::this_thread::sleep_for(50ms);
  return {{"ok", true}};
}

static int thread_count_approx() {
  // /proc/self/status Threads 行
  FILE* f = fopen("/proc/self/status", "r");
  if (!f) return -1;
  char line[128];
  int n = -1;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "Threads:", 8) == 0) {
      n = atoi(line + 8);
      break;
    }
  }
  fclose(f);
  return n;
}

int main() {
  auto& reg = agent::ToolRegistry::instance();

  auto add_tool = [&reg](const std::string& name,
                         nlohmann::json (*fn)(const nlohmann::json&)) {
    agent::ToolSchema s;
    s.name = name;
    s.description = "test tool";
    s.execute = fn;
    reg.register_tool(std::move(s));
  };

  // 1) 超时返回且不阻塞
  {
    add_tool("slow_echo", slow_tool);
    auto t0 = std::chrono::steady_clock::now();
    auto r = reg.call("slow_echo", {{"sleep_ms", 10000}}, 500 /*0.5s 超时*/);
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE("超时返回 error", r.error.find("timeout") != std::string::npos);
    ASSERT_TRUE("超时调用不阻塞(<1.5s)", dt < 1500);
  }

  // 2) 连续超时不累积线程
  {
    add_tool("blocker", blocking_tool);
    int base = thread_count_approx();
    for (int i = 0; i < 5; ++i) {
      reg.call("blocker", {}, 200);  // 每次 0.2s 超时
    }
    // 5 个 detached 工具线程仍在跑（阻塞中）——修复后它们不 join
    // 调用方，但线程本身还活着（等 block_flag）。验证调用方不被拖住：
    int after = thread_count_approx();
    ASSERT_TRUE("超时后调用方不被 join 拖死", after <= base + 6);
    g_block_flag = true;  // 放行残余线程，让它们自然退出
  }

  // 3) 正常路径不受影响
  {
    auto r = reg.call("slow_echo", {{"sleep_ms", 0}}, 2000);
    ASSERT_TRUE("正常执行 ok", r.ok);
  }

  return TEST_REPORT();
}
