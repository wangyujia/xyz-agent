#include "thin_agent/agent/GoalManager.h"

#include <sqlite3.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace thin_agent {
namespace agent {

GoalManager::GoalManager(const std::string& db_path) : db_path_(db_path) {
  ensure_db();
}

GoalManager::~GoalManager() {
  if (db_) sqlite3_close(db_);
}

void GoalManager::ensure_db() {
  if (db_) return;

  int rc;
  if (db_path_.empty()) {
    rc = sqlite3_open(":memory:", &db_);
  } else {
    rc = sqlite3_open(db_path_.c_str(), &db_);
  }
  if (rc != SQLITE_OK) return;
  // v0.52.18: busy_timeout——外部进程读（哨兵/取证脚本常态操作）撞
  // 写锁时等待重试而非立即 SQLITE_BUSY（压测 50 读 0 失败，此为慢盘/
  // 高负载下的标准防护）。
  sqlite3_busy_timeout(db_, 3000);

  const char* sql = R"(
    CREATE TABLE IF NOT EXISTS goals (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      description TEXT NOT NULL,
      status TEXT NOT NULL DEFAULT 'pending',
      progress_pct INTEGER NOT NULL DEFAULT 0,
      created_at TEXT NOT NULL,
      updated_at TEXT NOT NULL,
      parent_id INTEGER,
      subtask_ids TEXT,
      meta TEXT
    );
  )";
  sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
  // v0.52.3: 旧库迁移——任务树字段（parent_id/subtask_ids）
  sqlite3_exec(db_,
      "ALTER TABLE goals ADD COLUMN parent_id INTEGER;", nullptr, nullptr, nullptr);
  sqlite3_exec(db_,
      "ALTER TABLE goals ADD COLUMN subtask_ids TEXT;", nullptr, nullptr, nullptr);
  // v0.52.4: 旧库迁移——通用 meta（JSON：分解 payload/断点状态等）
  sqlite3_exec(db_,
      "ALTER TABLE goals ADD COLUMN meta TEXT;", nullptr, nullptr, nullptr);
}

std::string GoalManager::now_iso() const {
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream oss;
  oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

Goal GoalManager::create(const std::string& description) {
  std::lock_guard<std::mutex> lk(mu_);
  ensure_db();
  if (!db_) return {};

  std::string ts = now_iso();
  std::string sql = "INSERT INTO goals (description, status, progress_pct, created_at, updated_at) VALUES (?, 'active', 0, ?, ?);";
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, description.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, ts.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  Goal g;
  g.id = static_cast<int>(sqlite3_last_insert_rowid(db_));
  g.description = description;
  g.status = "active";
  g.progress_pct = 0;
  g.created_at = ts;
  g.updated_at = ts;
  return g;
}

std::vector<Goal> GoalManager::list(const std::string& status_filter) const {
  std::lock_guard<std::mutex> lk(mu_);
  if (!db_) return {};

  std::string sql = "SELECT id, description, status, progress_pct, created_at, updated_at, parent_id, subtask_ids FROM goals";  // v0.52.3: 树字段
  if (!status_filter.empty()) {
    sql += " WHERE status = ?";
  }
  sql += " ORDER BY id DESC;";

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (!status_filter.empty()) {
    sqlite3_bind_text(stmt, 1, status_filter.c_str(), -1, SQLITE_TRANSIENT);
  }

  std::vector<Goal> goals;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    Goal g;
    g.id = sqlite3_column_int(stmt, 0);
    g.description = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    g.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    g.progress_pct = sqlite3_column_int(stmt, 3);
    g.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    g.updated_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL)
      g.parent_id = sqlite3_column_int(stmt, 6);
    if (sqlite3_column_text(stmt, 7)) {
      try {
        for (auto& v : nlohmann::json::parse(
                 reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7))))
          g.subtask_ids.push_back(v.get<int>());
      } catch (...) {}
    }
    goals.push_back(g);
  }
  sqlite3_finalize(stmt);
  return goals;
}

