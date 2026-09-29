#pragma once

#include <string>
#include <vector>

#include "thin_agent/llm/CloudLlmClient.h"

namespace thin_agent {

/// Token 预算追踪器 — 估算消息的 token 数并追踪上下文窗口使用率。
///
/// 使用启发式估算（无损不依赖外部库）：
///   - CJK 字符（中文/日文/韩文）：~2 字符 / token
///   - ASCII 字符（英文/代码）：  ~4 字符 / token
///   - DeepSeek V4 Pro 默认上下文窗口：128K tokens
///
/// 示例：
///   TokenBudget budget(128000);
///   budget.add_system(4096);            // system prompt ≈ 4K tokens
///   budget.add_messages(conversation);  // 当前会话
///   if (budget.usage_pct() > 0.70f) {
///     // 压缩旧工具输出...
///   }
struct TokenBudget {
  int context_window;      ///< 总上下文窗口（tokens），默认 128000
  int system_tokens = 0;   ///< system prompt 的 token 估算值
  int msg_tokens  = 0;     ///< 当前 messages 数组的 token 估算值
  int total_api_calls = 0; ///< API 调用次数

  explicit TokenBudget(int window = 128000) : context_window(window) {}

  // ── 估算 ──────────────────────────────────────────
  /// 估算单段文本的 token 数。
  static int estimate_text(const std::string& text);

  /// 估算一条 ChatMessage 的 token 数（含 role/content/tool_calls JSON 开销）。
  static int estimate_message(const ChatMessage& msg);

  /// 估算 messages 数组的总 token 数。
  static int estimate_messages(const std::vector<ChatMessage>& msgs);

  // ── 状态查询 ──────────────────────────────────────
  int  total()           const { return system_tokens + msg_tokens; }
  int  remaining()       const { return context_window - total(); }
  float usage_pct()      const { return context_window > 0
                                  ? (float)total() / (float)context_window
                                  : 0.0f; }
  bool is_critical()     const { return usage_pct() > 0.85f; }  // >85%
  bool is_high()         const { return usage_pct() > 0.70f; }  // >70%
  bool is_comfortable()  const { return usage_pct() <= 0.50f; } // ≤50%

  /// 生成人类可读的状态摘要。
  std::string status_line() const;

  // ── 操作 ──────────────────────────────────────────
  void set_system(int tokens)   { system_tokens = tokens; }
  void recalc(const std::vector<ChatMessage>& msgs);
  void add_call()               { total_api_calls++; }
};

}  // namespace thin_agent
