#include "thin_agent/core/CronScheduler.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <sqlite3.h>

namespace thin_agent {

namespace {

/// 简单 cron 解析：支持 "every Nh"/"Nm"/"Ns"、"30m"、秒数和标准 "* * * * *" 格式。
int64_t parse_interval_seconds(const std::string& schedule) {
  if (schedule.empty()) return -1;

  // "every Nx"
  if (schedule.rfind("every ", 0) == 0) {
    int num = 0;
    char unit = 0;
    if (sscanf(schedule.c_str() + 6, "%d%c", &num, &unit) == 2) {
      switch (unit) {
        case 'h': case 'H': return num * 3600;
        case 'm': case 'M': return num * 60;
        case 's': case 'S': return num;
        default: break;
      }
    }
  }

  // "Nx"
  {
    int num = 0;
    char unit = 0;
    int consumed = 0;
    // v0.53.46: %n 保证 unit 是末字符——"2020-01-01T…" 的 '-' 单位
    /// 被匹配前先确认整串形态,防前缀数字+杂字符误吃
    if (sscanf(schedule.c_str(), "%d%c%n", &num, &unit, &consumed) == 2 &&
        consumed == (int)schedule.size()) {
      switch (unit) {
        case 'h': case 'H': return num * 3600;
        case 'm': case 'M': return num * 60;
        case 's': case 'S': return num;
        default: break;
      }
    }
  }

  // bare number = seconds
  {
    int num = 0;
    int consumed = 0;
    // v0.53.46 修:必须整串是纯数字——%n 检查尾随字符。
    /// 此前 "2020-01-01T00:00:00" 被 sscanf("%d") 吃成 2020 秒
    /// →ISO 一次性调度变 34 分钟循环触发、"一次后禁用"永不生效
    if (sscanf(schedule.c_str(), "%d%n", &num, &consumed) == 1 && num > 0 &&
        consumed == (int)schedule.size()) {
      return num;
    }
  }

  return -1;
}

/// 标准 cron 表达式检测："分 时 日 月 周"
/// 返回 true 表明显式包含空格分隔的 5 字段，认为需按 next_cron_match 计算。
bool is_cron_expr(const std::string& schedule) {
  if (schedule.empty()) return false;
  int fields = 0;
  bool in_space = true;
  for (char c : schedule) {
    if (c == ' ') {
      if (!in_space) { ++fields; in_space = true; }
    } else {
      in_space = false;
    }
  }
  if (!in_space) ++fields;
  return fields == 5;
}

/// 计算标准 cron 表达式的下次触发时间戳。
/// 支持: * N * * * (整点/半点), */N * * * * (每隔 N 分), N,N,N (列表)
static int64_t next_cron_match_impl(const std::string& schedule, int64_t from_ts);

int64_t next_cron_match(const std::string& schedule, int64_t from_ts) {
  // v0.53.75: 入口防御——字段 stoi 对畸形 token(* /abc、5;x)抛
  /// invalid_argument 未捕获,ticker 扫描线(L390)中招=线程崩=整个
  /// cron 停摆;畸形一律回落 interval 语义(-1→上层按不识别处理)
  try {
    return next_cron_match_impl(schedule, from_ts);
  } catch (...) {
    return -1;
  }
}

static int64_t next_cron_match_impl(const std::string& schedule, int64_t from_ts) {
  // 解析 5 个字段：min(0-59) hour(0-23) dom(1-31) mon(1-12) dow(0-6)
  // 每个字段类型：-2=通配(*), -1=未匹配, 0-59=字面值, >100=步进(编码 100+step)
  int fields[5] = {-1, -1, -1, -1, -1};
  std::istringstream ss(schedule);
  std::string token;
  int idx = 0;
  while (ss >> token && idx < 5) {
    if (token == "*" || token == "*/1") {
      fields[idx] = -2; // wildcard
    } else if (token.rfind("*/", 0) == 0) {
      int step = std::stoi(token.substr(2));
      fields[idx] = step > 0 ? (100 + step) : -2; // 100+step 表示步进
    } else {
      fields[idx] = std::stoi(token);
    }
    ++idx;
  }

  // 从 from_ts 的下一分钟开始扫描
  time_t t = from_ts;
  struct tm buf;
  gmtime_r(&t, &buf);
  buf.tm_sec = 0;
  buf.tm_min += 1;
  t = timegm(&buf);
  gmtime_r(&t, &buf);

  auto matches = [&](int field_idx, int value) -> bool {
    int f = fields[field_idx];
    if (f == -2) return true;          // wildcard
    if (f >= 0 && f <= 100 && value == f) return true; // literal
    if (f > 100) {                     // step: */N
      int step = f - 100;
      return value % step == 0;
    }
    return f < 0;  // unset → match any
  };

  auto next_minute = [&]() {
    buf.tm_sec = 0;
    buf.tm_min += 1;
    t = timegm(&buf);
    gmtime_r(&t, &buf);
  };

  // 最多扫 525600 分钟（1年）
  for (int i = 0; i < 525600; ++i) {
    if (!matches(0, buf.tm_min)) { next_minute(); continue; }
    // hour, dom, mon, dow 的步进只在匹配时检查
    int f_hour = fields[1];
    if (!(f_hour == -2 || f_hour < 0)) {
      if (f_hour >= 0 && f_hour <= 100 && buf.tm_hour != f_hour) { next_minute(); continue; }
      if (f_hour > 100) { int step = f_hour - 100; if (buf.tm_hour % step != 0) { next_minute(); continue; } }
    }
    int f_dom = fields[2];
    if (!(f_dom == -2 || f_dom < 0)) {
      if (f_dom >= 0 && f_dom <= 100 && buf.tm_mday != f_dom) { next_minute(); continue; }
      if (f_dom > 100) { int step = f_dom - 100; if (buf.tm_mday % step != 0) { next_minute(); continue; } }
    }
    int f_mon = fields[3];
    if (!(f_mon == -2 || f_mon < 0)) {
      int m = buf.tm_mon + 1;
      if (f_mon >= 0 && f_mon <= 100 && m != f_mon) { next_minute(); continue; }
      if (f_mon > 100) { int step = f_mon - 100; if (m % step != 0) { next_minute(); continue; } }
    }
    int f_dow = fields[4];
    if (!(f_dow == -2 || f_dow < 0)) {
      if (f_dow >= 0 && f_dow <= 100 && buf.tm_wday != f_dow) { next_minute(); continue; }
      if (f_dow > 100) { int step = f_dow - 100; if (buf.tm_wday % step != 0) { next_minute(); continue; } }
    }
    return timegm(&buf);
  }
  return -1;
}

/// 检测 ISO 时间戳：形如 "2026-06-01T09:00:00"。返回绝对时间戳（秒），或 -1。
int64_t parse_iso_timestamp(const std::string& s) {
  if (s.find('T') == std::string::npos || s.size() < 16) return -1;
  struct tm tm_buf = {};
  int yr, mo, dy, hr, mi, se;
  if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mi, &se) == 6) {
    tm_buf.tm_year = yr - 1900;
    tm_buf.tm_mon = mo - 1;
    tm_buf.tm_mday = dy;
    tm_buf.tm_hour = hr;
    tm_buf.tm_min = mi;
    tm_buf.tm_sec = se;
    tm_buf.tm_isdst = -1;
    time_t t = timegm(&tm_buf);
    if (t > 0) return static_cast<int64_t>(t);
  }
  return -1;
}

}  // namespace

