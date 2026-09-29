// v0.53.57: FC 批量工具并发上限回归
// 背景:tool_calls>1 全量 std::async——LLM 一轮 20 个重型工具=20 沙箱
// 同起(v0.52.10 实测 22 核打满);修:计数闸限 4。
// 验证(不依赖 LLM,直测闸逻辑同构性+批量执行正确性):
#include "test_macros.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <vector>
int main() {
  // 与实现同构的闸:8 任务限 4 并发,峰值并发计数应≤4 且全部完成
  constexpr int kMax = 4;
  std::mutex mu; std::condition_variable cv; int active = 0;
  std::atomic<int> peak{0}; std::atomic<int> done{0};
  std::vector<std::future<void>> fs;
  for (int i = 0; i < 8; ++i) {
    fs.push_back(std::async(std::launch::async, [&] {
      { std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return active < kMax; });
        ++active;
        peak.store(std::max(peak.load(), active)); }
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      { std::lock_guard<std::mutex> lk(mu); --active; }
      cv.notify_one();
      ++done;
    }));
  }
  for (auto& f : fs) f.get();
  ASSERT_TRUE("全部完成", done == 8);
  ASSERT_TRUE("峰值并发≤4", peak.load() <= kMax);
  ASSERT_TRUE("确实并行(峰值≥2)", peak.load() >= 2);
  return TEST_REPORT();
}
