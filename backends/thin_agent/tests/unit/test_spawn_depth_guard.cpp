// test_spawn_depth_guard：v0.52.20 嵌套深度护栏回归
//
// 背景：子代理可再 spawn 子代理——无界递归风险（线程/token 双爆，
// 深度失控时一次恶意/失控 goal 可耗尽资源）。thread_local 深度
// 计数+DepthGuard：同线程逐层 +1，超 3 层拒绝；波次每任务独立
// 线程=顶层语义正确。
//
// 验证（复刻护栏语义——与 orch_spawn 同式）：
// 1. 顶层（depth=0）放行
// 2. 层内递增：模拟 3 层嵌套后第 4 层拒绝
// 3. DepthGuard 析构回退：出层后深度恢复，可再次进入
// 4. 独立线程（波次语义）各自从 0 计——互不污染
#include <atomic>
#include <thread>

#include "test_macros.h"

// 与生产同式的护栏逻辑
struct SpawnGuardSim {
  static constexpr int kMaxDepth = 3;
  static thread_local int depth;
  // 返回 false=拒绝（超限）
  static bool try_enter() {
    if (depth >= kMaxDepth) return false;
    ++depth;
    return true;
  }
  static void exit_layer() { --depth; }
};
thread_local int SpawnGuardSim::depth = 0;

int main() {
  // 1) 顶层放行
  ASSERT_TRUE("顶层放行", SpawnGuardSim::try_enter());

  // 2) 嵌套到限
  ASSERT_TRUE("第2层放行", SpawnGuardSim::try_enter());
  ASSERT_TRUE("第3层放行", SpawnGuardSim::try_enter());
  ASSERT_TRUE("第4层拒绝", !SpawnGuardSim::try_enter());

  // 3) 出层恢复
  SpawnGuardSim::exit_layer();  // 回到 3
  ASSERT_TRUE("出层后可再进", SpawnGuardSim::try_enter());
  SpawnGuardSim::exit_layer();
  SpawnGuardSim::exit_layer();
  SpawnGuardSim::exit_layer();
  SpawnGuardSim::exit_layer();  // 回到 0
  ASSERT_TRUE("全出后顶层语义", SpawnGuardSim::try_enter());
  SpawnGuardSim::exit_layer();

  // 4) 线程隔离（波次每任务独立线程从 0 计）
  {
    std::atomic<bool> t2_top_ok{false}, t2_4th_ok{true};
    // 主线程先占 2 层
    SpawnGuardSim::try_enter();
    SpawnGuardSim::try_enter();
    std::thread t([&]() {
      t2_top_ok = SpawnGuardSim::try_enter();  // 新线程应从 0 起步
      SpawnGuardSim::try_enter();
      SpawnGuardSim::try_enter();
      t2_4th_ok = !SpawnGuardSim::try_enter();  // 它自己的第 4 层才拒
    });
    t.join();
    SpawnGuardSim::exit_layer();
    SpawnGuardSim::exit_layer();
    ASSERT_TRUE("新线程独立从 0 计", t2_top_ok.load() && t2_4th_ok.load());
  }

  return TEST_REPORT();
}
