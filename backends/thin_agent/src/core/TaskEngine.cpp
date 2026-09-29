#include "thin_agent/core/TaskEngine.h"

#include "thin_agent/log/LogEvent.h"  // v0.53.91: 启动收尾可观测

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <sqlite3.h>

// TaskEngine：SQLite 持久化任务队列，同步 submit + 指数退避重试 + 审计轨迹。

namespace thin_agent {

namespace {

/// JSON 序列化为字符串（SQLite 存储用）。
std::string jdump(const nlohmann::json& j) { return j.dump(); }

/// 任务状态机合法迁移判定（用于审计与防御性校验）。
bool is_valid_transition(const std::string& from_state, const std::string& to_state) {
  if (from_state.empty()) return to_state == "queued";
  if (from_state == to_state) return to_state == "retrying";
  if (from_state == "queued") return to_state == "running" || to_state == "cancelled";
  if (from_state == "running") {
    return to_state == "retrying" || to_state == "success" || to_state == "failed" || to_state == "cancelled";
  }
  if (from_state == "retrying") {
    return to_state == "retrying" || to_state == "success" || to_state == "failed" || to_state == "cancelled";
  }
  return false;
}

/// 将 ActionExecutor 错误码映射为任务域统一码（24xxx / 25xxx / 29999）。
int map_error_code(int action_code) {
  if (action_code == 0) return 0;
  if (action_code == 4001) return 24001;      // unsupported action
  if (action_code == 4004) return 24004;      // capture not found
  if (action_code == 4008) return 24008;      // invalid mode
  if (action_code >= 5000) return 25000 + action_code;  // internal class
  return 29999;
}

/// SQLite 返回值检查，失败时抛 runtime_error。
void check_sqlite(int rc, sqlite3* db, const char* step) {
  if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW) return;
  std::string msg = std::string(step) + ": " + sqlite3_errmsg(db);
  throw std::runtime_error(msg);
}
}  // namespace

TaskEngine::TaskEngine(std::shared_ptr<ActionExecutor> executor)
    : TaskEngine(std::move(executor), Config{}) {}

TaskEngine::TaskEngine(std::shared_ptr<ActionExecutor> executor, Config cfg)
    : executor_(std::move(executor)), cfg_(cfg) {}

TaskEngine::~TaskEngine() {
  if (db_) {
    sqlite3_close(static_cast<sqlite3*>(db_));
    db_ = nullptr;
  }
}

bool TaskEngine::init(const std::string& db_path) {
  std::filesystem::create_directories(std::filesystem::path(db_path).parent_path());
  db_path_ = db_path;

  sqlite3* db = nullptr;
  int rc = sqlite3_open(db_path_.c_str(), &db);
  if (rc != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return false;
  }
  db_ = db;

  // WAL 模式：允许并发读 + 单写，避免多测试进程锁冲突
  sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

  create_schema();
  sync_seq_from_db();

  // v0.53.91 启动收尾：上次进程死亡时留在 running/retrying/queued 的行，执行线程已随
  // 进程消失 → 无人推进，状态永久谎报"运行中"。启动即收尾并写可观测日志。
  const std::size_t reaped = reap_interrupted_tasks();
  if (reaped > 0) {
    log_event("task", LogLevel::Warn, "reaped interrupted tasks on startup",
              {{"count", std::to_string(reaped)}});
  }
  return true;
}

void TaskEngine::sync_seq_from_db() {
  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT task_id FROM tasks WHERE task_id LIKE 'task-%' ORDER BY rowid DESC LIMIT 1";
  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "prepare sync_seq_from_db");
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (txt) {
      std::string id(txt);
      auto pos = id.find('-');
      if (pos != std::string::npos && pos + 1 < id.size()) {
        try {
          seq_ = static_cast<uint64_t>(std::stoull(id.substr(pos + 1)));
        } catch (...) {
          seq_ = 0;
        }
      }
    }
  }
  sqlite3_finalize(stmt);
}