bool GoalManager::update(int id, const nlohmann::json& fields) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!db_) return false;

  std::string ts = now_iso();
  std::string sql = "UPDATE goals SET updated_at = ?";
  if (fields.contains("status")) sql += ", status = ?";
  if (fields.contains("progress_pct")) sql += ", progress_pct = ?";
  if (fields.contains("description")) sql += ", description = ?";
  sql += " WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);

  int idx = 1;
  sqlite3_bind_text(stmt, idx++, ts.c_str(), -1, SQLITE_TRANSIENT);
  if (fields.contains("status")) {
    std::string s = fields["status"].get<std::string>();
    sqlite3_bind_text(stmt, idx++, s.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (fields.contains("progress_pct")) {
    sqlite3_bind_int(stmt, idx++, fields["progress_pct"].get<int>());
  }
  if (fields.contains("description")) {
    std::string d = fields["description"].get<std::string>();
    sqlite3_bind_text(stmt, idx++, d.c_str(), -1, SQLITE_TRANSIENT);
  }
  sqlite3_bind_int(stmt, idx, id);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool GoalManager::remove(int id) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!db_) return false;

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_, "DELETE FROM goals WHERE id = ?;", -1, &stmt, nullptr);
  sqlite3_bind_int(stmt, 1, id);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

int GoalManager::active_count() const {
  auto goals = list("active");
  return static_cast<int>(goals.size());
}

std::string GoalManager::to_prompt_injection() const {
  auto goals = list("active");
  if (goals.empty()) return "";

  std::string out = "\n## 当前长期目标\n";
  for (const auto& g : goals) {
    out += "- [" + std::to_string(g.id) + "] " + g.description +
           " (进度: " + std::to_string(g.progress_pct) + "%)\n";
  }
  return out;
}

std::vector<Goal> GoalManager::stalled_goals(int hours) const {
  std::lock_guard<std::mutex> lk(mu_);
  if (!db_) return {};

  // 计算 cutoff 时间（当前 - hours）
  auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(hours);
  auto t = std::chrono::system_clock::to_time_t(cutoff);
  std::ostringstream oss;
  oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
  std::string cutoff_str = oss.str();

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_,
      "SELECT id, description, status, progress_pct, created_at, updated_at "
      "FROM goals WHERE status = 'active' AND updated_at < ? ORDER BY updated_at ASC;",
      -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, cutoff_str.c_str(), -1, SQLITE_TRANSIENT);

  std::vector<Goal> goals;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    Goal g;
    g.id = sqlite3_column_int(stmt, 0);
    g.description = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    g.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    g.progress_pct = sqlite3_column_int(stmt, 3);
    g.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    g.updated_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    goals.push_back(g);
  }
  sqlite3_finalize(stmt);
  return goals;
}

bool GoalManager::update_progress(int id, int progress_pct) {
  return update(id, {{"progress_pct", progress_pct}});
}

bool GoalManager::update_status(int id, const std::string& status, int progress_pct) {
  nlohmann::json fields;
  fields["status"] = status;
  if (progress_pct >= 0) fields["progress_pct"] = progress_pct;
  return update(id, fields);
}

// ── v0.52.3: 任务树 ─────────────────────────────────────────

