#pragma once

#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace thin_agent {
namespace agent {

/// 错误修正记录。
struct CorrectionEntry {
  std::string tool_name;
  std::string error_pattern;
  std::string fix_hint;
  int use_count{0};
  std::string created_at;
};

/// 跨会话学习存储：持久化工具调用失败→成功修正的映射。
/// Agent 从失败中学习，下次遇到相同错误自动避开。
///
/// 用法：
///   ErrorCorrectionStore store("corrections.db");
///   store.record("file_read", "permission denied", "use sudo before reading");
///   // 下次 AgentLoop 中注入：
///   auto hints = store.to_prompt_injection({"file_read", "terminal"});
class ErrorCorrectionStore {
 public:
  ErrorCorrectionStore();
  explicit ErrorCorrectionStore(const std::string& db_path);
  ~ErrorCorrectionStore();

  ErrorCorrectionStore(const ErrorCorrectionStore&) = delete;
  ErrorCorrectionStore& operator=(const ErrorCorrectionStore&) = delete;

  /// 记录一次修正。
  /// @param tool_name     工具名
  /// @param error_pattern 错误特征（如 "HTTP 500"、"permission denied"）
  /// @param fix_hint      修正提示（如 "check file permissions"）
  /// @return 修正 ID（用于计数更新）
  int64_t record(const std::string& tool_name,
                 const std::string& error_pattern,
                 const std::string& fix_hint);

  /// 修正成功后增加使用计数。
  void increment_use(int64_t correction_id);

  /// 获取某工具的历史修正（按使用次数降序）。
  std::vector<CorrectionEntry> get_corrections(const std::string& tool_name,
                                                int limit = 5) const;

  /// 根据错误特征查找匹配的修正提示。
  std::vector<CorrectionEntry> find_by_error(const std::string& tool_name,
                                              const std::string& error_snippet,
                                              int limit = 3) const;

  /// 将指定工具的修正拼接为 system prompt 注入文本。
  /// 只注入 use_count >= 2 的高频修正。
  std::string to_prompt_injection(const std::vector<std::string>& tool_names,
                                   int max_per_tool = 3) const;

  /// 统计信息。
  nlohmann::json stats() const;

 private:
  void ensure_db();
  static std::string now_iso();

  std::string db_path_;
  sqlite3* db_{nullptr};
  mutable std::mutex mu_;
};

}  // namespace agent
}  // namespace thin_agent
