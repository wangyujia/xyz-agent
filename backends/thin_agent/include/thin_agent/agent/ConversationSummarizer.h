#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace thin_agent {
namespace agent {

/// 对话上下文压缩器：当对话轮数超过阈值时，调用云端 LLM 生成摘要。
/// 摘要缓存为 system prompt 注入，避免长对话 token 爆炸。
class ConversationSummarizer {
 public:
  /// 摘要回调：summary 为生成的对话摘要文本。
  using SummarizeCallback = std::function<std::string(const std::string& prompt)>;

  /// @param max_turns      超过此行数触发压缩（默认 10 轮）
  /// @param keep_recent    保留最近 N 轮完整对话（默认 4 轮）
  /// @param summarize_fn   云端 LLM 调用函数
  ConversationSummarizer(int max_turns = 10, int keep_recent = 4,
                         SummarizeCallback summarize_fn = nullptr);

  /// 添加一轮对话消息（user 或 assistant）。
  void add_message(const std::string& role, const std::string& content);

  /// 在 system prompt 前应注入的摘要文本。空串 = 无需注入。
  std::string summary_prefix() const;

  /// 是否需要压缩。
  bool needs_summarize() const;

  /// 执行压缩（调用 summarize_fn），返回摘要文本。
  /// 压缩后将旧消息替换为摘要缓存，保留最近 keep_recent 条。
  std::string summarize();

  /// 清空所有对话记录与缓存。
  void reset();

  /// 当前缓存的对话消息数量。
  size_t message_count() const { return messages_.size(); }

 private:
  struct Message {
    std::string role;
    std::string content;
  };

  int max_turns_;
  int keep_recent_;
  SummarizeCallback summarize_fn_;
  std::deque<Message> messages_;
  std::string cached_summary_;
};

}  // namespace agent
}  // namespace thin_agent