Goal GoalManager::create_subtask(int parent_id, const std::string& description) {
  std::lock_guard<std::mutex> lk(mu_);
  ensure_db();
  if (!db_) return {};

  std::string ts = now_iso();
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_,
      "INSERT INTO goals (description, status, progress_pct, created_at, updated_at,"
      " parent_id) VALUES (?, 'pending', 0, ?, ?, ?);",
      -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, description.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, ts.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, ts.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 4, parent_id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  Goal g;
  g.id = static_cast<int>(sqlite3_last_insert_rowid(db_));
  g.description = description;
  g.status = "pending";
  g.created_at = ts;
  g.updated_at = ts;
  g.parent_id = parent_id;

  // 双向维护：父的 subtask_ids（JSON 数组）append 子 id 后写回
  {
    // 读取父 subtask_ids（JSON 数组）→ append → 写回
    std::string cur = "[]";
    sqlite3_prepare_v2(db_, "SELECT subtask_ids FROM goals WHERE id = ?;",
                       -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, parent_id);
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0)) {
      cur = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    try {
      auto arr = nlohmann::json::parse(cur);
      if (!arr.is_array()) arr = nlohmann::json::array();
      arr.push_back(g.id);
      cur = arr.dump();
    } catch (...) {
      cur = "[" + std::to_string(g.id) + "]";
    }
    sqlite3_prepare_v2(db_, "UPDATE goals SET subtask_ids = ? WHERE id = ?;",
                       -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, cur.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, parent_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  return g;
}

Goal GoalManager::get(int id) const {
  std::lock_guard<std::mutex> lk(mu_);
  if (!db_) return {};
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_,
      "SELECT id, description, status, progress_pct, created_at, updated_at,"
      " parent_id, subtask_ids, meta FROM goals WHERE id = ?;",
      -1, &stmt, nullptr);
  sqlite3_bind_int(stmt, 1, id);
  Goal g;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    g.id = sqlite3_column_int(stmt, 0);
    g.description = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    g.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    g.progress_pct = sqlite3_column_int(stmt, 3);
    g.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    g.updated_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL)
      g.parent_id = sqlite3_column_int(stmt, 6);
    if (sqlite3_column_text(stmt, 7)) {
      try {
        for (auto& v : nlohmann::json::parse(
                 reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7))))
          g.subtask_ids.push_back(v.get<int>());
      } catch (...) {}
    }
    if (sqlite3_column_text(stmt, 8)) {
      try {
        g.meta = nlohmann::json::parse(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8)));
      } catch (...) {}
    }
  }
  sqlite3_finalize(stmt);
  return g;
}

nlohmann::json GoalManager::get_meta(int id) const {
  return get(id).meta;
}

bool GoalManager::set_meta(int id, const nlohmann::json& meta) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!db_) return false;
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_,
      "UPDATE goals SET meta = ?, updated_at = ? WHERE id = ?;",
      -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, meta.dump().c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, now_iso().c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, id);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

int GoalManager::refresh_parent_progress(int child_id) {
  Goal child = get(child_id);
  if (child.parent_id == 0 || child.parent_id == child.id) return -1;

  Goal parent = get(child.parent_id);
  if (parent.id == 0 || parent.subtask_ids.empty()) return -1;

  int done_count = 0;
  int failed_count = 0;
  for (int sid : parent.subtask_ids) {
    Goal s = get(sid);
    if (s.id == 0) continue;
    if (s.status == "done") ++done_count;
    else if (s.status == "failed") ++failed_count;
  }
  int pct = static_cast<int>(100.0 * done_count / parent.subtask_ids.size());

  // 全部终结态 → 父状态收敛（有失败 → failed，否则 done）
  std::string parent_status;
  if (done_count + failed_count == (int)parent.subtask_ids.size()) {
    parent_status = failed_count > 0 ? "failed" : "done";
    pct = failed_count > 0 ? pct : 100;
  }
  nlohmann::json fields{{"progress_pct", pct}};
  if (!parent_status.empty()) fields["status"] = parent_status;
  update(parent.id, fields);
  return pct;
}

nlohmann::json GoalManager::tree_json(int root_id) const {
  Goal root = get(root_id);
  nlohmann::json j;
  if (root.id == 0) return j;
  j["id"] = root.id;
  j["description"] = root.description;
  j["status"] = root.status;
  j["progress_pct"] = root.progress_pct;
  if (!root.subtask_ids.empty()) {
    j["subtasks"] = nlohmann::json::array();
    for (int sid : root.subtask_ids) {
      // 单层树（decompose 是一层分解，不递归——防异常深树拖垮查询）
      Goal s = get(sid);
      if (s.id == 0) continue;
      j["subtasks"].push_back({
          {"id", s.id},
          {"description", s.description},
          {"status", s.status},
          {"progress_pct", s.progress_pct}});
    }
  }
  return j;
}

}  // namespace agent
}  // namespace thin_agent