/// v0.53.91: 启动收尾（僵尸任务）。崩溃/重启后没有任何恢复队列，非终态行就是死行。
/// 状态机约束决定目标态：running/retrying → failed（合法迁移），queued → cancelled
/// （`queued -> failed` 非法，写了会抛 invalid state transition）。
std::size_t TaskEngine::reap_interrupted_tasks() {
  auto* db = static_cast<sqlite3*>(db_);
  struct Stale {
    std::string id, state, result_json, error_json;
    int attempts;
  };
  std::vector<Stale> stale;
  {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT task_id, state, attempts, result_json, error_json FROM tasks "
        "WHERE state IN ('running','retrying','queued')";
    check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "prepare reap");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      auto col = [&](int i) {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
        return t ? std::string(t) : std::string();
      };
      stale.push_back({col(0), col(1), col(3), col(4), sqlite3_column_int(stmt, 2)});
    }
    sqlite3_finalize(stmt);
  }

  for (const auto& s : stale) {
    const bool never_started = (s.state == "queued");
    const std::string to = never_started ? "cancelled" : "failed";
    const int code = never_started ? 26010 : 26011;
    const std::string msg = never_started
                                ? "interrupted by restart (never started)"
                                : "interrupted by restart (execution lost)";
    try {
      // 尽量保住原有 result/error 现场（便于排障），只改状态与说明
      update_task_row(s.id, to, s.attempts, code, msg, s.result_json, s.error_json);
    } catch (const std::exception& e) {
      log_event("task", LogLevel::Error, "reap failed",
                {{"task_id", s.id}, {"what", e.what()}});
    }
  }
  return stale.size();
}

void TaskEngine::create_schema() {
  auto* db = static_cast<sqlite3*>(db_);
  const char* sql = R"SQL(
CREATE TABLE IF NOT EXISTS tasks (
  task_id TEXT PRIMARY KEY,
  action TEXT NOT NULL,
  args_json TEXT NOT NULL,
  state TEXT NOT NULL,
  attempts INTEGER NOT NULL,
  max_retries INTEGER NOT NULL,
  code INTEGER NOT NULL,
  message TEXT NOT NULL,
  result_json TEXT,
  error_json TEXT,
  idempotency_key TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_tasks_idem ON tasks(idempotency_key);

CREATE TABLE IF NOT EXISTS task_audits (
  audit_id INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id TEXT NOT NULL,
  from_state TEXT NOT NULL,
  to_state TEXT NOT NULL,
  attempts INTEGER NOT NULL,
  code INTEGER NOT NULL,
  message TEXT NOT NULL,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_task_audits_task ON task_audits(task_id, audit_id DESC);
)SQL";

  char* err = nullptr;
  int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string msg = err ? err : "sqlite schema error";
    if (err) sqlite3_free(err);
    throw std::runtime_error(msg);
  }
}

std::string TaskEngine::now_iso() const {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto tt = system_clock::to_time_t(now);
  std::tm tm = *std::gmtime(&tt);
  std::ostringstream os;
  os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return os.str();
}

std::string TaskEngine::new_task_id() {
  std::lock_guard<std::mutex> lk(mu_);
  ++seq_;
  return "task-" + std::to_string(seq_);
}

void TaskEngine::append_audit(const std::string& task_id,
                              const std::string& from_state,
                              const std::string& to_state,
                              int attempts,
                              int code,
                              const std::string& message) {
  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = R"SQL(
INSERT INTO task_audits(task_id, from_state, to_state, attempts, code, message, created_at)
VALUES(?, ?, ?, ?, ?, ?, ?)
)SQL";
  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "prepare append_audit");
  std::string ts = now_iso();
  sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, from_state.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, to_state.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 4, attempts);
  sqlite3_bind_int(stmt, 5, code);
  sqlite3_bind_text(stmt, 6, message.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, ts.c_str(), -1, SQLITE_TRANSIENT);
  check_sqlite(sqlite3_step(stmt), db, "step append_audit");
  sqlite3_finalize(stmt);
}

std::string TaskEngine::fetch_task_state(const std::string& task_id) {
  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT state FROM tasks WHERE task_id=?";
  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "prepare fetch_task_state");
  sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  std::string state = (rc == SQLITE_ROW) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) : "";
  sqlite3_finalize(stmt);
  return state;
}

bool TaskEngine::task_exists(const std::string& task_id) {
  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT 1 FROM tasks WHERE task_id=? LIMIT 1";
  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "prepare task_exists");
  sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  const bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
  sqlite3_finalize(stmt);
  return exists;
}

