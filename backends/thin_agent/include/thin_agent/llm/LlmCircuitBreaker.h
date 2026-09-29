#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <nlohmann/json.hpp>
#include <random>   // v0.54.16: jitter 改用 std::mt19937（原 rand_r 是 POSIX 名）
#include <utility>
#include <vector>
#include <mutex>
#include <string>

namespace thin_agent {

/// v0.52.6: LLM 端点熔断器（进程级单例，per-endpoint 隔离）。
///
/// 背景（三晚 GLM 间歇超时风暴实测）：固定 1/2/4s 重试在端点故障期
/// 间每次请求烧满超时时长×3 重试+全部 worker 同步撞车；端点恢复后
/// 积压请求又瞬时涌入。熔断器在连续 transient 失败后快速失败
///（circuit_open 秒拒），冷却后半开探测，成功才恢复流量。
///
/// 状态机：closed →连续 kThreshold 次 transient 失败→ open（冷却
/// kCooldownMs）→ 半开（放行 1 发探测）→ 成功=closed / 失败=open
///（冷却指数退避至 kMaxCooldownMs）。
///
/// 4xx 语义错误（余额 1113/鉴权等）不计入：重试无意义，但也不应
/// 触发冷却（服务本身没故障）——由调用方直接失败。
class LlmCircuitBreaker {
 public:
  static LlmCircuitBreaker& instance() {
    static LlmCircuitBreaker cb;
    return cb;
  }

  /// 请求前调用：true=放行，false=熔断开（快速失败）。
  bool allow_request(const std::string& endpoint) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& s = states_[endpoint];
    auto now = std::chrono::steady_clock::now();
    if (s.open) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - s.opened_at).count();
      if (elapsed >= s.cooldown_ms) {
        // 冷却期满 → 半开：放行这一发探测
        s.open = false;
        s.half_open = true;
        return true;
      }
      return false;
    }
    return true;
  }

  /// 结果上报：ok=true 计数清零（半开探测成功 → 关闸恢复）；
  /// transient 失败（网络/5xx/超时）连续达阈值 → 开闸。
  /// 4xx 语义错误由调用方判断后不上报（不触发熔断）。
  void report(const std::string& endpoint, bool ok) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& s = states_[endpoint];
    if (ok) {
      // 成功即全复位（半开探测成功关闸；管理性上报成功也关闸）
      s.open = false;
      s.half_open = false;
      s.consecutive_failures = 0;
      s.cooldown_ms = kCooldownMs;
      return;
    }
    if (++s.consecutive_failures >= kThreshold) {
      s.open = true;
      s.half_open = false;
      s.opened_at = std::chrono::steady_clock::now();
      // 指数退避：30s → 60s → 120s 封顶
      s.cooldown_ms = std::min(s.cooldown_ms * 2, kMaxCooldownMs);
    }
  }

  /// 熔断状态查询（诊断/日志用）。
  bool is_open(const std::string& endpoint) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = states_.find(endpoint);
    return it != states_.end() && it->second.open;
  }

  /// 测试用：清零（单测间隔离）。
  void reset() {
    std::lock_guard<std::mutex> lk(mu_);
    states_.clear();
  }

  /// v0.53.25: 全量快照（/stats 与实时状态卡数据源）。返回
  /// [{endpoint, open, half_open, consecutive_failures,
  ///   cooldown_remaining_ms}]——只读诊断，不改变状态。
  std::vector<std::pair<std::string, nlohmann::json>> snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::pair<std::string, nlohmann::json>> out;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [ep, s] : states_) {
      long remaining = 0;
      if (s.open) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now - s.opened_at).count();
        remaining = s.cooldown_ms - static_cast<long>(elapsed);
        if (remaining < 0) remaining = 0;
      }
      out.push_back({ep, nlohmann::json{
                             {"open", s.open},
                             {"half_open", s.half_open},
                             {"consecutive_failures", s.consecutive_failures},
                                 {"cooldown_remaining_ms", remaining},
                             {"cooldown_ms", s.cooldown_ms}}});
    }
    return out;
  }

  static constexpr int kThreshold = 3;          ///< 连续 transient 失败阈值
  static constexpr long kCooldownMs = 30000;    ///< 初始冷却 30s
  static constexpr long kMaxCooldownMs = 120000;  ///< 冷却封顶 2min

 private:
  struct State {
    bool open{false};
    bool half_open{false};
    int consecutive_failures{0};
    long cooldown_ms{kCooldownMs};
    std::chrono::steady_clock::time_point opened_at{};
  };
  mutable std::mutex mu_;
  std::map<std::string, State> states_;
};

/// v0.52.6: 重试延迟 jitter——基准延迟 ±30% 随机（打散并发同步撞车）。
/// 确定性注入：测试可用 THIN_AGENT_TEST_NO_JITTER=1 关闭（复现旧固定
/// 步进行为）。
inline long jittered_delay_ms(long base_ms) {
  if (std::getenv("THIN_AGENT_TEST_NO_JITTER")) return base_ms;
  static thread_local unsigned seed = static_cast<unsigned>(
      std::chrono::steady_clock::now().time_since_epoch().count() &
      0xffffffff);
  long spread = base_ms * 30 / 100;
  if (spread <= 0) return base_ms;
  // v0.54.16: 原实现用 `rand_r(&seed)`——**POSIX 专属**（MSVC/MinGW 都没有该符号 ⇒ Windows
  // 侧整个 TU 编不过，且本头被多个 TU 包含）。改用标准库 `thread_local` 引擎：与 rand_r 的
  // "每线程独立、免锁、种子各不相同"意图等价，且跨平台（含 `<random>`）。
  static thread_local std::mt19937 jitter_rng(seed);
  std::uniform_int_distribution<long> jitter_dist(0, 2 * spread);
  return base_ms - spread + jitter_dist(jitter_rng);
}

}  // namespace thin_agent
