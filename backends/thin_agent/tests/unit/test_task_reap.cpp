// unit_task_reap：崩溃/重启后的僵尸任务启动收尾（v0.53.91）
//
// 缺陷（本轮实锤）：TaskEngine 无任何启动恢复逻辑 —— 进程崩溃或重启后，库里仍处于
// running/retrying 的行**执行线程已随进程消失**，无人推进、无人重放，状态却永久
// 谎报"运行中"（UI/list_tasks 都显示在跑）。
//
// 本测试用**直接写库**真实模拟"上次进程死在半路"的库状态（比构造线程更忠实），
// 然后重新 init 一个 TaskEngine，锁住收尾契约：
//   T1 running  → failed(26011) + message 标注 interrupted by restart
//   T2 retrying → failed(26011)，且 attempts/result/error 现场被保留
//   T3 queued   → cancelled(26010)（状态机只允许 queued→running|cancelled）
//   T4 success/failed/cancelled 终态行**不受影响**（幂等/不误伤）
//   T5 二次 init 不再收尾（幂等，第二次 reap 计数为 0）
#include <sqlite3.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#include "test_macros.h"
#include "thin_agent/core/TaskEngine.h"

using namespace thin_agent;

/// 以"崩溃现场"身份直接写一行（模拟上次进程死在半路）。
/// 注意两处踩坑（写测试时实测）：①tasks 表有 UNIQUE INDEX idx_tasks_idem
/// (idempotency_key)，**多行共用 '' 会静默失败**（本函数初版就中招，只写进 1 行）；
/// ②因此必须**返回并断言错误**——sqlite 静默失败正是本项目反复收口的那类缺陷。
static std::string seed_row(const std::string& db, const std::string& id,
                            const std::string& state, int attempts,
                            const std::string& result_json, const std::string& error_json) {
  sqlite3* h = nullptr;
  if (sqlite3_open(db.c_str(), &h) != SQLITE_OK) return "open failed";
  sqlite3_busy_timeout(h, 3000);
  const char* sql =
      "INSERT INTO tasks (task_id, action, args_json, state, attempts, max_retries, code, "
      "message, result_json, error_json, idempotency_key, created_at, updated_at) "
      "VALUES (?, 'capture_photo', '{}', ?, ?, 3, 0, '', ?, ?, ?, 't', 't')";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(h, sql, -1, &st, nullptr) != SQLITE_OK) {
    std::string e = sqlite3_errmsg(h);
    sqlite3_close(h);
    return "prepare: " + e;
  }
  const std::string idem = "idem-" + id;   // 唯一：避开 idx_tasks_idem 冲突
  sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, state.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, attempts);
  sqlite3_bind_text(st, 4, result_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, error_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, idem.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(st);
  std::string err = (rc == SQLITE_DONE) ? "" : std::string(sqlite3_errmsg(h));
  sqlite3_finalize(st);
  sqlite3_close(h);
  return err;
}

int main() {
  std::error_code ec;
  std::filesystem::remove_all("data_taskreap", ec);
  std::filesystem::create_directories("data_taskreap", ec);
  const std::string db = "data_taskreap/agent_tasks.db";

  // 第一次 init：建表
  {
    TaskEngine e(std::make_shared<ActionExecutor>(nullptr));
    e.init(db);
  }

  // 模拟上次进程崩溃现场
  const std::string se1 = seed_row(db, "task-1", "running", 1, "{}", "{}");
  const std::string se2 = seed_row(db, "task-2", "retrying", 2, "{\"partial\":true}", "{\"attempt\":2,\"reason\":\"boom\"}");
  const std::string se3 = seed_row(db, "task-3", "queued", 0, "{}", "{}");
  const std::string se4 = seed_row(db, "task-4", "success", 1, "{\"ok\":1}", "{}");
  const std::string se5 = seed_row(db, "task-5", "cancelled", 0, "{}", "{}");
  ASSERT_TRUE("T0 崩溃现场写入全部成功（静默失败守卫）",
              se1.empty() && se2.empty() && se3.empty() && se4.empty() && se5.empty());

  // 重启：新引擎 init 时应自动收尾
  TaskEngine e2(std::make_shared<ActionExecutor>(nullptr));
  ASSERT_TRUE("T0 重启 init 成功", e2.init(db));

  auto t1 = e2.get_task("task-1");
  ASSERT_EQ("T1 running → failed", t1.value("state", ""), std::string("failed"));
  ASSERT_EQ("T1 错误码 26011", t1.value("code", 0), 26011);
  ASSERT_TRUE("T1 message 标注重启中断",
              t1.value("message", "").find("interrupted by restart") != std::string::npos);

  auto t2 = e2.get_task("task-2");
  ASSERT_EQ("T2 retrying → failed", t2.value("state", ""), std::string("failed"));
  ASSERT_EQ("T2 attempts 现场保留", t2.value("attempts", 0), 2);
  ASSERT_TRUE("T2 result 现场保留",
              t2.value("result", nlohmann::json::object()).value("partial", false));
  ASSERT_TRUE("T2 error 现场保留（可排障）",
              t2.value("error", nlohmann::json::object()).contains("reason"));

  auto t3 = e2.get_task("task-3");
  ASSERT_EQ("T3 queued → cancelled（状态机约束）", t3.value("state", ""), std::string("cancelled"));
  ASSERT_EQ("T3 错误码 26010", t3.value("code", 0), 26010);

  ASSERT_EQ("T4 终态 success 不受影响", e2.get_task("task-4").value("state", ""),
            std::string("success"));
  ASSERT_EQ("T4 终态 cancelled 不受影响", e2.get_task("task-5").value("state", ""),
            std::string("cancelled"));

  // T5 幂等：第二次 init 无僵尸可收
  TaskEngine e3(std::make_shared<ActionExecutor>(nullptr));
  ASSERT_TRUE("T5 三次 init 成功", e3.init(db));
  ASSERT_EQ("T5 幂等：再次收尾计数为 0",
            static_cast<long long>(e3.reap_interrupted_tasks()), 0LL);
  auto non_terminal = e3.list_tasks(50, "running");
  ASSERT_EQ("T5 库中已无 running 坑位", static_cast<long long>(non_terminal.size()), 0LL);

  return TEST_REPORT();
}
