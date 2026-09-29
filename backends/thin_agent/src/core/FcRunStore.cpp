/// v0.52.26: FcRunStore 实现 —— 断点续跑 #2 持久化层。
/// 表结构（agent_sessions.db 同库新表，复用 profile 数据目录）：
///   fc_runs(run_id PK, session_id, user_text, status, iter, messages,
///           updated_at_ms)
///   fc_approvals(run_id, session_id, tool_name, tool_args,
///                fc_iterations_used, user_text, created_at_ms,
///                PRIMARY KEY(session_id))

#include "thin_agent/core/FcRunStore.h"
#include <mutex>

#include <sqlite3.h>

#include <chrono>
#include <cstring>

namespace thin_agent {

struct FcRunStore::Impl {
  std::mutex mu_;  // v0.53.42: sqlite 单连接非线程安全

  sqlite3* db{nullptr};
};

FcRunStore::FcRunStore() : impl_(std::make_unique<Impl>()) {}
FcRunStore::~FcRunStore() {
  if (impl_->db) sqlite3_close(impl_->db);
}

static bool exec_sql(sqlite3* db, const char* sql, std::string* err = nullptr) {
  char* e = nullptr;
  int rc = sqlite3_exec(db, sql, nullptr, nullptr, &e);
  if (rc != SQLITE_OK) {
    if (err && e) *err = e;
    if (e) sqlite3_free(e);
    return false;
  }
  return true;
}

static int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool FcRunStore::open(const std::string& db_path) {
  if (sqlite3_open(db_path.c_str(), &impl_->db) != SQLITE_OK) {
    // v0.53.74: 失败也须 close(句柄已分配)
    sqlite3_close(impl_->db);
    impl_->db = nullptr;
    return false;
  }
  std::string err;
  if (!exec_sql(impl_->db,
                "CREATE TABLE IF NOT EXISTS fc_runs ("
                " run_id TEXT PRIMARY KEY,"
                " session_id TEXT NOT NULL,"
                " user_text TEXT DEFAULT '',"
                " status TEXT NOT NULL DEFAULT 'running',"
                " iter INTEGER DEFAULT 0,"
                " messages TEXT DEFAULT '[]',"
                " updated_at_ms INTEGER DEFAULT 0);",
                &err) ||
      !exec_sql(impl_->db,
                "CREATE INDEX IF NOT EXISTS idx_fc_runs_session"
                " ON fc_runs(session_id, updated_at_ms DESC);",
                &err) ||
      !exec_sql(impl_->db,
                "CREATE TABLE IF NOT EXISTS fc_approvals ("
                " run_id TEXT NOT NULL,"
                " session_id TEXT NOT NULL,"
                " tool_name TEXT NOT NULL,"
                " tool_args TEXT DEFAULT '',"
                " fc_iterations_used INTEGER DEFAULT 0,"
                " user_text TEXT DEFAULT '',"
                " created_at_ms INTEGER DEFAULT 0,"
                " PRIMARY KEY(session_id));",
                &err)) {
    return false;
  }
  return true;
}

bool FcRunStore::create_run(const FcRun& run) {
  std::lock_guard<std::mutex> lk(impl_->mu_);  // v0.53.42: 单连接互斥(4 worker 并发)
  const char* sql =
      "INSERT OR REPLACE INTO fc_runs"
      "(run_id, session_id, user_text, status, iter, messages, updated_at_ms)"
      " VALUES(?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_text(st, 1, run.run_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, run.session_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, run.user_text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, run.status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 5, run.iter);
  sqlite3_bind_text(st, 6, run.messages.dump().c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 7, now_ms());
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool FcRunStore::append_messages(const std::string& run_id, int iter,
                                 const nlohmann::json& new_messages) {
  std::lock_guard<std::mutex> lk(impl_->mu_);  // v0.53.42: 单连接互斥(4 worker 并发)
  // 读旧 → 拼接 → 写回（工具边界频率低，行级读改写足够；messages 行上限
  // 由中间压缩天然约束——工具结果已截断 500 字符）
  const char* sel = "SELECT messages FROM fc_runs WHERE run_id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(impl_->db, sel, -1, &st, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_text(st, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
  nlohmann::json merged = nlohmann::json::array();
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    if (p) {
      try { merged = nlohmann::json::parse(p); } catch (...) {}
    }
  }
  sqlite3_finalize(st);
  if (!merged.is_array()) merged = nlohmann::json::array();
  if (new_messages.is_array())
    for (const auto& m : new_messages) merged.push_back(m);

  const char* upd =
      "UPDATE fc_runs SET messages=?, iter=?, updated_at_ms=? WHERE run_id=?;";
  if (sqlite3_prepare_v2(impl_->db, upd, -1, &st, nullptr) != SQLITE_OK)
    return false;
  std::string dump = merged.dump();
  sqlite3_bind_text(st, 1, dump.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, iter);
  sqlite3_bind_int64(st, 3, now_ms());
  sqlite3_bind_text(st, 4, run_id.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool FcRunStore::finish_run(const std::string& run_id, const std::string& status) {
  std::lock_guard<std::mutex> lk(impl_->mu_);  // v0.53.42: 单连接互斥(4 worker 并发)
  const char* upd =
      "UPDATE fc_runs SET status=?, updated_at_ms=? WHERE run_id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(impl_->db, upd, -1, &st, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_text(st, 1, status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, now_ms());
  sqlite3_bind_text(st, 3, run_id.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

int FcRunStore::mark_interrupted_on_boot() {
  std::lock_guard<std::mutex> lk(impl_->mu_);  // v0.53.42: 单连接互斥(4 worker 并发)
  char* e = nullptr;
  int rc = sqlite3_exec(impl_->db,
                        "UPDATE fc_runs SET status='interrupted'"
                        " WHERE status='running';",
                        nullptr, nullptr, &e);
  int n = 0;
  if (rc == SQLITE_OK) n = sqlite3_changes(impl_->db);
  if (e) sqlite3_free(e);
  return n;
}

FcRun FcRunStore::load_resumable(const std::string& session_id) {
  std::lock_guard<std::mutex> lk(impl_->mu_);  // v0.53.42: 单连接互斥(4 worker 并发)
  FcRun run;  // 空 run_id = 无
  const char* sel =
      "SELECT run_id, session_id, user_text, status, iter, messages"
      " FROM fc_runs WHERE session_id=? AND status='interrupted'"
      " ORDER BY updated_at_ms DESC LIMIT 1;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(impl_->db, sel, -1, &st, nullptr) != SQLITE_OK)
    return run;
  sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) {
    run.run_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    run.session_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    run.user_text = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    run.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    run.iter = sqlite3_column_int(st, 4);
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    if (p) { try { run.messages = nlohmann::json::parse(p); } catch (...) {} }
  }
  sqlite3_finalize(st);
  return run;
}

std::vector<FcRun> FcRunStore::list_runs(const std::string& session_id,
                                         int limit) {
  std::vector<FcRun> out;
  const char* sel =
      "SELECT run_id, session_id, user_text, status, iter, messages,"
      " updated_at_ms FROM fc_runs WHERE session_id=?"
      " ORDER BY updated_at_ms DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(impl_->db, sel, -1, &st, nullptr) != SQLITE_OK)
    return out;
  sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    FcRun r;
    r.run_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.session_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.user_text = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.iter = sqlite3_column_int(st, 4);
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    if (p) { try { r.messages = nlohmann::json::parse(p); } catch (...) {} }
    r.updated_at_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool FcRunStore::save_approval(const FcPendingApproval& pa) {
  std::lock_guard<std::mutex> lk(impl_->mu_);  // v0.53.42: 单连接互斥(4 worker 并发)
  const char* sql =
      "INSERT OR REPLACE INTO fc_approvals"
      "(run_id, session_id, tool_name, tool_args, fc_iterations_used,"
      " user_text, created_at_ms) VALUES(?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_text(st, 1, pa.run_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, pa.session_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, pa.tool_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, pa.tool_args.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 5, pa.fc_iterations_used);
  sqlite3_bind_text(st, 6, pa.user_text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 7, pa.created_at_ms ? pa.created_at_ms : now_ms());
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

FcPendingApproval FcRunStore::take_approval(const std::string& session_id) {
  std::lock_guard<std::mutex> lk(impl_->mu_);  // v0.53.42: 单连接互斥(4 worker 并发)
  FcPendingApproval pa;  // 空 run_id = 无
  const char* sel =
      "SELECT run_id, session_id, tool_name, tool_args,"
      " fc_iterations_used, user_text, created_at_ms"
      " FROM fc_approvals WHERE session_id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(impl_->db, sel, -1, &st, nullptr) != SQLITE_OK)
    return pa;
  sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) {
    pa.run_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    pa.session_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    pa.tool_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    pa.tool_args = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    pa.fc_iterations_used = sqlite3_column_int(st, 4);
    pa.user_text = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    pa.created_at_ms = sqlite3_column_int64(st, 6);
  }
  sqlite3_finalize(st);
  if (!pa.run_id.empty()) {
    const char* del = "DELETE FROM fc_approvals WHERE session_id=?;";
    if (sqlite3_prepare_v2(impl_->db, del, -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(st);
      sqlite3_finalize(st);
    }
  }
  return pa;
}

int FcRunStore::purge_expired_approvals(int ttl_sec) {
  const int64_t cutoff = now_ms() - static_cast<int64_t>(ttl_sec) * 1000;
  const char* del = "DELETE FROM fc_approvals WHERE created_at_ms<?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(impl_->db, del, -1, &st, nullptr) != SQLITE_OK)
    return 0;
  sqlite3_bind_int64(st, 1, cutoff);
  sqlite3_step(st);
  sqlite3_finalize(st);
  return sqlite3_changes(impl_->db);
}

}  // namespace thin_agent
