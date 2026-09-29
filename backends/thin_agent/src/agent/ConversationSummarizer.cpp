#include "thin_agent/agent/ConversationSummarizer.h"

#include <sstream>

namespace thin_agent {
namespace agent {

ConversationSummarizer::ConversationSummarizer(int max_turns, int keep_recent,
                                               SummarizeCallback summarize_fn)
    : max_turns_(max_turns), keep_recent_(keep_recent),
      summarize_fn_(std::move(summarize_fn)) {}

void ConversationSummarizer::add_message(const std::string& role,
                                         const std::string& content) {
  messages_.push_back({role, content});
}

bool ConversationSummarizer::needs_summarize() const {
  return static_cast<int>(messages_.size()) > max_turns_;
}

std::string ConversationSummarizer::summary_prefix() const {
  if (cached_summary_.empty()) return "";
  return "[Conversation Summary]\n" + cached_summary_ + "\n[/Summary]\n";
}

std::string ConversationSummarizer::summarize() {
  if (!summarize_fn_ || messages_.size() <= static_cast<size_t>(keep_recent_)) {
    return cached_summary_;
  }

  // 构建 prompt：输入最近对话，输出摘要
  std::ostringstream prompt;
  prompt << "Please summarize the following conversation in 2-3 sentences. "
         << "Focus on key topics, decisions, and user preferences. "
         << "Respond with ONLY the summary, no preamble.\n\n";

  if (!cached_summary_.empty()) {
    prompt << "Previous summary: " << cached_summary_ << "\n\n";
  }

  int to_summarize = static_cast<int>(messages_.size()) - keep_recent_;
  for (int i = 0; i < to_summarize; ++i) {
    prompt << "[" << messages_[i].role << "]: " << messages_[i].content << "\n";
  }

  std::string summary = summarize_fn_(prompt.str());
  if (summary.empty()) return cached_summary_;

  // 替换缓存
  if (!cached_summary_.empty()) {
    cached_summary_ = summary;  // 覆盖旧摘要
  } else {
    cached_summary_ = summary;
  }

  // 删除已压缩的消息，只保留最近 keep_recent_ 条
  while (static_cast<int>(messages_.size()) > keep_recent_) {
    messages_.pop_front();
  }

  return cached_summary_;
}

void ConversationSummarizer::reset() {
  messages_.clear();
  cached_summary_.clear();
}

}  // namespace agent
}  // namespace thin_agent
