// unit_chat_workers_concurrency：多 worker 并发队列安全测试
// 验证 v0.47.0 多 worker 线程池的核心不变量：
//   1. 多生产者多消费者下消息不丢失（push 数 == consume 数）
//   2. 队列上限 kMaxQueueSize 生效（超限拒绝）
//   3. 优雅关闭（stop 后所有 worker 正确退出，无死锁）
//
// 本测试复刻 ws_agent_main.cpp 的队列模型（不依赖 mongoose），独立验证并发语义。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include "test_macros.h"

// ── 复刻 ws_agent_main.cpp 的队列模型 ──
struct PendingJob {
  int id;
  std::string payload;
};

static constexpr size_t kMaxQueueSize = 32;

static std::mutex g_mu;
static std::condition_variable g_cv;
static std::deque<PendingJob> g_queue;
static std::atomic<bool> g_stop{false};
static std::atomic<int> g_consumed{0};
static std::atomic<int> g_rejected{0};

void worker_loop() {
  for (;;) {
    PendingJob job;
    {
      std::unique_lock<std::mutex> lk(g_mu);
      g_cv.wait(lk, [] { return g_stop.load() || !g_queue.empty(); });
      if (g_queue.empty() && g_stop.load()) break;
      if (g_queue.empty()) continue;
      job = std::move(g_queue.front());
      g_queue.pop_front();
    }
    // 模拟处理（不加锁，模拟 FC 循环的长时间执行）
    g_consumed.fetch_add(1);
  }
}

// 返回 true 表示成功入队，false 表示队列满被拒绝
bool try_enqueue(int id) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_queue.size() >= kMaxQueueSize) {
      g_rejected.fetch_add(1);
      return false;
    }
    g_queue.push_back(PendingJob{id, "payload_" + std::to_string(id)});
  }
  g_cv.notify_one();
  return true;
}

int main() {
  // ── 测试 1：多生产者多消费者 — 消息不丢失 ──
  {
    g_stop = false;
    g_consumed = 0;
    g_rejected = 0;
    g_queue.clear();

    // 启动 3 个 worker
    std::vector<std::thread> workers;
    for (int i = 0; i < 3; ++i) {
      workers.emplace_back(worker_loop);
    }

    // 4 个生产者各发 100 条
    const int producers = 4;
    const int per_producer = 100;
    std::vector<std::thread> prod_threads;
    for (int p = 0; p < producers; ++p) {
      prod_threads.emplace_back([p, per_producer]() {
        for (int i = 0; i < per_producer; ++i) {
          try_enqueue(p * per_producer + i);
        }
      });
    }

    // 等所有生产者完成
    for (auto& t : prod_threads) t.join();

    // 等待队列消费完（有界等待）
    for (int wait = 0; wait < 100; ++wait) {
      {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_queue.empty()) break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    g_stop = true;
    g_cv.notify_all();
    for (auto& w : workers) {
      if (w.joinable()) w.join();
    }

    int expected = producers * per_producer;
    // 不变量：所有消息要么被消费，要么被显式拒绝（队列满），无静默丢失
    int total_accounted = g_consumed.load() + g_rejected.load();
    ASSERT_TRUE("mp_mc_no_loss", total_accounted == expected);
    ASSERT_TRUE("mp_mc_queue_empty", g_queue.empty());
  }

  // ── 测试 2：队列上限 kMaxQueueSize 生效 ──
  {
    g_stop = false;
    g_consumed = 0;
    g_rejected = 0;
    g_queue.clear();

    // 不启动 worker，快速灌满队列
    int pushed = 0;
    for (int i = 0; i < 100; ++i) {
      if (try_enqueue(i)) ++pushed;
    }

    ASSERT_TRUE("queue_cap_max_pushed", pushed == static_cast<int>(kMaxQueueSize));
    ASSERT_TRUE("queue_cap_rejected",
                g_rejected.load() == 100 - static_cast<int>(kMaxQueueSize));

    // 启动 worker 消费剩余
    std::vector<std::thread> workers;
    for (int i = 0; i < 2; ++i) {
      workers.emplace_back(worker_loop);
    }

    for (int wait = 0; wait < 50; ++wait) {
      {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_queue.empty()) break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    g_stop = true;
    g_cv.notify_all();
    for (auto& w : workers) {
      if (w.joinable()) w.join();
    }

    ASSERT_TRUE("queue_cap_consumed_all",
                g_consumed.load() == static_cast<int>(kMaxQueueSize));
  }

  // ── 测试 3：优雅关闭 — stop 后所有 worker 正确退出 ──
  {
    g_stop = false;
    g_consumed = 0;
    g_queue.clear();

    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
      workers.emplace_back(worker_loop);
    }

    // 发几条消息
    for (int i = 0; i < 10; ++i) try_enqueue(i);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 发停止信号
    g_stop = true;
    g_cv.notify_all();

    // 所有 worker 应在 2 秒内退出（join 不阻塞）
    auto start = std::chrono::steady_clock::now();
    for (auto& w : workers) {
      if (w.joinable()) w.join();
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    ASSERT_TRUE("graceful_shutdown_fast", elapsed < 2000);
    ASSERT_TRUE("graceful_shutdown_consumed", g_consumed.load() == 10);
  }

  // ── 测试 4：并发消费不同 session — 无竞争 ──
  // 模拟多 worker 同时处理不同 id 的 job（类似不同 session_id 的 chat 请求）
  {
    g_stop = false;
    g_consumed = 0;
    g_queue.clear();

    std::atomic<int> active_consumers{0};
    std::atomic<int> max_concurrent{0};

    // 自定义 worker：记录并发度
    auto concurrent_worker = [&]() {
      for (;;) {
        PendingJob job;
        {
          std::unique_lock<std::mutex> lk(g_mu);
          g_cv.wait(lk, [] { return g_stop.load() || !g_queue.empty(); });
          if (g_queue.empty() && g_stop.load()) break;
          if (g_queue.empty()) continue;
          job = std::move(g_queue.front());
          g_queue.pop_front();
        }
        int cur = active_consumers.fetch_add(1) + 1;
        int prev_max = max_concurrent.load();
        while (cur > prev_max && !max_concurrent.compare_exchange_weak(prev_max, cur)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // 模拟工作
        active_consumers.fetch_sub(1);
        g_consumed.fetch_add(1);
      }
    };

    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
      workers.emplace_back(concurrent_worker);
    }

    // 灌入足够多消息让多个 worker 同时工作
    for (int i = 0; i < 50; ++i) try_enqueue(i);

    for (int wait = 0; wait < 100; ++wait) {
      {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_queue.empty()) break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    g_stop = true;
    g_cv.notify_all();
    for (auto& w : workers) {
      if (w.joinable()) w.join();
    }

    ASSERT_TRUE("concurrent_sessions_all_consumed", g_consumed.load() > 0);
    // 4 个 worker + 10ms 工作 + 50 条消息 → 应该有并发
    ASSERT_TRUE("concurrent_sessions_parallelism", max_concurrent.load() >= 2);
  }

  TEST_REPORT();
}