CronScheduler::CronScheduler() {
  if (sqlite3_open(":memory:", &db_) != SQLITE_OK) {
    sqlite3_close(db_); db_ = nullptr;  // v0.53.74: 失败释放句柄
  }
  ensure_db();
}

CronScheduler::~CronScheduler() {
  stop();
  if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

void CronScheduler::ensure_db() {
  if (!db_) return;
  const char* sql = R"(
    CREATE TABLE IF NOT EXISTS cron_tasks (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      name TEXT NOT NULL,
      schedule TEXT NOT NULL,
      prompt TEXT NOT NULL DEFAULT '',
      enabled INTEGER NOT NULL DEFAULT 1,
      last_run_ts INTEGER NOT NULL DEFAULT 0,
      next_run_ts INTEGER NOT NULL DEFAULT 0,
      run_count INTEGER NOT NULL DEFAULT 0,
      created_at TEXT NOT NULL DEFAULT (datetime('now')),
      updated_at TEXT NOT NULL DEFAULT (datetime('now')),
      max_repeat INTEGER NOT NULL DEFAULT 0,
      no_agent INTEGER NOT NULL DEFAULT 0,
      no_agent_script TEXT NOT NULL DEFAULT '',
      deliver_to TEXT NOT NULL DEFAULT 'chat',
      workdir TEXT NOT NULL DEFAULT ''
    );
    CREATE INDEX IF NOT EXISTS idx_cron_next_run ON cron_tasks(next_run_ts, enabled);
  )";
  sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);

  // v0.42.0: 尝试添加新列（已有表则忽略）
  auto add_col = [&](const char* col_def) {
    std::string alter = "ALTER TABLE cron_tasks ADD COLUMN ";
    alter += col_def;
    sqlite3_exec(db_, alter.c_str(), nullptr, nullptr, nullptr);
  };
  add_col("max_repeat INTEGER NOT NULL DEFAULT 0");
  add_col("no_agent INTEGER NOT NULL DEFAULT 0");
  add_col("no_agent_script TEXT NOT NULL DEFAULT ''");
  add_col("deliver_to TEXT NOT NULL DEFAULT 'chat'");
  add_col("workdir TEXT NOT NULL DEFAULT ''");
  add_col("model TEXT NOT NULL DEFAULT ''");
  add_col("provider TEXT NOT NULL DEFAULT ''");
  add_col("context_from INTEGER NOT NULL DEFAULT 0");
  add_col("skills TEXT NOT NULL DEFAULT ''");
  add_col("toolsets TEXT NOT NULL DEFAULT ''");

  // v0.42.1: 任务输出存储表（供 context_from 引用）
  const char* out_sql = R"(
    CREATE TABLE IF NOT EXISTS cron_outputs (
      task_key TEXT NOT NULL,
      timestamp INTEGER NOT NULL,
      output TEXT NOT NULL DEFAULT '',
      PRIMARY KEY (task_key, timestamp)
    );
  )";
  sqlite3_exec(db_, out_sql, nullptr, nullptr, nullptr);
}

