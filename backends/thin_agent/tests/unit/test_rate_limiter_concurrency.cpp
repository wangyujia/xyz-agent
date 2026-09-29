// v0.53.36: 限流并发公平性——cv 排队替代旧 sleep 惊群。
// 14 workers @60/min(1 token/s,桶初值 10):期望总耗时≈4s
// (10 枚立得+4 枚按秒恢复)。旧实现惊群会>>4s 且 tokens 负余额。
// 4s 预算防慢机误报(RUN_SERIAL 不需要——纯内存无端口)。
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include "thin_agent/llm/LlmRateLimiter.h"

int main() {
  auto& rl = thin_agent::LlmRateLimiter::instance();
  rl.configure_for_test(60);  // 1 token/s
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> ts;
  for (int i = 0; i < 14; ++i) ts.emplace_back([&rl] { rl.acquire("ep"); });
  for (auto& t : ts) t.join();
  const double sec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("14 workers total=%.2fs (expect ~4s)\n", sec);
  const bool pass = sec > 3.5 && sec < 5.5;
  std::printf("%s\n", pass ? "PASS: rate limiter fair queuing" : "FAIL");
  return pass ? 0 : 1;
}
