#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {
namespace agent {

/// 记忆条目。
struct MemoryEntry {
  int64_t id = 0;
  std::string content;       ///< 记忆文本内容
  std::string source;        ///< 来源类型：conversation / fact / preference
  std::string session_id;    ///< 关联会话 ID
  std::string created_at;    ///< ISO8601 创建时间
  float importance = 1.0f;   ///< 重要性权重（0~1）

  nlohmann::json to_json() const {
    return {{"id", id},
             {"content", content},
             {"source", source},
             {"session_id", session_id},
             {"created_at", created_at},
             {"importance", importance}};
  }
};

/// 向量搜索结果。
struct SearchResult {
  MemoryEntry entry;
  float similarity = 0.0f;  ///< 余弦相似度（-1 ~ 1）
};

/// 基于 SQLite 的向量存储：存储记忆文本 + 浮点嵌入向量，支持余弦相似度搜索。
///
/// 表结构：
///   CREATE TABLE memories (
///     id INTEGER PRIMARY KEY AUTOINCREMENT,
///     content TEXT NOT NULL,
///     source TEXT NOT NULL DEFAULT 'conversation',
///     session_id TEXT NOT NULL DEFAULT '',
///     created_at TEXT NOT NULL,
///     importance REAL NOT NULL DEFAULT 1.0,
///     embedding BLOB NOT NULL
///   );
///
/// 搜索策略：全表扫描 + 手动计算余弦相似度（适合 < 100K 条场景）。
class VectorStore {
 public:
  VectorStore();
  ~VectorStore();

  /// 打开/创建 SQLite 数据库并建表。
  /// @param db_path  数据库文件路径
  /// @param dim      嵌入向量维度
  bool init(const std::string& db_path, int dim);

  /// 获取嵌入维度。
  int dimension() const { return dim_; }

  /// 插入一条记忆（含嵌入向量）。返回新行的 id。
  int64_t insert(const MemoryEntry& entry, const std::vector<float>& embedding);

  /// 相似度搜索：返回 top_k 个最相似的记忆。
  /// @param query_embedding  查询文本的嵌入向量
  /// @param top_k            返回条数
  /// @param min_similarity   最低相似度阈值
  std::vector<SearchResult> search(const std::vector<float>& query_embedding,
                                   int top_k = 5,
                                   float min_similarity = 0.3f);

  /// 按 ID 删除记忆。
  bool remove(int64_t id);

  /// 按会话 ID 删除所有记忆。
  int remove_by_session(const std::string& session_id);

  /// 返回记忆总数。
  int64_t count() const;

  /// 获取最近 N 条记忆（按插入顺序倒序）。
  std::vector<MemoryEntry> recent(int limit = 20);

  /// 按来源统计记忆数量。
  nlohmann::json stats() const;

 private:
  std::string now_iso() const;

  /// float[] → BLOB 序列化。
  static std::string serialize_embedding(const std::vector<float>& vec);
  /// BLOB → float[] 反序列化。
  static std::vector<float> deserialize_embedding(const void* blob, int len,
                                                   int dim);

  void* db_{nullptr};  ///< sqlite3*
  // v0.53.41: sqlite3 单连接非线程安全——4 worker 并发 ingest/recall 会
  // interleaved step(MISUSE/崩溃)。WAL 只管多连接不管单连接并发。
  std::mutex mu_;
  int dim_{0};
};

}  // namespace agent
}  // namespace thin_agent
