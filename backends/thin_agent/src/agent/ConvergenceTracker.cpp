#include "thin_agent/agent/ConvergenceTracker.h"

#include <algorithm>
#include <sstream>

namespace thin_agent {

void ConvergenceTracker::record(const std::string& tool_name,
                                 const std::string& args_sig,
                                 bool ok) {
  total_calls++;
  if (!ok) error_calls++;

  // 构造签名：tool_name + ":" + args_sig（截断前 200 字符）
  std::string sig = tool_name + ":" + args_sig.substr(0, 200);

  tool_freq_[tool_name]++;

  if (seen_sigs_.count(sig) == 0) {
    // 新签名
    seen_sigs_.insert(sig);
    novel_calls++;
    repeat_streak = 0;
  } else {
    // 已见过的签名 → 重复
    repeat_streak++;
    if (repeat_streak > max_repeat_streak) {
      max_repeat_streak = repeat_streak;
    }
  }
}

std::string ConvergenceTracker::pressure_hint() const {
  if (total_calls == 0) return "";

  std::ostringstream oss;

  if (is_looping()) {
    oss << "⚠️ 你已连续 " << repeat_streak
        << " 次调用重复的工具签名。考虑换一种方法，或如果目标已达成则输出最终回答。\n";
  }

  if (is_stalled()) {
    oss << "📊 仅有 " << static_cast<int>(novelty_ratio() * 100)
        << "% 的操作为新操作（共 " << total_calls << " 次）。"
        << "任务可能已接近完成，请整合结果并输出最终回答。\n";
  }

  if (is_wrong_direction()) {
    oss << "❌ 工具执行失败率 " << static_cast<int>(error_rate() * 100)
        << "%（" << error_calls << "/" << total_calls << "）。"
        << "可能路径错误，请重新评估方法。\n";
  }

  return oss.str();
}

std::string ConvergenceTracker::status_line() const {
  std::ostringstream oss;
  oss << "[convergence] calls=" << total_calls
      << " novel=" << novel_calls
      << " (" << static_cast<int>(novelty_ratio() * 100) << "%)"
      << " errors=" << error_calls
      << " (" << static_cast<int>(error_rate() * 100) << "%)"
      << " repeat_streak=" << repeat_streak
      << " max_repeat=" << max_repeat_streak;
  if (is_looping()) oss << " LOOPING";
  if (is_stalled()) oss << " STALLED";
  if (is_wrong_direction()) oss << " WRONG_DIRECTION";
  return oss.str();
}

}  // namespace thin_agent