void CronScheduler::start(const std::string& db_path,
                          TaskCallback callback,
                          int tick_ms) {
  if (!db_path.empty() && db_path != ":memory:") {
    if (db_) sqlite3_close(db_);
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
      sqlite3_close(db_); db_ = nullptr;  // v0.53.74
    }
    ensure_db();
  }
  db_path_ = db_path;
  callback_ = std::move(callback);
  tick_ms_ = tick_ms;

  // v0.53.3: start 幂等——插件静态实例被多 AgentService 复用时（单测
  // 多实例/同进程多服务），二次 start 直接覆盖旧 joinable ticker →
  // std::thread operator= terminate（生产实证）。已运行则仅刷新参数。
  if (running_ && ticker_ && ticker_->joinable()) return;
  running_ = true;
  ticker_ = std::make_unique<std::thread>(&CronScheduler::ticker_loop, this);
}

void CronScheduler::clear_callback() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  callback_ = nullptr;
}

void CronScheduler::stop() {
  running_ = false;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);  // v0.49.2: 与 wait_for 共用 mu_，notify 立即唤醒
    stop_cv_.notify_all();
  }
  if (ticker_ && ticker_->joinable()) {
    ticker_->join();
    ticker_.reset();
  }
}

void CronScheduler::init_db() {
  if (!db_) {
    std::string path = db_path_.empty() ? ":memory:" : db_path_;
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
      sqlite3_close(db_); db_ = nullptr;  // v0.53.74
    }
  }
  ensure_db();
}