std::optional<std::string> TaskEngine::find_by_idem(const std::string& idem_key) {
  if (idem_key.empty()) return std::nullopt;
  auto* db = static_cast<sqlite3*>(db_);

  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT task_id FROM tasks WHERE idempotency_key=? LIMIT 1";
  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "prepare find_by_idem");
  sqlite3_bind_text(stmt, 1, idem_key.c_str(), -1, SQLITE_TRANSIENT);

  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return id;
  }
  sqlite3_finalize(stmt);
  return std::nullopt;
}

void TaskEngine::persist_task(const std::string& task_id,
                              const std::string& action,
                              const std::string& args_json,
                              const std::string& idem_key) {
  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = R"SQL(
INSERT INTO tasks(task_id, action, args_json, state, attempts, max_retries, code, message,
                  result_json, error_json, idempotency_key, created_at, updated_at)
VALUES(?, ?, ?, 'queued', 0, ?, 0, 'queued', '{}', '{}', ?, ?, ?)
)SQL";

  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "prepare persist_task");
  std::string ts = now_iso();
  sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, action.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, args_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 4, cfg_.max_retries);
  if (idem_key.empty()) sqlite3_bind_null(stmt, 5);
  else sqlite3_bind_text(stmt, 5, idem_key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, ts.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, ts.c_str(), -1, SQLITE_TRANSIENT);

  check_sqlite(sqlite3_step(stmt), db, "step persist_task");
  sqlite3_finalize(stmt);

  append_audit(task_id, "", "queued", 0, 0, "queued");
}

void TaskEngine::update_task_row(const std::string& task_id,
                                 const std::string& state,
                                 int attempts,
                                 int code,
                                 const std::string& message,
                                 const std::string& result_json,
                                 const std::string& error_json) {
  auto* db = static_cast<sqlite3*>(db_);
  const std::string prev = fetch_task_state(task_id);
  if (!is_valid_transition(prev, state)) {
    throw std::runtime_error("invalid state transition: " + prev + " -> " + state);
  }

  sqlite3_stmt* stmt = nullptr;
  const char* sql = R"SQL(
UPDATE tasks
SET state=?, attempts=?, code=?, message=?, result_json=?, error_json=?, updated_at=?
WHERE task_id=?
)SQL";
  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "prepare update_task");
  std::string ts = now_iso();
  sqlite3_bind_text(stmt, 1, state.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, attempts);
  sqlite3_bind_int(stmt, 3, code);
  sqlite3_bind_text(stmt, 4, message.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, result_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, error_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, ts.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, task_id.c_str(), -1, SQLITE_TRANSIENT);
  check_sqlite(sqlite3_step(stmt), db, "step update_task");
  sqlite3_finalize(stmt);

  if (prev != state || state == "retrying") {
    append_audit(task_id, prev, state, attempts, code, message);
  }
}

/// v0.53.89: 取消标志三件套——协作式取消（记账式取消的修复）。
/// 背景：此前 cancel_task 只改 DB 行，全仓**无任何取消检查点**：在跑动作不中断、
/// 重试与退避照跑，且 submit_task 在动作结束后**无条件写终态** → 取消被 success
/// 覆盖（用户看到"取消成功"，任务实际照常完成并产生副作用）。
bool TaskEngine::is_cancelled(const std::string& task_id) {
  std::lock_guard<std::mutex> lk(cancel_mu_);
  return cancelled_.find(task_id) != cancelled_.end();
}

void TaskEngine::clear_cancelled(const std::string& task_id) {
  std::lock_guard<std::mutex> lk(cancel_mu_);
  cancelled_.erase(task_id);
}

std::size_t TaskEngine::cancelled_pending_count() {
  std::lock_guard<std::mutex> lk(cancel_mu_);
  return cancelled_.size();
}

/// 100ms 切片睡眠，期间轮询取消标志：退避最多 3s（配置上限）不再"取消后还睡满"。
bool TaskEngine::sleep_with_cancel(const std::string& task_id, int ms) {
  constexpr int kSlice = 100;
  int slept = 0;
  while (slept < ms) {
    if (is_cancelled(task_id)) return false;
    const int chunk = std::min(kSlice, ms - slept);
    std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
    slept += chunk;
  }
  return !is_cancelled(task_id);
}

