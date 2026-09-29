#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "thin_agent/agent/EmbeddingProvider.h"
#include "thin_agent/agent/VectorStore.h"

namespace thin_agent {
namespace agent {

/// 记忆管理器：自动从对话中抽取记忆、语义搜索、构建 LLM 上下文。
///
/// 用法：
///   auto embedding = std::make_shared<LocalHashEmbeddingProvider>(256);
///   MemoryManager mgr(embedding, "data/agent_memory.db");
///   mgr.ingest_conversation("sess-1", "用户说了什么", "助手回复了什么");
///   auto ctx = mgr.build_context("查天气", 3);
class MemoryManager {
 public:
  /// @param embedding  嵌入提供者
  /// @param db_path    VectorStore 数据库路径
  /// @param auto_extract  是否自动从对话中提取记忆（默认 true）
  MemoryManager(std::shared_ptr<EmbeddingProvider> embedding,
                const std::string& db_path,
                bool auto_extract = true);

  /// 摄取一条原始对话：自动提取关键信息并存入 VectorStore。
  /// @param session_id           会话 ID
  /// @param user_message         用户消息
  /// @param assistant_response   助手回复
  void ingest_conversation(const std::string& session_id,
                          const std::string& user_message,
                          const std::string& assistant_response);

  /// 手动存入一条记忆（不经过自动提取）。
  int64_t remember(const std::string& content,
                   const std::string& source = "fact",
                   const std::string& session_id = "",
                   float importance = 1.0f);

  /// 语义搜索相关记忆。
  std::vector<SearchResult> recall(const std::string& query, int top_k = 5);

  /// 构建注入 LLM prompt 的记忆上下文文本。
  /// @param query  搜索查询
  /// @param top_k  返回条数
  std::string build_context(const std::string& query, int top_k = 5);

  /// 获取最近 N 条记忆。
  std::vector<MemoryEntry> recent(int limit = 10);

  /// 获取统计信息。
  nlohmann::json stats() const;

  /// 清空指定会话的所有记忆。
  int clear_session(const std::string& session_id);

 private:
  /// 从对话文本中提取简洁的记忆条目。
  /// 简单策略：按句号/换行拆分 + 过滤太短/太长 + 重要性标记。
  std::vector<std::string> extract_facts(const std::string& user_message,
                                         const std::string& assistant_response);

  std::shared_ptr<EmbeddingProvider> embedding_;
  VectorStore store_;
  bool auto_extract_;
};

}  // namespace agent
}  // namespace thin_agent