int CronScheduler::schema_version() const {
  if (!db_) return 0;

  int ver = 0;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "PRAGMA user_version", -1, &stmt, nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) ver = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return ver;
}

void CronScheduler::upgrade_schema(int from_version) {
  (void)from_version;
}

void CronScheduler::ticker_loop() {
  while (running_) {
    // v0.49.2: 可中断等待——stop() notify 后立即退出，不耗满 tick
    {
      std::unique_lock<std::recursive_mutex> lk(mu_);
      stop_cv_.wait_for(lk, std::chrono::milliseconds(tick_ms_),
                        [this]() { return !running_.load(); });
    }
    if (!running_) break;

    // v0.53.46: 两段式——①锁内收集到期任务+推 next_run(防 ticker 内
    /// 重入重触发)②放锁后执行。此前 execute_task(宿主 callback=完整
    /// agent chat,LLM 分钟级)在持锁内串行——多任务同刻到期一个慢全体
    /// 等,add/remove/stop 全阻塞,stop 要等任务跑完才返回
    std::vector<nlohmann::json> due_tasks;
    {
      std::lock_guard<std::recursive_mutex> lock(mu_);
      if (!db_) continue;

      int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

      const char* sql =
          "SELECT id, name, schedule, prompt, enabled, last_run_ts, next_run_ts, "
          "run_count, max_repeat, no_agent, no_agent_script, deliver_to, workdir, "
          "model, provider, context_from, skills, toolsets "
          "FROM cron_tasks WHERE enabled = 1 AND next_run_ts <= ? ORDER BY next_run_ts ASC";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, now);

      while (sqlite3_step(stmt) == SQLITE_ROW) {
        nlohmann::json task;
        task["id"] = sqlite3_column_int(stmt, 0);
        task["name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        task["schedule"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        task["prompt"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        task["enabled"] = sqlite3_column_int(stmt, 4) != 0;
        task["last_run_ts"] = static_cast<int64_t>(sqlite3_column_int64(stmt, 5));
        task["next_run_ts"] = static_cast<int64_t>(sqlite3_column_int64(stmt, 6));
        task["run_count"] = sqlite3_column_int(stmt, 7);
        task["max_repeat"] = sqlite3_column_int(stmt, 8);
        task["no_agent"] = sqlite3_column_int(stmt, 9) != 0;
        task["no_agent_script"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        task["deliver_to"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        task["workdir"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        task["model"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        task["provider"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
        task["context_from"] = sqlite3_column_int(stmt, 15);
        task["skills"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
        task["toolsets"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 17));

        due_tasks.push_back(task);
        int task_id = task["id"].get<int>();

        // max_repeat: 达到后自动禁用
        if (task.contains("max_repeat") && task["max_repeat"].get<int>() > 0) {
          int run_count = task["run_count"].get<int>() + 1;
          if (run_count >= task["max_repeat"].get<int>()) {
            sqlite3_stmt* dis = nullptr;
            const char* ds = "UPDATE cron_tasks SET enabled = 0, run_count = ?, "
                             "last_run_ts = ?, updated_at = datetime('now') WHERE id = ?";
            if (sqlite3_prepare_v2(db_, ds, -1, &dis, nullptr) == SQLITE_OK) {
              sqlite3_bind_int(dis, 1, run_count);
              sqlite3_bind_int64(dis, 2, now);
              sqlite3_bind_int(dis, 3, task_id);
              sqlite3_step(dis);
              sqlite3_finalize(dis);
            }
            continue;
          }
        }

        int64_t interval = parse_interval_seconds(task["schedule"]);
        int64_t next = -1;
        if (interval > 0) {
          next = now + interval;
        } else if (is_cron_expr(task["schedule"])) {
          next = next_cron_match(task["schedule"], now);
        } else if (parse_iso_timestamp(task["schedule"]) > 0) {
          // ISO 一次触发后禁用
          sqlite3_stmt* dis = nullptr;
          const char* ds = "UPDATE cron_tasks SET enabled = 0, last_run_ts = ?, "
                           "run_count = run_count + 1, updated_at = datetime('now') WHERE id = ?";
          if (sqlite3_prepare_v2(db_, ds, -1, &dis, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(dis, 1, now);
            sqlite3_bind_int(dis, 2, task_id);
            sqlite3_step(dis);
            sqlite3_finalize(dis);
          }
          continue;
        }
        if (next > 0) {
          const char* upd =
              "UPDATE cron_tasks SET last_run_ts = ?, next_run_ts = ?, run_count = run_count + 1, "
              "updated_at = datetime('now') WHERE id = ?";
          sqlite3_stmt* upd_stmt = nullptr;
          if (sqlite3_prepare_v2(db_, upd, -1, &upd_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(upd_stmt, 1, now);
            sqlite3_bind_int64(upd_stmt, 2, next);
            sqlite3_bind_int(upd_stmt, 3, task_id);
            sqlite3_step(upd_stmt);
            sqlite3_finalize(upd_stmt);
          }
        }
      }
    }
      sqlite3_finalize(stmt);
    }  // v0.53.46: 锁段结束

    // ②放锁后执行——慢 callback(LLM/脚本)不再阻塞调度器与其他 API
    for (auto& task : due_tasks) {
      if (!running_) break;
      execute_task(task);
    }
  }
}

void CronScheduler::execute_task(const nlohmann::json& task) {
  // v0.53.4: 锁内取回调副本——与 clear_callback（宿主析构注销）竞态安全
  TaskCallback cb;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    cb = callback_;
  }
  if (cb) {
    cb(task);
  }
}

bool CronScheduler::is_due(const std::string& schedule, int64_t last_run_ts) const {
  int64_t interval = parse_interval_seconds(schedule);
  if (interval <= 0) return false;

  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  return (last_run_ts + interval) <= now;
}

// ── CRUD ────────────────────────────────────────

nlohmann::json CronScheduler::add_task(const std::string& name,
                                       const std::string& schedule,
                                       const std::string& prompt,
                                       bool enabled) {
  return add_task_full(name, schedule, prompt, enabled, 0, false, "", "chat", "");
}

nlohmann::json CronScheduler::add_task_full(const std::string& name,
    const std::string& schedule, const std::string& prompt, bool enabled,
    int max_repeat, bool no_agent, const std::string& no_agent_script,
    const std::string& deliver_to, const std::string& workdir,
    const std::string& model, const std::string& provider,
    int context_from, const std::string& skills,
    const std::string& toolsets) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!db_) return {};

  // v0.51.3: 入参硬校验——生产实测 "every 1s" 任务跑 103 次打满 worker
  // 与 LLM 配额（一条消息即拒绝服务）。三个防线：
  //   1. 字段长度上限（name 200 / schedule 64 / prompt 10K 字节）
  //   2. 调度间隔下限 60s（every Ns / "30m" / 裸秒数形态统一约束）
  //   3. 无法解析的表达式返回 error（原为静默空 JSON，调用方无从分辨）
  {
    constexpr size_t kMaxNameLen = 200;
    constexpr size_t kMaxScheduleLen = 64;
    constexpr size_t kMaxPromptLen = 10 * 1024;
    constexpr int64_t kMinIntervalSec = 60;  // 高频限流下限
    nlohmann::json err;
    if (name.size() > kMaxNameLen || schedule.size() > kMaxScheduleLen ||
        prompt.size() > kMaxPromptLen) {
      err["error"] = "field too long: name<=200, schedule<=64, prompt<=10240 bytes";
      err["name_len"] = name.size();
      err["schedule_len"] = schedule.size();
      err["prompt_len"] = prompt.size();
      return err;
    }
    int64_t check_interval = parse_interval_seconds(schedule);
    if (check_interval > 0 && check_interval < kMinIntervalSec) {
      err["error"] = "schedule interval too short: minimum is every 60s";
      err["interval_sec"] = check_interval;
      return err;
    }
  }

  int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  int64_t interval = parse_interval_seconds(schedule);
  int64_t next = -1;
  if (interval > 0) {
    next = now + interval;
  } else if (is_cron_expr(schedule)) {
    next = next_cron_match(schedule, now);
  } else {
    next = parse_iso_timestamp(schedule);
  }
  if (next < 0) {  // 无法解析的调度表达式
    // v0.51.3: 显式 error（原返回空 JSON，调用方无从分辨失败原因）
    nlohmann::json err;
    err["error"] = "unparseable schedule: " + schedule.substr(0, 64);
    return err;
  }

  const char* sql =
      "INSERT INTO cron_tasks (name, schedule, prompt, enabled, next_run_ts, "
      "max_repeat, no_agent, no_agent_script, deliver_to, workdir, "
      "model, provider, context_from, skills, toolsets) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  sqlite3_stmt* stmt = nullptr;
  nlohmann::json result;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, schedule.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, prompt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, enabled ? 1 : 0);
    sqlite3_bind_int64(stmt, 5, next);

    // v0.42.0: 附加字段
    sqlite3_bind_int(stmt, 6, max_repeat);
    sqlite3_bind_int(stmt, 7, no_agent ? 1 : 0);
    sqlite3_bind_text(stmt, 8, no_agent_script.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, deliver_to.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, workdir.c_str(), -1, SQLITE_TRANSIENT);
    // v0.42.1: 附加字段
    sqlite3_bind_text(stmt, 11, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, provider.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 13, context_from);
    sqlite3_bind_text(stmt, 14, skills.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 15, toolsets.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_DONE) {
      int id = static_cast<int>(sqlite3_last_insert_rowid(db_));
      result["id"] = id;
      result["name"] = name;
      result["schedule"] = schedule;
      result["prompt"] = prompt;
      result["enabled"] = enabled;
      result["next_run_ts"] = next;
      result["run_count"] = 0;
      result["max_repeat"] = max_repeat;
      result["no_agent"] = no_agent;
      result["no_agent_script"] = no_agent_script;
      result["deliver_to"] = deliver_to;
      result["workdir"] = workdir;
      result["model"] = model;
      result["provider"] = provider;
      result["context_from"] = context_from;
      result["skills"] = skills;
      result["toolsets"] = toolsets;
    }
  }
  sqlite3_finalize(stmt);
  return result;
}

std::vector<nlohmann::json> CronScheduler::list_tasks() {
  std::lock_guard<std::recursive_mutex> lock(mu_);

  std::vector<nlohmann::json> tasks;
  if (!db_) return tasks;

  const char* sql =
      "SELECT id, name, schedule, prompt, enabled, last_run_ts, next_run_ts, run_count, "
      "max_repeat, no_agent, no_agent_script, deliver_to, workdir, "
      "model, provider, context_from, skills, toolsets "
      "FROM cron_tasks ORDER BY id";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      nlohmann::json t;
      t["id"] = sqlite3_column_int(stmt, 0);
      t["name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
      t["schedule"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
      t["prompt"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
      t["enabled"] = sqlite3_column_int(stmt, 4) != 0;
      t["last_run_ts"] = static_cast<int64_t>(sqlite3_column_int64(stmt, 5));
      t["next_run_ts"] = static_cast<int64_t>(sqlite3_column_int64(stmt, 6));
      t["run_count"] = sqlite3_column_int(stmt, 7);
      // v0.42.0: 附加字段（向前兼容旧表）
      if (sqlite3_column_count(stmt) > 8) {
        t["max_repeat"] = sqlite3_column_int(stmt, 8);
        t["no_agent"] = sqlite3_column_int(stmt, 9) != 0;
        const char* ns = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        if (ns) t["no_agent_script"] = ns;
        const char* dt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        if (dt) t["deliver_to"] = dt;
        const char* wd = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        if (wd) t["workdir"] = wd;
      }
      if (sqlite3_column_count(stmt) > 13) {
        const char* mdl = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        if (mdl) t["model"] = mdl;
        const char* prv = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
        if (prv) t["provider"] = prv;
        t["context_from"] = sqlite3_column_int(stmt, 15);
        const char* skl = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
        if (skl) t["skills"] = skl;
        const char* ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 17));
        if (ts) t["toolsets"] = ts;
      }
      tasks.push_back(t);
    }
  }
  sqlite3_finalize(stmt);
  return tasks;
}

