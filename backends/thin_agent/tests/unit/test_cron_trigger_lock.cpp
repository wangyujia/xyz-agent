// v0.54.6 (R91 拍板)：trigger_now = 两段式锁 + **每任务串行执行锁**（"排队重跑"语义）
//
// 三态历史（防回归）：
//   ≤v0.54.2  整个函数持 `mu_`（含分钟级 callback）⇒ add/remove/list/stats/stop 全阻塞
//   v0.54.3   放锁执行 + "在飞即拒绝返回 false"——修了阻塞，但**丢了"排队重跑"语义**
//   v0.54.6   放锁执行 + 每任务锁**排队**（等前一次跑完，再跑一遍）
//
// 判据：
//   A) 慢 callback 期间 add_task 不被阻塞（<500ms）——R90 的成果必须保住
//   B) 同任务并发二次触发**排队**（不是被拒绝）：等前一次执行完后再执行一次，返回 true
//   C) 两次执行**不重叠**（并发峰值 == 1）
//   D) **callback 收到的是真任务行**（id/name/prompt 都对）
//      —— 回归判据：v0.54.3 曾在函数开头重复声明 `nlohmann::json task;`，锁内同名变量把外层
//      遮蔽 ⇒ 放锁后的 `execute_task(task)` 传的是**外层空对象**：宿主侧 name="unnamed"、
//      prompt=""，"手动立即触发"实际只跑一次**空 chat**，真 prompt 从未被执行。
//      （旧断言只数 callback 次数，对这种"内容空"零覆盖 —— 本条是补的盲区。）
#include "test_macros.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "thin_agent/core/CronScheduler.h"

using namespace std::chrono;

