#include "thin_agent/agent/ErrorCorrectionStore.h"

#include <sqlite3.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace thin_agent {
namespace agent {

ErrorCorrectionStore::ErrorCorrectionStore() {
  ensure_db();
}

ErrorCorrectionStore::ErrorCorrectionStore(const std::string& db_path)
    : db_path_(db_path) {
  ensure_db();
}

ErrorCorrectionStore::~ErrorCorrectionStore() {
  if (db_) sqlite3_close(db_);
}

void ErrorCorrectionStore::ensure_db() {
  if (db_) return;

  int rc;
  if (db_path_.empty()) {
    rc = sqlite3_open(":memory:", &db_);
  } else {
    rc = sqlite3_open(db_path_.c_str(), &db_);
  }
  if (rc != SQLITE_OK) return;

  const char* sql = R"(
    CREATE TABLE IF NOT EXISTS corrections (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      tool_name TEXT NOT NULL,
      error_pattern TEXT NOT NULL,
      fix_hint TEXT NOT NULL,
      use_count INTEGER NOT NULL DEFAULT 1,
      created_at TEXT NOT NULL
    );
    CREATE UNIQUE INDEX IF NOT EXISTS idx_corrections_unique
      ON corrections(tool_name, error_pattern);
  )";
  sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
}

std::string ErrorCorrectionStore::now_iso() {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream oss;
  oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

int64_t ErrorCorrectionStore::record(const std::string& tool_name,
                                      const std::string& error_pattern,
                                      const std::string& fix_hint) {
  std::lock_guard<std::mutex> lk(mu_);
  ensure_db();
  if (!db_) return -1;

  std::string ts = now_iso();

  // UPSERT: 如果相同 tool+error 已存在，增加 use_count
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO corrections (tool_name, error_pattern, fix_hint, use_count, created_at) "
      "VALUES (?, ?, ?, 1, ?) "
      "ON CONFLICT(tool_name, error_pattern) DO UPDATE SET "
      "use_count = use_count + 1, fix_hint = excluded.fix_hint;";

  sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, tool_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, error_pattern.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, fix_hint.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, ts.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return sqlite3_last_insert_rowid(db_);
}

void ErrorCorrectionStore::increment_use(int64_t correction_id) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!db_ || correction_id <= 0) return;

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_,
      "UPDATE corrections SET use_count = use_count + 1 WHERE id = ?;",
      -1, &stmt, nullptr);
  sqlite3_bind_int64(stmt, 1, correction_id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

std::vector<CorrectionEntry> ErrorCorrectionStore::get_corrections(
    const std::string& tool_name, int limit) const {
  std::lock_guard<std::mutex> lk(mu_);
  if (!db_) return {};

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_,
      "SELECT id, tool_name, error_pattern, fix_hint, use_count, created_at "
      "FROM corrections WHERE tool_name = ? ORDER BY use_count DESC LIMIT ?;",
      -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, tool_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, limit);

  std::vector<CorrectionEntry> results;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    CorrectionEntry e;
    e.tool_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    e.error_pattern = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    e.fix_hint = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    e.use_count = sqlite3_column_int(stmt, 4);
    e.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    results.push_back(e);
  }
  sqlite3_finalize(stmt);
  return results;
}

std::vector<CorrectionEntry> ErrorCorrectionStore::find_by_error(
    const std::string& tool_name,
    const std::string& error_snippet,
    int limit) const {
  std::lock_guard<std::mutex> lk(mu_);
  if (!db_) return {};

  // LIKE 匹配：error_pattern 包含给定的 snippet
  std::string pattern = "%" + error_snippet + "%";
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_,
      "SELECT id, tool_name, error_pattern, fix_hint, use_count, created_at "
      "FROM corrections WHERE tool_name = ? AND error_pattern LIKE ? "
      "ORDER BY use_count DESC LIMIT ?;",
      -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, tool_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, limit);

  std::vector<CorrectionEntry> results;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    CorrectionEntry e;
    e.tool_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    e.error_pattern = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    e.fix_hint = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    e.use_count = sqlite3_column_int(stmt, 4);
    e.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    results.push_back(e);
  }
  sqlite3_finalize(stmt);
  return results;
}

std::string ErrorCorrectionStore::to_prompt_injection(
    const std::vector<std::string>& tool_names,
    int max_per_tool) const {
  std::ostringstream ss;
  bool has_any = false;

  for (const auto& tn : tool_names) {
    auto corrections = get_corrections(tn, max_per_tool);
    // 只注入高频修正（>=2 次）
    for (const auto& c : corrections) {
      if (c.use_count < 2) continue;
      if (!has_any) {
        ss << "\n[Learned Error Corrections from Past Sessions]\n";
        has_any = true;
      }
      ss << "- " << tn << ": when seeing \"" << c.error_pattern
         << "\" → " << c.fix_hint
         << " (used " << c.use_count << "×)\n";
    }
  }

  if (has_any) ss << "[/Corrections]\n";
  return ss.str();
}

nlohmann::json ErrorCorrectionStore::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  if (!db_) return {{"total", 0}};

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_,
      "SELECT COUNT(*), SUM(use_count) FROM corrections;",
      -1, &stmt, nullptr);
  int total = 0, total_uses = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    total = sqlite3_column_int(stmt, 0);
    total_uses = sqlite3_column_int(stmt, 1);
  }
  sqlite3_finalize(stmt);
  return {{"total_corrections", total}, {"total_uses", total_uses}};
}

}  // namespace agent
}  // namespace thin_agent
