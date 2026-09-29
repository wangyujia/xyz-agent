#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace thin_agent {

/// FC 循环收敛追踪器 — 检测进度 vs 循环。
///
/// 核心信号：
///   - novelty_ratio：新颖工具调用占比（<20% 可能兜圈子）
///   - repeat_streak：连续重复调用次数（≥3 → 循环警报）
///   - error_rate：失败工具调用占比（>50% → 路径错误）
///
/// 示例：
///   ConvergenceTracker ct;
///   for (auto& tc : tool_calls) {
///     ct.record(tc.name, tc.args_summary, tc.ok);
///   }
///   if (ct.is_looping()) { ... 警告 LLM 或提前终止 ... }
struct ConvergenceTracker {
  int total_calls  = 0;   ///< 总工具调用次数
  int novel_calls  = 0;   ///< 使用了新签名的调用次数
  int error_calls  = 0;   ///< 工具执行失败次数
  int repeat_streak = 0;  ///< 当前连续使用已见签名的次数
  int max_repeat_streak = 0;  ///< 历史最长的重复连续

  // ── 追踪 ──────────────────────────────────────────
  /// 记录一次工具调用。
  /// @param tool_name    工具名称
  /// @param args_sig     参数签名（取前 200 字符，避免完整参数）
  /// @param ok           工具执行是否成功
  void record(const std::string& tool_name,
              const std::string& args_sig,
              bool ok);

  // ── 信号 ──────────────────────────────────────────
  /// 工具调用中有多少比例是新的？
  float novelty_ratio() const {
    return total_calls > 0
           ? (float)novel_calls / (float)total_calls
           : 1.0f;
  }

  /// 工具调用失败率。
  float error_rate() const {
    return total_calls > 0
           ? (float)error_calls / (float)total_calls
           : 0.0f;
  }

  // ── 诊断 ──────────────────────────────────────────
  /// 当前是否在循环中？（连续重复 ≥ 2 次 — 降低阈值以更快检测）
  bool is_looping() const { return repeat_streak >= 2; }

  /// 严重循环 — 连续重复 ≥ 3 次，应强制终止
  bool is_severe_loop() const { return repeat_streak >= 3; }

  /// 是否陷入新颖性枯竭？（≥5 轮 且 新颖率 < 0.2）
  bool is_stalled() const {
    return total_calls >= 5 && novelty_ratio() < 0.20f;
  }

  /// 是否方向错误？（失败率 > 50% 且 ≥3 次调用）
  bool is_wrong_direction() const {
    return total_calls >= 3 && error_rate() > 0.50f;
  }

  // ── 压力信号 ──────────────────────────────────────
  /// 生成可注入 system prompt 的状态总结（中文）。
  /// 仅在有问题时非空——避免无意义的噪声注入。
  std::string pressure_hint() const;

  /// 人类可读状态行（调试日志用）。
  std::string status_line() const;

 private:
  std::set<std::string> seen_sigs_;           ///< 已见签名集合
  std::map<std::string, int> tool_freq_;      ///< 各工具调用频次
};

}  // namespace thin_agent