/// 取消结果（统一形状：state=cancelled + 26010 + 说明原因）。
static nlohmann::json cancelled_result(const char* why) {
  nlohmann::json r = {
      {"state", "cancelled"},
      {"attempts", 0},
      {"code", 26010},
      {"message", why},
      {"result", nlohmann::json::object()},
      {"error", nlohmann::json{{"reason", why}}},
  };
  return r;
}

/// 带指数退避与抖动的重试执行；每次失败写入 retrying 审计。
nlohmann::json TaskEngine::execute_with_retry(const std::string& task_id,
                                              const std::string& action,
                                              const nlohmann::json& args) {
  Kv kv;
  if (args.is_object()) {
    for (auto it = args.begin(); it != args.end(); ++it) {
      if (it.value().is_string()) kv[it.key()] = it.value().get<std::string>();
      else kv[it.key()] = it.value().dump();
    }
  }

  nlohmann::json result = {
      {"state", "failed"},
      {"attempts", 0},
      {"code", 29999},
      {"message", "unknown"},
      {"result", nlohmann::json::object()},
      {"error", nlohmann::json::object()},
  };

  if (!executor_) {
    result["code"] = 25001;
    result["message"] = "executor unavailable";
    result["error"] = nlohmann::json{{"reason", "executor unavailable"}};
    return result;
  }

  std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> jitter(-cfg_.backoff_jitter_ms, cfg_.backoff_jitter_ms);

  for (int attempt = 1; attempt <= cfg_.max_retries + 1; ++attempt) {
    // 取消检查点 1/3：尝试开始前——取消过的任务不再发起新尝试
    if (is_cancelled(task_id)) return cancelled_result("cancelled before attempt");

    Result r = executor_->execute(action, kv, cfg_.action_timeout_ms);

    // 取消检查点 2/3：动作返回后——**取消优先于执行结果**（长动作跑到一半被取消时，
    // 其结果不再冒充 success；副作用可能已发生，故 message 如实说明）
    if (is_cancelled(task_id)) {
      nlohmann::json c = cancelled_result("cancelled during execution (side effects may have occurred)");
      c["attempts"] = attempt;
      return c;
    }
    nlohmann::json data = nlohmann::json::object();
    for (const auto& [k, v] : r.data) data[k] = v;

    const int unified = map_error_code(r.code);
    if (r.code == 0) {
      result["state"] = "success";
      result["attempts"] = attempt;
      result["code"] = 0;
      result["message"] = r.message;
      result["result"] = data;
      result["error"] = nlohmann::json::object();
      return result;
    }

    result["attempts"] = attempt;
    result["code"] = unified;
    result["message"] = r.message;
    result["result"] = data;
    result["error"] = nlohmann::json{{"attempt", attempt}, {"reason", r.message}, {"raw_code", r.code}};

    if (attempt <= cfg_.max_retries) {
      // 竞态兜底：checkpoint 2 与本行之间若被取消，cancelled->retrying 非法会抛异常
      if (is_cancelled(task_id)) {
        nlohmann::json c = cancelled_result("cancelled before retry bookkeeping");
        c["attempts"] = attempt;
        return c;
      }
      update_task_row(task_id, "retrying", attempt, unified, "retrying", data.dump(), result["error"].dump());
      int backoff = cfg_.backoff_base_ms * static_cast<int>(std::pow(2, attempt - 1));
      backoff = std::min(backoff, cfg_.backoff_max_ms) + jitter(rng);
      if (backoff < 0) backoff = 0;
      // 取消检查点 3/3：退避睡眠可被打断（否则取消后还要睡满 backoff_max_ms）
      if (!sleep_with_cancel(task_id, backoff)) {
        nlohmann::json c = cancelled_result("cancelled during backoff");
        c["attempts"] = attempt;
        return c;
      }
    }
  }

  result["state"] = "failed";
  return result;
}