int main() {
  // v0.54.7 (R93): **密闭化**——本测试此前用固定 DB 路径且启动前不清理，于是：
  //   CronScheduler::start() 首次 tick 会**补跑**库里"已到期且 enabled=1"的行（cron catch-up
  //   语义）。旧版本测试留下 17 行 `schedule='2020-01-01T00:00:00'`（早已到期）且 enabled=1
  //   的残留 ⇒ 本进程 ticker 一启动就把它们跑掉 ⇒ callback 里多出 'trig'（旧 id）/ 'trig2'
  //   的额外调用 ⇒ B（cb_count==2）/C（并发峰值==1）/D（seen.size()==2、id/name/prompt）红。
  //   **这不是 -j4 专属：单跑也 1/3 概率红**（全量红、单跑绿的历史现象即此）。
  //   · 复现口令：向库里插入 1 行 `name='trig', schedule='2020-01-01T00:00:00', enabled=1,
  //     next_run_ts=now-100` → 立刻 5 条断言失败（已实测）。
  //   · 修法：每次运行从**空库**开始（连同 -wal/-shm），行为不再依赖历史残留。
  for (const char* f : {"/tmp/test_cron_trigger_lock.db",
                        "/tmp/test_cron_trigger_lock.db-wal",
                        "/tmp/test_cron_trigger_lock.db-shm"}) {
    std::remove(f);
  }
  std::atomic<bool> in_cb{false};
  std::atomic<bool> release_first{false};
  std::atomic<int> cb_count{0};
  std::atomic<int> concurrent{0};
  std::atomic<int> max_concurrent{0};
  std::mutex tasks_mu;
  std::vector<nlohmann::json> seen;

  thin_agent::CronScheduler cron;
  cron.start("/tmp/test_cron_trigger_lock.db",
             thin_agent::CronScheduler::TaskCallback([&](const nlohmann::json& t) {
               const int c = concurrent.fetch_add(1) + 1;
               int prev = max_concurrent.load();
               while (c > prev && !max_concurrent.compare_exchange_weak(prev, c)) {
               }
               const int n = cb_count.fetch_add(1) + 1;
               {
                 std::lock_guard<std::mutex> lk(tasks_mu);
                 seen.push_back(t);
               }
               if (n == 1) {
                 in_cb = true;
                 while (!release_first.load()) std::this_thread::sleep_for(milliseconds(20));
               }
               concurrent.fetch_sub(1);
             }),
             200);

  // 调度用 "every 3600s"（next_run_ts = now+3600，ticker 不会来插一脚，判据确定）
  auto added = cron.add_task("trig", "every 3600s", "hi");
  const int task_id = added.value("id", 0);
  ASSERT_TRUE("add_task 拿到 id", task_id > 0);

  // 后台线程：首次 trigger_now（进入 callback 后阻塞）
  std::atomic<bool> trig1_ret{false};
  std::thread trig1([&]() { trig1_ret = cron.trigger_now(task_id); });

  auto t0 = steady_clock::now();
  while (!in_cb.load() &&
         duration_cast<milliseconds>(steady_clock::now() - t0).count() < 3000) {
    std::this_thread::sleep_for(milliseconds(10));
  }
  ASSERT_TRUE("首次 callback 已进入（trigger_now 正在执行）", in_cb.load());

  // ── A) 慢 callback 期间 add_task 不被阻塞（R90 成果）──
  std::atomic<bool> add_done{false};
  std::atomic<long> add_ms{-1};
  std::thread adder([&]() {
    auto ta = steady_clock::now();
    auto r = cron.add_task("trig2", "every 3600s", "hi2");
    add_ms = (long)duration_cast<milliseconds>(steady_clock::now() - ta).count();
    add_done = true;
    (void)r;
  });
  {
    auto tw = steady_clock::now();
    while (!add_done.load() &&
           duration_cast<milliseconds>(steady_clock::now() - tw).count() < 800) {
      std::this_thread::sleep_for(milliseconds(10));
    }
  }
  ASSERT_TRUE("A 慢 callback 期间 add_task 快速完成（<500ms，修前会阻塞）",
              add_done.load() && add_ms.load() >= 0 && add_ms.load() < 500);

  // ── B) 二次触发：**排队**（不是拒绝）──
  std::atomic<bool> trig2_done{false};
  std::atomic<bool> trig2_ret{false};
  std::thread trig2([&]() { trig2_ret = cron.trigger_now(task_id); trig2_done = true; });
  {
    auto tw = steady_clock::now();
    while (duration_cast<milliseconds>(steady_clock::now() - tw).count() < 400) {
      std::this_thread::sleep_for(milliseconds(20));
    }
  }
  ASSERT_TRUE("B 排队中：二次触发尚未返回（在等前一次执行完）", !trig2_done.load());
  ASSERT_TRUE("B 排队期间 callback 仍只跑了 1 次（未并发重复执行）", cb_count.load() == 1);

  release_first = true;  // 放行首次 callback ⇒ 二次触发应紧接着执行

  {
    auto tw = steady_clock::now();
    while (!trig2_done.load() &&
           duration_cast<milliseconds>(steady_clock::now() - tw).count() < 5000) {
      std::this_thread::sleep_for(milliseconds(20));
    }
  }
  ASSERT_TRUE("B 二次触发最终返回（排队重跑完成，未被无限阻塞）", trig2_done.load());
  ASSERT_TRUE("B 二次触发返回 true（旧语义=排队重跑，不是拒绝）", trig2_ret.load());
  ASSERT_TRUE("B callback 共执行 2 次（= 排队重跑）", cb_count.load() == 2);
  ASSERT_TRUE("C 两次执行不重叠（并发峰值 == 1）", max_concurrent.load() == 1);

  trig1.join();
  trig2.join();
  adder.join();
  ASSERT_TRUE("首次 trigger_now 返回 true", trig1_ret.load());

  // ── D) 负载正确性（v0.54.3 空对象/null 回归的判据）──
  // 读法**逐字对齐宿主**（AgentService.cpp 的 cron 回调：name/prompt/workdir/no_agent）：
  // 修前 callback 收到 null，宿主第一句 value() 就抛 type_error.306 ⇒ 服务侧
  // "手动立即触发"必然失败、`cron_fired: <name> | prompt=...` 事件永不出现。
  {
    std::lock_guard<std::mutex> lk(tasks_mu);
    ASSERT_TRUE("D callback 收到 2 次调用", seen.size() == 2);
    for (size_t i = 0; i < seen.size(); ++i) {
      const std::string host_name = seen[i].value("name", std::string("unnamed"));
      const std::string host_prompt = seen[i].value("prompt", std::string());
      const std::string host_workdir = seen[i].value("workdir", std::string());
      const bool host_no_agent = seen[i].value("no_agent", false);
      ASSERT_TRUE("D 收到真任务 id（非 null 对象）", seen[i].value("id", 0) == task_id);
      ASSERT_TRUE("D 宿主读法不抛且 name 正确", host_name == "trig");
      ASSERT_TRUE("D 宿主读法不抛且 prompt 正确", host_prompt == "hi");
      ASSERT_TRUE("D 宿主读法不抛且 workdir/no_agent 取到默认值",
                  host_workdir.empty() && !host_no_agent);
    }
  }

  cron.stop();
  return TEST_REPORT();
}
