#pragma once

#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace thin_agent {

/// FTS5 全文搜索 codebase.db 知识库。
///
/// 用法：
///   KbSearcher kb;
///   kb.open("/path/to/codebase.db");
///   auto results = kb.search("segfault camera_init", 10);
///
/// v0.27.0: 支持多 KB——通过 kb_name 查找 kb_index.json 定位 DB。
class KbSearcher {
 public:
  KbSearcher();
  explicit KbSearcher(const std::string& db_path);
  ~KbSearcher();

  KbSearcher(const KbSearcher&) = delete;
  KbSearcher& operator=(const KbSearcher&) = delete;

  /// 打开 KB 数据库。返回 true 如果成功。
  bool open(const std::string& db_path);

  /// v0.27.0: 按 kb 名打开——查 kb_index.json 获取实际 .db 路径。
  /// kb_name="codebase" → ~/.thin_agent/kb/codebase.db
  ///   如果 kb_index.json 中存在，用注册路径；否则回退到 {kb_dir}/{kb_name}.db
  bool open_by_name(const std::string& kb_name);

  /// 全文搜索。query 支持 FTS5 标准语法。
  /// @param query 搜索词
  /// @param limit 最大结果数
  /// @return 匹配结果数组
  nlohmann::json search(const std::string& query, int limit = 10);

  /// 返回索引文档总数。
  int doc_count() const;

  /// 数据库是否已打开。
  bool is_open() const { return db_ != nullptr; }

 private:
  sqlite3* db_{nullptr};
  mutable std::mutex mu_;
};

/// v0.27.0: 根据 kb_name 解析 .db 路径。
/// 先查 kb_index.json 注册表，找不到则按 {kb_dir}/{kb_name}.db 回退。
std::string resolve_kb_db_path(const std::string& kb_name,
                                const std::string& kb_dir = "",
                                const std::string& index_path = "");

}  // namespace thin_agent