bool CronScheduler::update_task(int task_id, const nlohmann::json& fields) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!db_) return false;

  std::ostringstream sql;
  sql << "UPDATE cron_tasks SET updated_at = datetime('now')";

  if (fields.contains("name")) sql << ", name = ?";
  if (fields.contains("schedule")) sql << ", schedule = ?";
  if (fields.contains("prompt")) sql << ", prompt = ?";
  if (fields.contains("enabled")) sql << ", enabled = ?";

  sql << " WHERE id = ?";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.str().c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  int idx = 1;
  if (fields.contains("name"))
    sqlite3_bind_text(stmt, idx++, fields["name"].get<std::string>().c_str(), -1, SQLITE_TRANSIENT);
  if (fields.contains("schedule"))
    sqlite3_bind_text(stmt, idx++, fields["schedule"].get<std::string>().c_str(), -1, SQLITE_TRANSIENT);
  if (fields.contains("prompt"))
    sqlite3_bind_text(stmt, idx++, fields["prompt"].get<std::string>().c_str(), -1, SQLITE_TRANSIENT);
  if (fields.contains("enabled"))
    sqlite3_bind_int(stmt, idx++, fields["enabled"].get<bool>() ? 1 : 0);
  sqlite3_bind_int(stmt, idx, task_id);

  bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(stmt);
  return ok;
}

