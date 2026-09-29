// v0.53.99 (R86): 网关/客户端**重连退避策略**——单点收口，纯策略无 IO 便于单测。
//
// 背景（实测）：im_gateway_v2 的 on_timer(3s) 里 `if (!g_agent_conn) agent_connect();`
// **无任何退避** → agent 长时间不可用（笔记本合盖/后端重启）时每 3 秒一次连接尝试，
// 永不收敛：日志被 attempts 刷屏、连接 churn 持续。
//
// 策略：3s → 6s → 12s → 24s → 48s → 60s(封顶)；**连接成功后 reset()** 回到 3s。
#pragma once

#include <algorithm>
#include <cstdint>

namespace thin_agent {

class ReconnectBackoff {
 public:
  static constexpr int64_t kBaseMs = 3000;
  static constexpr int64_t kCapMs = 60000;

  explicit ReconnectBackoff(int64_t base_ms = kBaseMs, int64_t cap_ms = kCapMs)
      : base_ms_(base_ms > 0 ? base_ms : kBaseMs),
        cap_ms_(cap_ms >= base_ms ? cap_ms : base_ms),
        next_ms_(base_ms_ > 0 ? base_ms_ : kBaseMs) {}

  /// 取"本次失败后应等待的时长"，并把档位推进到下一档（指数增长、封顶 cap）。
  int64_t next_delay_ms() {
    const int64_t d = next_ms_;
    next_ms_ = std::min(next_ms_ * 2, cap_ms_);
    return d;
  }

  /// 连接成功 → 复位到基准档（下次断线从 3s 重新开始）
  void reset() { next_ms_ = base_ms_; }

  /// 当前档位（供日志/可观测）
  int64_t pending_delay_ms() const { return next_ms_; }
  bool at_cap() const { return next_ms_ >= cap_ms_; }

 private:
  int64_t base_ms_;
  int64_t cap_ms_;
  int64_t next_ms_;
};

}  // namespace thin_agent