/// 幂等提交：先查 idempotency_key，再 persist → running → execute_with_retry → 终态落库。
std::string TaskEngine::submit_task(const std::string& action,
                                    const nlohmann::json& args,
                                    const std::string& idempotency_key) {
  if (auto old = find_by_idem(idempotency_key); old.has_value()) {
    return *old;
  }

  const std::string task_id = new_task_id();
  const std::string args_json = jdump(args);
  persist_task(task_id, action, args_json, idempotency_key);

  update_task_row(task_id, "running", 0, 0, "running", "{}", "{}");

  auto res = execute_with_retry(task_id, action, args);
  std::string state = res.value("state", "failed");
  const int attempts = res.value("attempts", 0);
  int code = res.value("code", 29999);
  std::string message = res.value("message", "unknown");
  const std::string result_json = jdump(res.value("result", nlohmann::json::object()));
  const std::string error_json = jdump(res.value("error", nlohmann::json::object()));

  // v0.53.89 结果后写保护：取消**优先于**执行结果。执行循环虽已在 3 个检查点返回
  // cancelled，但取消可能恰好发生在最后一步与本次写库之间（TOCTOU）——这里再判一次，
  // 否则"取消成功"会被 success 静默覆盖（这是本缺陷最伤的一条）。
  if (is_cancelled(task_id)) {
    // 若取消已落库（状态已是终态），**不能再写**：状态机不许 cancelled->cancelled /
    // cancelled->success，重复写会抛 std::runtime_error（v0.53.89 单测当场抓到）。
    const std::string cur = fetch_task_state(task_id);
    if (cur == "cancelled") {
      clear_cancelled(task_id);   // 终态已就位 → 只释放标志
      return task_id;
    }
    state = "cancelled";
    code = 26010;
    message = "cancelled (execution result discarded)";
  }
  update_task_row(task_id, state, attempts, code, message, result_json, error_json);
  clear_cancelled(task_id);  // 终态已落库 → 释放内存标志（防 set 无界增长）

  return task_id;
}

/// 查询单条任务行并反序列化 JSON 字段。
nlohmann::json TaskEngine::get_task(const std::string& task_id) {
  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = R"SQL(
SELECT task_id, action, args_json, state, attempts, max_retries, code, message,
       result_json, error_json, idempotency_key, created_at, updated_at
FROM tasks WHERE task_id=?
)SQL";
  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "prepare get_task");
  sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);

  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return nlohmann::json{{"exists", false}, {"task_id", task_id}};
  }

  nlohmann::json row = {
      {"exists", true},
      {"task_id", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))},
      {"action", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))},
      {"args", nlohmann::json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)))},
      {"state", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))},
      {"attempts", sqlite3_column_int(stmt, 4)},
      {"max_retries", sqlite3_column_int(stmt, 5)},
      {"code", sqlite3_column_int(stmt, 6)},
      {"message", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7))},
      {"result", nlohmann::json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8)))},
      {"error", nlohmann::json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9)))},
      {"idempotency_key", sqlite3_column_type(stmt, 10) == SQLITE_NULL ? "" : reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10))},
      {"created_at", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11))},
      {"updated_at", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12))},
  };

  sqlite3_finalize(stmt);
  return row;
}

/// 按更新时间倒序列出任务，支持 state/action 过滤与 limit 截断。
nlohmann::json TaskEngine::list_tasks(int limit,
                                      const std::string& state_filter,
                                      const std::string& action_filter,
                                      int* limit_applied_out) {
  if (limit <= 0) limit = 20;
  if (limit > 200) limit = 200;
  if (limit_applied_out) *limit_applied_out = limit;

  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;

  std::string sql =
      "SELECT task_id, action, state, attempts, code, message, created_at, updated_at "
      "FROM tasks";

  std::vector<std::string> where;
  if (!state_filter.empty()) where.push_back("state=?");
  if (!action_filter.empty()) where.push_back("action=?");
  if (!where.empty()) {
    sql += " WHERE ";
    for (size_t i = 0; i < where.size(); ++i) {
      if (i) sql += " AND ";
      sql += where[i];
    }
  }
  sql += " ORDER BY rowid DESC LIMIT ?";

  check_sqlite(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), db, "prepare list_tasks");
  int bind_idx = 1;
  if (!state_filter.empty()) {
    sqlite3_bind_text(stmt, bind_idx++, state_filter.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (!action_filter.empty()) {
    sqlite3_bind_text(stmt, bind_idx++, action_filter.c_str(), -1, SQLITE_TRANSIENT);
  }
  sqlite3_bind_int(stmt, bind_idx, limit);

  nlohmann::json arr = nlohmann::json::array();
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    arr.push_back({
        {"task_id", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))},
        {"action", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))},
        {"state", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))},
        {"attempts", sqlite3_column_int(stmt, 3)},
        {"code", sqlite3_column_int(stmt, 4)},
        {"message", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5))},
        {"created_at", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6))},
        {"updated_at", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7))},
    });
  }
  sqlite3_finalize(stmt);
  return arr;
}