bool CronScheduler::remove_task(int task_id) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!db_) return false;

  sqlite3_stmt* stmt = nullptr;
  bool ok = false;
  if (sqlite3_prepare_v2(db_, "DELETE FROM cron_tasks WHERE id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, task_id);
    ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  }
  sqlite3_finalize(stmt);
  return ok;
}

// ── v0.42.1: context_from 输出存储 ──

void CronScheduler::save_output(const std::string& task_key, int64_t timestamp,
                                 const std::string& output) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!db_) return;
  // upsert
  const char* sql =
      "INSERT OR REPLACE INTO cron_outputs (task_key, timestamp, output) "
      "VALUES (?, ?, ?)";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, task_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, timestamp);
    sqlite3_bind_text(stmt, 3, output.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

std::string CronScheduler::load_output(int task_id) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!db_) return "";
  char key[32];
  snprintf(key, sizeof(key), "task_%d", task_id);
  const char* sql =
      "SELECT output FROM cron_outputs WHERE task_key = ? ORDER BY timestamp DESC LIMIT 1";
  sqlite3_stmt* stmt = nullptr;
  std::string result;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      if (txt) result = txt;
    }
    sqlite3_finalize(stmt);
  }
  return result;
}

bool CronScheduler::set_enabled(int task_id, bool enabled) {
  return update_task(task_id, {{"enabled", enabled}});
}

