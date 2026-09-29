#include "thin_agent/core/KbSearcher.h"
#include "thin_agent/RuntimePaths.h"

#include <sqlite3.h>

#include <cstring>
#include <fstream>
#include <sstream>

namespace thin_agent {

// ── v0.27.0: kb_index.json 路径解析 ──

std::string resolve_kb_db_path(const std::string& kb_name,
                                const std::string& kb_dir,
                                const std::string& index_path) {
  using namespace nlohmann;

  std::string dir = kb_dir.empty() ? default_kb_dir() : kb_dir;
  std::string idx = index_path.empty() ? default_kb_index_path() : index_path;

  // 1. 先查 kb_index.json
  std::ifstream ifs(idx);
  if (ifs.good()) {
    try {
      auto j = json::parse(ifs);
      if (j.contains("kbs") && j["kbs"].contains(kb_name)) {
        auto& info = j["kbs"][kb_name];
        std::string path = info.value("path", "");
        if (!path.empty()) {
          // 相对路径 → 补齐 kb_dir
          if (path[0] != '/') {
            return dir + "/" + path;
          }
          return path;
        }
      }
    } catch (...) {
      // parse error → fall through to default
    }
  }

  // 2. 回退: {kb_dir}/{kb_name}.db
  return dir + "/" + kb_name + ".db";
}

KbSearcher::KbSearcher() = default;

KbSearcher::KbSearcher(const std::string& db_path) {
  open(db_path);
}

KbSearcher::~KbSearcher() {
  if (db_) sqlite3_close(db_);
}

bool KbSearcher::open(const std::string& db_path) {
  std::lock_guard<std::mutex> lk(mu_);
  if (db_) sqlite3_close(db_);
  db_ = nullptr;
  int rc = sqlite3_open(db_path.c_str(), &db_);
  if (rc != SQLITE_OK) {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
    return false;
  }
  // v0.27.1: WAL 模式 — 与 KbIndexer 读写并发安全
  sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
  sqlite3_extended_result_codes(db_, 1);
  return true;
}

bool KbSearcher::open_by_name(const std::string& kb_name) {
  std::string db_path = resolve_kb_db_path(kb_name);
  return open(db_path);
}

int KbSearcher::doc_count() const {
  std::lock_guard<std::mutex> lk(mu_);
  if (!db_) return 0;
  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_,
      "SELECT COUNT(*) FROM codebase", -1, &stmt, nullptr);
  if (rc != SQLITE_OK) return 0;
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

nlohmann::json KbSearcher::search(const std::string& query, int limit) {
  std::lock_guard<std::mutex> lk(mu_);

  nlohmann::json result;
  result["query"] = query;
  result["matches"] = 0;
  result["results"] = nlohmann::json::array();

  if (!db_) {
    result["error"] = "KB database not open";
    return result;
  }
  if (query.empty()) {
    result["error"] = "empty query";
    return result;
  }
  if (limit <= 0) limit = 10;
  if (limit > 50) limit = 50;  // cap to prevent excessive output

  // FTS5 MATCH query with rank ordering
  // Escaping: double any double-quote in user query for SQL safety
  std::string escaped = query;
  for (size_t pos = 0; (pos = escaped.find('"', pos)) != std::string::npos; pos += 2) {
    escaped.insert(pos, "\"");
  }

  // v0.27.1: 兼容新旧 schema（新表有 repo 列，旧 build_kb.py 产物没有）
  bool has_repo = true;
  const char* sql_new = R"(
    SELECT repo, path, filename, extension, symbols,
           substr(content, 1, 300)
    FROM codebase
    WHERE codebase MATCH ?
    ORDER BY rank
    LIMIT ?
  )";
  const char* sql_old = R"(
    SELECT path, filename, extension, symbols,
           substr(content, 1, 300)
    FROM codebase
    WHERE codebase MATCH ?
    ORDER BY rank
    LIMIT ?
  )";

  const char* like_sql_new = R"(
    SELECT repo, path, filename, extension, symbols,
           substr(content, 1, 300)
    FROM codebase
    WHERE content LIKE ?1 OR symbols LIKE ?1
    LIMIT ?2
  )";
  const char* like_sql_old = R"(
    SELECT path, filename, extension, symbols,
           substr(content, 1, 300)
    FROM codebase
    WHERE content LIKE ?1 OR symbols LIKE ?1
    LIMIT ?2
  )";

  sqlite3_stmt* stmt = nullptr;
  bool used_fts = false;  // v0.53.55: FTS 路径标志(step 错时降级 LIKE)
  int rc = sqlite3_prepare_v2(db_, sql_new, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    // 可能是旧 schema（无 repo 列），回退
    has_repo = false;
    sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(db_, sql_old, -1, &stmt, nullptr);
  }
  if (rc != SQLITE_OK) {
    // 旧 schema 也失败 → LIKE fallback
    has_repo = true;  // 重新尝试新 schema
    sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(db_, like_sql_new, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
      has_repo = false;
      sqlite3_finalize(stmt);
      rc = sqlite3_prepare_v2(db_, like_sql_old, -1, &stmt, nullptr);
    }
    if (rc != SQLITE_OK) {
      result["error"] = "query preparation failed";
      return result;
    }
    std::string like_pattern = "%" + query + "%";
    sqlite3_bind_text(stmt, 1, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
  } else {
    // v0.53.55: 短语包裹(只转义双引号不够)——"std::vector"/NEAR/
    /// -x 等符号查询触发 fts5 语法错→空结果;包成 "..." 短语后
    /// 整串匹配;内部双引号已翻倍(escaped)
    used_fts = true;  // v0.53.55: 走 FTS(供 step 错降级判断)
    const std::string phrase = "\"" + escaped + "\"";
    sqlite3_bind_text(stmt, 1, phrase.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
  }

  auto& results = result["results"];
  int step_rc = sqlite3_step(stmt);
  if ((step_rc == SQLITE_ERROR || step_rc == SQLITE_MISUSE) && used_fts) {
    // v0.53.55: FTS 语法错(转义未覆盖的极端串)→降级 LIKE 兜底
    sqlite3_finalize(stmt);
    const char* fb = has_repo ? like_sql_new : like_sql_old;
    if (sqlite3_prepare_v2(db_, fb, -1, &stmt, nullptr) == SQLITE_OK) {
      std::string like_pattern = "%" + query + "%";
      sqlite3_bind_text(stmt, 1, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 2, limit);
      step_rc = sqlite3_step(stmt);
    }
  }
  while (step_rc == SQLITE_ROW) {
    nlohmann::json doc;
    if (has_repo) {
      doc["repo"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) ?: "";
      doc["path"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)) ?: "";
      doc["filename"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)) ?: "";
      doc["extension"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)) ?: "";
      doc["symbols"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)) ?: "";
      doc["snippet"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)) ?: "";
    } else {
      doc["repo"] = "";
      doc["path"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) ?: "";
      doc["filename"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)) ?: "";
      doc["extension"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)) ?: "";
      doc["symbols"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)) ?: "";
      doc["snippet"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)) ?: "";
    }
    results.push_back(std::move(doc));
    step_rc = sqlite3_step(stmt);
  }

  sqlite3_finalize(stmt);

  result["matches"] = results.size();
  return result;
}

}  // namespace thin_agent