/// 取消 queued/running/retrying 任务；终态任务返回 cancelled=false。
nlohmann::json TaskEngine::cancel_task(const std::string& task_id, const std::string& reason) {
  auto task = get_task(task_id);
  if (!task.value("exists", false)) return task;

  const std::string st = task.value("state", "");
  if (st == "success" || st == "failed" || st == "cancelled") {
    return nlohmann::json{{"exists", true}, {"task_id", task_id}, {"cancelled", false}, {"reason", "terminal state"}, {"state", st}};
  }

  // v0.53.89: 先置协作式标志（执行线程据此在检查点停下），再落库——顺序不可反：
  // 反了则"已在跑的尝试"可能在本行落库后仍写回 success（结果后写）。
  {
    std::lock_guard<std::mutex> ck(cancel_mu_);
    cancelled_.insert(task_id);
  }
  update_task_row(task_id,
                  "cancelled",
                  task.value("attempts", 0),
                  26010,
                  reason.empty() ? "cancelled" : reason,
                  task.value("result", nlohmann::json::object()).dump(),
                  task.value("error", nlohmann::json::object()).dump());
  auto after = get_task(task_id);
  after["cancelled"] = true;
  return after;
}

/// 读取原任务参数并以新 idempotency_key 重新 submit。
nlohmann::json TaskEngine::replay_task(const std::string& task_id, const std::string& idempotency_key) {
  auto task = get_task(task_id);
  if (!task.value("exists", false)) return nlohmann::json{{"exists", false}, {"task_id", task_id}};

  const std::string action = task.value("action", "");
  const nlohmann::json args = task.value("args", nlohmann::json::object());
  const std::string new_task_id = submit_task(action, args, idempotency_key);
  return nlohmann::json{{"exists", true}, {"original_task_id", task_id}, {"replayed_task_id", new_task_id}, {"task", get_task(new_task_id)}};
}

