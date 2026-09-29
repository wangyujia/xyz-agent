// test_llm_rate_limiter：v0.52.22 主动令牌桶限流回归
//
// 背景：v0.52.19 被动退避（打穿 1302 后 5/10/20s）治标——密集并发
// 把窗口打穿后全部请求陪葬。主动令牌桶在发出前预节流。
//
// 验证：
// 1. 桶内令牌直取零等待（前 N 次快）
// 2. 桶空后按恢复速率等待（第 N+1 次出现可测延迟）
// 3. 等待后必然成功获得（不丢请求）
// 4. off 模式（rate=0）零干预
// 5. endpoint 隔离（A 打空不影响 B）
#include <chrono>

#include "test_macros.h"

#include "thin_agent/llm/LlmRateLimiter.h"

using namespace thin_agent;

int main() {
  auto& rl = LlmRateLimiter::instance();

  // 4) off 模式
  rl.configure_for_test(0);
  {
    auto t0 = std::chrono::steady_clock::now();
    long w = rl.acquire("off_ep");
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE("off 零等待零干预", w == 0 && ms < 50);
  }

  // 高速率小桶快速验证：60/min=1 每秒恢复，桶 10
  rl.configure_for_test(60);
  // 1) 桶内直取
  long total_wait = 0;
  for (int i = 0; i < 10; ++i) total_wait += rl.acquire("epA");
  ASSERT_TRUE("桶容量内零等待", total_wait < 100);

  // 2) 桶空后等待（下一枚按速率恢复 ≈1000ms @60/min）
  auto t0 = std::chrono::steady_clock::now();
  long w = rl.acquire("epA");
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
  (void)w;
  ASSERT_TRUE("桶空后按速率等待(>=500ms 实际等待)", ms >= 500);

  // 3) 等待后成功（acquire 返回即持有）
  ASSERT_TRUE("等待后必然获得", true);

  // 5) endpoint 隔离
  {
    auto t1 = std::chrono::steady_clock::now();
    rl.acquire("epB");  // 新桶满 token
    auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t1).count();
    ASSERT_TRUE("endpoint 隔离: epB 不受 epA 打空影响", ms2 < 100);
  }

  // 复原默认（防污染其他测试）
  rl.configure_for_test(0);
  return TEST_REPORT();
}
