#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <map>
#include <mutex>
#include <string>

namespace thin_agent {

/// v0.52.22: LLM 端点主动限流器（令牌桶，进程级单例，per-endpoint）。
///
/// 背景：v0.52.19 的 rate_limited 识别是【被动】退避（打穿 1302 后
/// 5/10/20s 重试）——密集并发（3+ 波次×多轮）会把整个账户窗口打穿，
/// 后续全部请求陪葬至窗口恢复。主动令牌桶在【发出前】预节流：
/// 平均速率内直接放行，超速排队等待（不超时），桶容量允许小突发。
///
/// 语义：
/// - rate_per_min：每分钟令牌数（默认 30——GLM coding 端点实测安全值，
///   可 env THIN_AGENT_LLM_RATE_PER_MIN 覆盖；0=不限流）
/// - burst：桶容量（默认 rate 的 1/3——允许短突发不惩罚正常使用）
/// - acquire(endpoint)：拿令牌；不足则按需等待（令牌按时间恢复）
/// - 与熔断器互补：限流管"别打太快"，熔断管"坏了别打"
class LlmRateLimiter {
 public:
  static LlmRateLimiter& instance() {
    static LlmRateLimiter rl;
    return rl;
  }

  /// 阻塞获取令牌（等待恢复，不主动失败——调用方已有超时控制）。
  /// v0.53.36: 条件变量替代 sleep 轮询——此前多 worker 等同一 endpoint 时
  /// 同时醒来抢同一枚令牌,败者按【完整周期】重新等待(实际是 N×串行等待),
  /// 且 refill 量不足以覆盖时 tokens 扣成负数。cv 通知按到达序逐个放行。
  /// 返回实际等待的毫秒数（诊断用）。
  long acquire(const std::string& endpoint) {
    if (rate_per_min_ <= 0) return 0;  // 不限流
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(mu_);
    auto& b = buckets_[endpoint];
    refill(b);
    // 排队等待直至令牌≥1(notify 在每次 refill 后广播,拿到者扣减后
    // 余量可能仍<1,剩余等待者继续挂起——公平 FIFO 由 cv 唤醒序近似保证)
    // 时间驱动令牌恢复+事件驱动唤醒的错配:全部等待者挂起后无人 notify,
    // 令牌恢复也无人重判——必须周期性醒来 refill(100ms 粒度,代价可忽略)。
    while (!([&] { refill(b); return b.tokens >= 1.0; }())) {
      cv_.wait_for(lk, std::chrono::milliseconds(100));
    }
    b.tokens -= 1.0;
    cv_.notify_all();  // 令牌恢复是连续速率,扣减后可能够下一人——广播重判
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
  }

  /// 测试注入：重置状态与速率。
  void configure_for_test(int rate_per_min) {
    std::lock_guard<std::mutex> lk(mu_);
    rate_per_min_ = rate_per_min;
    buckets_.clear();
  }

  int rate_per_min() const { return rate_per_min_; }

 private:
  LlmRateLimiter() {
    if (const char* e = std::getenv("THIN_AGENT_LLM_RATE_PER_MIN")) {
      rate_per_min_ = atoi(e);
    }
  }

  struct Bucket {
    double tokens = 10.0;  // 初始满桶（允许冷启动突发）
    std::chrono::steady_clock::time_point last =
        std::chrono::steady_clock::now();
  };

  void refill(Bucket& b) {
    // v0.53.36: 令牌恢复后广播等待者(cv 版 acquire 的谓词重判依赖此通知)
    // 注意:refill 在持锁环境下调用(acquire 内 unique_lock)——直接 notify 安全。
    struct NotifyOnExit {
      std::condition_variable* cv;
      bool fire = false;
      ~NotifyOnExit() { if (fire) cv->notify_all(); }
    } notify_on_exit{&cv_, false};
    notify_on_exit.fire = true;
    auto now = std::chrono::steady_clock::now();
    double elapsed_min =
        std::chrono::duration<double>(now - b.last).count() / 60.0;
    b.last = now;
    if (elapsed_min <= 0) return;
    b.tokens += elapsed_min * rate_per_min_;
    if (b.tokens > burst_) b.tokens = burst_;
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::map<std::string, Bucket> buckets_;
  int rate_per_min_ = 30;                      ///< 0=off
  double burst_ = 10.0;                        ///< 桶容量
};

}  // namespace thin_agent