/// 返回审计列表及最新迁移摘要（供 AgentService 记忆证据链使用）。
nlohmann::json TaskEngine::list_task_audits(const std::string& task_id, int limit) {
  if (limit <= 0) limit = 50;
  if (limit > 200) limit = 200;
  const int limit_applied = limit;

  const bool exists = task_exists(task_id);

  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql = R"SQL(
SELECT audit_id, from_state, to_state, attempts, code, message, created_at
FROM task_audits
WHERE task_id=?
ORDER BY audit_id DESC
LIMIT ?
)SQL";
  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db, "prepare list_task_audits");
  sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, limit);

  nlohmann::json arr = nlohmann::json::array();
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    arr.push_back({
        {"audit_id", sqlite3_column_int(stmt, 0)},
        {"from_state", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))},
        {"to_state", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))},
        {"attempts", sqlite3_column_int(stmt, 3)},
        {"code", sqlite3_column_int(stmt, 4)},
        {"message", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5))},
        {"created_at", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6))},
    });
  }
  sqlite3_finalize(stmt);
  const bool audit_has_entries = !arr.empty();
  const int audit_latest_audit_id = arr.empty() ? -1 : arr.front().value("audit_id", -1);
  const std::string audit_latest_from_state =
      arr.empty() ? "" : arr.front().value("from_state", "");
  const std::string audit_latest_to_state =
      arr.empty() ? "" : arr.front().value("to_state", "");
  const int audit_latest_attempts = arr.empty() ? -1 : arr.front().value("attempts", -1);
  const int audit_latest_code = arr.empty() ? -1 : arr.front().value("code", -1);
  const std::string audit_latest_message =
      arr.empty() ? "" : arr.front().value("message", "");
  const std::string audit_latest_created_at =
      arr.empty() ? "" : arr.front().value("created_at", "");
  const std::string audit_latest_transition =
      arr.empty() ? "" : (audit_latest_from_state + "->" + audit_latest_to_state);
  const bool audit_latest_is_terminal =
      audit_latest_to_state == "success" || audit_latest_to_state == "failed" ||
      audit_latest_to_state == "cancelled";
  const bool audit_latest_is_queued = audit_latest_to_state == "queued";
  const bool audit_latest_is_running = audit_latest_to_state == "running";
  const bool audit_latest_is_retrying = audit_latest_to_state == "retrying";
  const bool audit_latest_is_non_terminal =
      audit_latest_is_queued || audit_latest_is_running || audit_latest_is_retrying;
  const bool audit_latest_is_active =
      audit_latest_is_queued || audit_latest_is_running || audit_latest_is_retrying;
  const bool audit_latest_is_success = audit_latest_to_state == "success";
  const bool audit_latest_is_failed = audit_latest_to_state == "failed";
  const bool audit_latest_is_cancelled = audit_latest_to_state == "cancelled";
  const bool audit_latest_is_done =
      audit_latest_is_success || audit_latest_is_failed || audit_latest_is_cancelled;
  const bool audit_latest_is_failure_terminal =
      audit_latest_is_failed || audit_latest_is_cancelled;
  const std::string audit_latest_state_category =
      arr.empty() ? "none"
                  : (audit_latest_is_non_terminal
                         ? "non_terminal"
                         : (audit_latest_is_success ? "terminal_success"
                                                    : (audit_latest_is_failure_terminal
                                                           ? "terminal_failure"
                                                           : "unknown")));
  const int audit_latest_state_rank =
      audit_latest_state_category == "none"
          ? 0
          : (audit_latest_state_category == "non_terminal"
                 ? 1
                 : (audit_latest_state_category == "terminal_success"
                        ? 2
                        : (audit_latest_state_category == "terminal_failure" ? 3 : -1)));
  const bool audit_latest_has_started =
      audit_latest_is_running || audit_latest_is_retrying || audit_latest_is_success ||
      audit_latest_is_failed || audit_latest_is_cancelled;
  const bool audit_latest_is_in_progress =
      audit_latest_is_running || audit_latest_is_retrying;
  const bool audit_latest_has_error = audit_latest_code > 0;
  return nlohmann::json{{"task_id", task_id},
                        {"exists", exists},
                        {"audit_has_entries", audit_has_entries},
                        {"limit_applied", limit_applied},
                        {"audit_count_returned", static_cast<int>(arr.size())},
                        {"audit_latest_audit_id", audit_latest_audit_id},
                        {"audit_latest_from_state", audit_latest_from_state},
                        {"audit_latest_to_state", audit_latest_to_state},
                        {"audit_latest_transition", audit_latest_transition},
                        {"audit_latest_is_terminal", audit_latest_is_terminal},
                        {"audit_latest_is_non_terminal", audit_latest_is_non_terminal},
                        {"audit_latest_is_active", audit_latest_is_active},
                        {"audit_latest_is_queued", audit_latest_is_queued},
                        {"audit_latest_is_running", audit_latest_is_running},
                        {"audit_latest_is_retrying", audit_latest_is_retrying},
                        {"audit_latest_is_success", audit_latest_is_success},
                        {"audit_latest_is_failed", audit_latest_is_failed},
                        {"audit_latest_is_cancelled", audit_latest_is_cancelled},
                        {"audit_latest_is_done", audit_latest_is_done},
                        {"audit_latest_is_failure_terminal", audit_latest_is_failure_terminal},
                        {"audit_latest_state_category", audit_latest_state_category},
                        {"audit_latest_state_rank", audit_latest_state_rank},
                        {"audit_latest_has_started", audit_latest_has_started},
                        {"audit_latest_is_in_progress", audit_latest_is_in_progress},
                        {"audit_latest_has_error", audit_latest_has_error},
                        {"audit_latest_attempts", audit_latest_attempts},
                        {"audit_latest_code", audit_latest_code},
                        {"audit_latest_message", audit_latest_message},
                        {"audit_latest_created_at", audit_latest_created_at},
                        {"audits", arr}};
}

}  // namespace thin_agent
