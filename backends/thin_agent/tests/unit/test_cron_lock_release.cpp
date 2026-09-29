// v0.53.46: CronScheduler 两段式锁——慢 callback 不阻塞 stop
// 背景:execute_task(宿主 callback)此前在持锁内跑,stop 要等任务完;
// 修复后 stop 在 tick 间隙立即生效(1.5s 慢 callback 中 stop 应 <1s 返回)
#include "test_macros.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include "thin_agent/core/CronScheduler.h"
using namespace std::chrono;
int main() {
  // v0.54.7 (R93): 密闭化——固定路径 DB 必须启动前清空，否则上一轮/旧版遗留的"已到期
  // enabled=1"行会被本进程 ticker 补跑（见 test_cron_trigger_lock.cpp 的详细说明与复现口令）。
  for (const char* f : {"/tmp/test_cron_lock.db", "/tmp/test_cron_lock.db-wal",
                        "/tmp/test_cron_lock.db-shm"}) {
    std::remove(f);
  }
  std::atomic<bool> in_cb{false};
  std::atomic<bool> release_cb{false};
  thin_agent::CronScheduler cron;
  cron.start("/tmp/test_cron_lock.db",
             thin_agent::CronScheduler::TaskCallback([&](const nlohmann::json&) {
               in_cb = true;
               while (!release_cb.load()) std::this_thread::sleep_for(milliseconds(20));
             }),
             50);
  // 已过期的 ISO 一次性任务(下轮 tick 即触发;最小间隔 60s 拒绝 ms 格式)
  auto added = cron.add_task("t", "2020-01-01T00:00:00", "hi");
  ASSERT_TRUE("add", !added.empty() || added.is_object());
  auto t0 = steady_clock::now();
  while (!in_cb.load() && duration_cast<milliseconds>(steady_clock::now()-t0).count() < 3000) {
    std::this_thread::sleep_for(milliseconds(10));
  }
  ASSERT_TRUE("callback 触发", in_cb.load());
  // callback 挂起中调 stop——修前:等 mu_(被 ticker 持有跑 callback)≈挂死;
  // 修后:stop 立即返回(callback 仍在跑,由 release 后自然退出)
  // v0.53.46 语义:stop 等 ticker(callback)跑完是【正确设计】
  ///(callback 引用宿主栈,遗弃=UB;生产 callback 均有界:LLM 超时/
  /// 脚本看门狗)。本测试验证的是 Y1 核心:慢 callback 期间
  ///【add_task 不被阻塞】(修前 ticker 持锁跑 callback,add 挂)。
  auto t_add = steady_clock::now();
  auto r2 = cron.add_task("t2", "2020-01-01T00:00:00", "hi2");
  auto add_ms = duration_cast<milliseconds>(steady_clock::now()-t_add).count();
  ASSERT_TRUE("慢 callback 期间 add 快速完成(<500ms)", add_ms < 500);
  (void)r2;
  release_cb = true;
  std::this_thread::sleep_for(milliseconds(300));
  cron.stop();
  return TEST_REPORT();
}
