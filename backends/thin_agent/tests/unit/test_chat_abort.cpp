// unit_chat_abort：流式中断标志逻辑测试
// 验证 v0.47.3 的核心不变量：
//   1. abort_chat(session) 设置标志 → check_and_clear_abort 返回 true
//   2. check 后标志自动清除（不残留）
//   3. 未 abort 的 session → check 返回 false
//   4. 多 session 隔离 — abort A 不影响 B

// 本测试直接复刻 abort_sessions_ + check_and_clear_abort 逻辑（不依赖
// AgentService 完整初始化），验证 set/check/clear/isolation 语义。

#include <cassert>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_set>
#include "test_macros.h"

// 复刻 AgentService 的 abort 逻辑
class AbortFlagTracker {
 public:
  void abort(const std::string& sid) {
    std::lock_guard<std::mutex> lk(mu_);
    aborted_.insert(sid);
  }
  bool check_and_clear(const std::string& sid) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = aborted_.find(sid);
    if (it != aborted_.end()) {
      aborted_.erase(it);
      return true;
    }
    return false;
  }
 private:
  std::mutex mu_;
  std::unordered_set<std::string> aborted_;
};

int main() {
  AbortFlagTracker tracker;

  // ── 测试 1: 基本设置 + 检查 + 自动清除 ──
  {
    tracker.abort("session-A");
    ASSERT_TRUE("set_then_check_true", tracker.check_and_clear("session-A"));

    // 再次检查应该 false（已清除）
    ASSERT_TRUE("check_again_false", !tracker.check_and_clear("session-A"));
  }

  // ── 测试 2: 未 abort 的 session ──
  {
    ASSERT_TRUE("unaborted_false", !tracker.check_and_clear("never-aborted"));
  }

  // ── 测试 3: 多 session 隔离 ──
  {
    tracker.abort("sess-X");
    // Y 未被 abort
    ASSERT_TRUE("isolation_Y_false", !tracker.check_and_clear("sess-Y"));
    // X 被 abort
    ASSERT_TRUE("isolation_X_true", tracker.check_and_clear("sess-X"));
    // Y 仍然 false
    ASSERT_TRUE("isolation_Y_still_false", !tracker.check_and_clear("sess-Y"));
  }

  // ── 测试 4: 重复 abort 同一 session ──
  {
    tracker.abort("sess-Z");
    tracker.abort("sess-Z");  // 重复
    // 只检查一次返回 true（集合去重）
    ASSERT_TRUE("duplicate_first_check", tracker.check_and_clear("sess-Z"));
    ASSERT_TRUE("duplicate_second_check", !tracker.check_and_clear("sess-Z"));
  }

  // ── 测试 5: 模拟 FC 循环中断场景 ──
  // 模拟 FC 循环每轮检查 abort：第 3 轮被中断
  {
    AbortFlagTracker fc_tracker;
    const std::string sid = "fc-test-session";
    int iter_completed = 0;
    int max_iter = 10;

    for (int iter = 0; iter < max_iter; ++iter) {
      // 模拟第 3 轮时用户发 abort
      if (iter == 3) {
        fc_tracker.abort(sid);
      }
      // FC 循环 iter 开始检查中断
      if (fc_tracker.check_and_clear(sid)) {
        break;  // 中断退出
      }
      iter_completed = iter + 1;
    }

    ASSERT_TRUE("fc_interrupted_at_3", iter_completed == 3);
  }

  TEST_REPORT();
}