bool CronScheduler::trigger_now(int task_id) {
  // v0.54.6 (R91 拍板)：**每任务串行执行锁** + 两段式（对齐 v0.53.46 对 ticker 的同一约定）。
  // 三态历史（防回归）：
  //   ≤v0.54.2  整个函数持 `mu_`（含分钟级 callback）⇒ add/remove/set_enabled/list/stats/stop
  //             全阻塞到任务跑完（R73 立项）
  //   v0.54.3   放锁执行 + "在飞即拒绝返回 false"：修了阻塞，但**丢了"排队重跑"语义**；
  //             同版本还引入一处回归——函数开头多了一句 `nlohmann::json task;`（默认构造是
  //             **null**），把锁内那份遮蔽掉，于是放锁后的 `execute_task(task)` 收到 null ⇒
  //             宿主 callback 首次 `task.value(...)` 就抛 type_error.306，"手动立即触发"
  //             变成**必然失败**（v0.54.6 一并修复，判据见 unit_cron_trigger_lock 的 D 组）
  //   v0.54.6   放锁执行 + 每任务锁**排队**：同一任务的并发二次触发等前一次跑完再跑一遍
  //             （= 旧"持 mu_ 串行"的实际效果），且不同任务互不阻塞（旧实现里慢任务 A 会拖住 B）
  std::shared_ptr<std::recursive_mutex> exec_mu;
  {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!db_) return false;
    auto it = task_exec_mu_.find(task_id);
    if (it == task_exec_mu_.end()) {
      it = task_exec_mu_.emplace(task_id, std::make_shared<std::recursive_mutex>()).first;
    }
    exec_mu = it->second;
  }  // ← 放锁：mu_ 绝不跨任务执行

  // 锁外**排队**段：同一任务串行（等前一次执行完成），随后重读任务行——与旧"持 mu_ 串行"下
  // 第二个调用者可见的状态一致（能看到前一次执行写入的 last_run_ts / run_count）。
  std::lock_guard<std::recursive_mutex> exec_lock(*exec_mu);

  nlohmann::json task;
  {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!db_) return false;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT id, name, schedule, prompt, enabled, last_run_ts, next_run_ts, run_count FROM cron_tasks WHERE id = ?", -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, task_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      task["id"] = sqlite3_column_int(stmt, 0);
      task["name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
      task["schedule"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
      task["prompt"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
      task["enabled"] = sqlite3_column_int(stmt, 4) != 0;
      task["last_run_ts"] = static_cast<int64_t>(sqlite3_column_int64(stmt, 5));
      task["next_run_ts"] = static_cast<int64_t>(sqlite3_column_int64(stmt, 6));
      task["run_count"] = sqlite3_column_int(stmt, 7);
    }
  }
  sqlite3_finalize(stmt);

  if (!task.contains("id")) return false;

  }  // ← 放锁（mu_ 绝不跨任务执行）

  execute_task(task);   // 锁外执行（callback 可能分钟级）

  // 回收：仅在**无其它等待者**时删除该任务的执行锁（map + 本地各持一份 = use_count 2）。
  // 有等待者时必须留给最后一个离开的等待者回收——若在这里无条件 erase，后来的调用者会
  // 拿到一把**新锁**，从而越过仍在排队的等待者（破坏"同一把锁排队"的语义）。
  {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = task_exec_mu_.find(task_id);
    if (it != task_exec_mu_.end() && it->second.use_count() == 2) task_exec_mu_.erase(it);
  }
  return true;
}

nlohmann::json CronScheduler::stats() const {
  nlohmann::json s;
  auto tasks = const_cast<CronScheduler*>(this)->list_tasks();
  s["total"] = tasks.size();
  int enabled = 0;
  for (const auto& t : tasks) {
    if (t.value("enabled", false)) ++enabled;
  }
  s["enabled"] = enabled;
  return s;
}

}  // namespace thin_agent
