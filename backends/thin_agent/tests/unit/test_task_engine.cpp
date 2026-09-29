// unit_task_engine：TaskEngine 提交/查询/取消/replay/审计与重试行为。

#include <filesystem>
#include <iostream>
#include <memory>
#include <unistd.h>

#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/TaskEngine.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"

namespace {
int expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    return 1;
  }
  return 0;
}
}  // namespace

int main() {
  // 使用 /tmp 唯一路径避免并行测试锁冲突
  std::string db_path = "/tmp/test_tasks_" + std::to_string(getpid()) + ".db";
  std::filesystem::remove(db_path);

  auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
  auto ex = std::make_shared<thin_agent::ActionExecutor>(dc);

  thin_agent::TaskEngine::Config cfg;
  cfg.max_retries = 2;
  cfg.action_timeout_ms = 1000;
  thin_agent::TaskEngine eng(ex, cfg);
  if (int rc = expect(eng.init(db_path), "task engine init"); rc) return rc;

  const std::string t1 = eng.submit_task("capture_photo", nlohmann::json::object(), "idem-1");
  const auto r1 = eng.get_task(t1);
  if (int rc = expect(r1.value("exists", false), "task exists"); rc) return rc;
  if (int rc = expect(r1.value("state", "") == "success", "task state success"); rc) return rc;

  const std::string t1_dup = eng.submit_task("capture_photo", nlohmann::json::object(), "idem-1");
  if (int rc = expect(t1_dup == t1, "idempotency returns same task"); rc) return rc;

  auto audits_success = eng.list_task_audits(t1, 20);
  if (int rc = expect(audits_success["exists"].get<bool>(), "audit success task exists=true"); rc) return rc;
  if (int rc = expect(audits_success["audit_latest_to_state"].get<std::string>() == "success", "audit success latest to_state=success"); rc) return rc;
  if (int rc = expect(audits_success["audit_latest_state_category"].get<std::string>() == "terminal_success", "audit success latest state_category=terminal_success"); rc) return rc;
  if (int rc = expect(audits_success["audit_latest_state_rank"].get<int>() == 2, "audit success latest state_rank=2"); rc) return rc;

  const std::string t2 = eng.submit_task("not_allowed_action", nlohmann::json::object(), "idem-2");
  const auto r2 = eng.get_task(t2);
  if (int rc = expect(r2.value("state", "") == "failed", "unsupported action failed"); rc) return rc;
  if (int rc = expect(r2.value("attempts", 0) == 3, "failed action retried max+1 times"); rc) return rc;
  if (int rc = expect(r2.value("code", 0) == 24001, "failed action code mapped to unified code 24001"); rc) return rc;

  auto audits = eng.list_task_audits(t2, 20);
  if (int rc = expect(audits["exists"].get<bool>(), "audit list existing task exists=true"); rc) return rc;
  if (int rc = expect(audits["audits"].is_array(), "audit list array"); rc) return rc;
  if (int rc = expect(audits["audits"].size() >= 4, "audit list has queued/running/retrying/failed transitions"); rc) return rc;
  bool has_retrying = false;
  bool has_failed = false;
  for (const auto& it : audits["audits"]) {
    const std::string to_state = it.value("to_state", "");
    if (to_state == "retrying") has_retrying = true;
    if (to_state == "failed") has_failed = true;
  }
  if (int rc = expect(has_retrying, "audit list contains retrying transition"); rc) return rc;
  if (int rc = expect(has_failed, "audit list contains failed transition"); rc) return rc;
  int retrying_count = 0;
  for (const auto& it : audits["audits"]) {
    if (it.value("to_state", "") == "retrying") ++retrying_count;
  }
  if (int rc = expect(retrying_count >= 2, "audit list records each retrying attempt"); rc) return rc;

  auto audits_default_from_zero = eng.list_task_audits(t2, 0);
  if (int rc = expect(audits_default_from_zero["audits"].is_array(), "audit list(0) is array"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audits"].size() >= 4, "audit list(0) uses default and returns audits"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["limit_applied"].get<int>() == 50, "audit list(0) limit_applied=50"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_count_returned"].get<int>() == static_cast<int>(audits_default_from_zero["audits"].size()), "audit list(0) audit_count_returned matches audits size"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_has_entries"].is_boolean(), "audit list(0) audit_has_entries is bool"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_has_entries"].get<bool>(), "audit list(0) audit_has_entries=true for existing task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_audit_id"].is_number_integer(), "audit list(0) audit_latest_audit_id is integer"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_audit_id"].get<int>() == audits_default_from_zero["audits"][0]["audit_id"].get<int>(), "audit list(0) audit_latest_audit_id matches first audit id"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_from_state"].is_string(), "audit list(0) audit_latest_from_state is string"); rc) return rc;
  if (int rc = expect(!audits_default_from_zero["audit_latest_from_state"].get<std::string>().empty(), "audit list(0) audit_latest_from_state non-empty for existing task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_from_state"].get<std::string>() == audits_default_from_zero["audits"][0]["from_state"].get<std::string>(), "audit list(0) audit_latest_from_state matches first audit from_state"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_to_state"].is_string(), "audit list(0) audit_latest_to_state is string"); rc) return rc;
  if (int rc = expect(!audits_default_from_zero["audit_latest_to_state"].get<std::string>().empty(), "audit list(0) audit_latest_to_state non-empty for existing task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_transition"].is_string(), "audit list(0) audit_latest_transition is string"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_transition"].get<std::string>() ==
                          audits_default_from_zero["audits"][0]["from_state"].get<std::string>() + "->" +
                              audits_default_from_zero["audits"][0]["to_state"].get<std::string>(),
                      "audit list(0) audit_latest_transition matches first audit from/to");
      rc)
    return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_terminal"].is_boolean(), "audit list(0) audit_latest_is_terminal is bool"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_terminal"].get<bool>(), "audit list(0) audit_latest_is_terminal=true for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_non_terminal"].is_boolean(), "audit list(0) audit_latest_is_non_terminal is bool"); rc) return rc;
  if (int rc = expect(!audits_default_from_zero["audit_latest_is_non_terminal"].get<bool>(), "audit list(0) audit_latest_is_non_terminal=false for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_active"].is_boolean(), "audit list(0) audit_latest_is_active is bool"); rc) return rc;
  if (int rc = expect(!audits_default_from_zero["audit_latest_is_active"].get<bool>(), "audit list(0) audit_latest_is_active=false for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_active"].get<bool>() ==
                          audits_default_from_zero["audit_latest_is_non_terminal"].get<bool>(),
                      "audit list(0) audit_latest_is_active equals audit_latest_is_non_terminal");
      rc)
    return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_active"].get<bool>() ==
                          (audits_default_from_zero["audit_latest_is_queued"].get<bool>() ||
                           audits_default_from_zero["audit_latest_is_running"].get<bool>() ||
                           audits_default_from_zero["audit_latest_is_retrying"].get<bool>()),
                      "audit list(0) audit_latest_is_active equals queued/running/retrying disjunction");
      rc)
    return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_queued"].is_boolean(), "audit list(0) audit_latest_is_queued is bool"); rc) return rc;
  if (int rc = expect(!audits_default_from_zero["audit_latest_is_queued"].get<bool>(), "audit list(0) audit_latest_is_queued=false for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_running"].is_boolean(), "audit list(0) audit_latest_is_running is bool"); rc) return rc;
  if (int rc = expect(!audits_default_from_zero["audit_latest_is_running"].get<bool>(), "audit list(0) audit_latest_is_running=false for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_retrying"].is_boolean(), "audit list(0) audit_latest_is_retrying is bool"); rc) return rc;
  if (int rc = expect(!audits_default_from_zero["audit_latest_is_retrying"].get<bool>(), "audit list(0) audit_latest_is_retrying=false for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_success"].is_boolean(), "audit list(0) audit_latest_is_success is bool"); rc) return rc;
  if (int rc = expect(!audits_default_from_zero["audit_latest_is_success"].get<bool>(), "audit list(0) audit_latest_is_success=false for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_failed"].is_boolean(), "audit list(0) audit_latest_is_failed is bool"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_failed"].get<bool>(), "audit list(0) audit_latest_is_failed=true for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_cancelled"].is_boolean(), "audit list(0) audit_latest_is_cancelled is bool"); rc) return rc;
  if (int rc = expect(!audits_default_from_zero["audit_latest_is_cancelled"].get<bool>(), "audit list(0) audit_latest_is_cancelled=false for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_done"].is_boolean(), "audit list(0) audit_latest_is_done is bool"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_done"].get<bool>(), "audit list(0) audit_latest_is_done=true for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_failure_terminal"].is_boolean(), "audit list(0) audit_latest_is_failure_terminal is bool"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_failure_terminal"].get<bool>(), "audit list(0) audit_latest_is_failure_terminal=true for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_state_category"].is_string(), "audit list(0) audit_latest_state_category is string"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_state_category"].get<std::string>() == "terminal_failure", "audit list(0) audit_latest_state_category=terminal_failure for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_state_rank"].is_number_integer(), "audit list(0) audit_latest_state_rank is integer"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_state_rank"].get<int>() == 3, "audit list(0) audit_latest_state_rank=3 for terminal_failure"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_has_started"].is_boolean(), "audit list(0) audit_latest_has_started is bool"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_has_started"].get<bool>(), "audit list(0) audit_latest_has_started=true for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_is_in_progress"].is_boolean(), "audit list(0) audit_latest_is_in_progress is bool"); rc) return rc;
  if (int rc = expect(!audits_default_from_zero["audit_latest_is_in_progress"].get<bool>(), "audit list(0) audit_latest_is_in_progress=false for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_has_error"].is_boolean(), "audit list(0) audit_latest_has_error is bool"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_has_error"].get<bool>(), "audit list(0) audit_latest_has_error=true for failed task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_code"].is_number_integer(), "audit list(0) audit_latest_code is integer"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_code"].get<int>() == audits_default_from_zero["audits"][0]["code"].get<int>(), "audit list(0) audit_latest_code matches first audit code"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_attempts"].is_number_integer(), "audit list(0) audit_latest_attempts is integer"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_attempts"].get<int>() == audits_default_from_zero["audits"][0]["attempts"].get<int>(), "audit list(0) audit_latest_attempts matches first audit attempts"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_message"].is_string(), "audit list(0) audit_latest_message is string"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_message"].get<std::string>() == audits_default_from_zero["audits"][0]["message"].get<std::string>(), "audit list(0) audit_latest_message matches first audit message"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_created_at"].is_string(), "audit list(0) audit_latest_created_at is string"); rc) return rc;
  if (int rc = expect(!audits_default_from_zero["audit_latest_created_at"].get<std::string>().empty(), "audit list(0) audit_latest_created_at non-empty for existing task"); rc) return rc;
  if (int rc = expect(audits_default_from_zero["audit_latest_created_at"].get<std::string>() == audits_default_from_zero["audits"][0]["created_at"].get<std::string>(), "audit list(0) audit_latest_created_at matches first audit created_at"); rc) return rc;

  auto audits_negative = eng.list_task_audits(t2, -9);
  if (int rc = expect(audits_negative["audits"].is_array(), "audit list(-9) is array"); rc) return rc;
  if (int rc = expect(audits_negative["audits"].size() == audits_default_from_zero["audits"].size(), "audit list negative limit default consistency"); rc) return rc;
  if (int rc = expect(audits_negative["limit_applied"].get<int>() == 50, "audit list(-9) limit_applied=50"); rc) return rc;

  auto audits_clamped = eng.list_task_audits(t2, 1000);
  if (int rc = expect(audits_clamped["audits"].is_array(), "audit list(1000) is array"); rc) return rc;
  if (int rc = expect(audits_clamped["audits"].size() == audits_default_from_zero["audits"].size(), "audit list limit is clamped to 200"); rc) return rc;
  if (int rc = expect(audits_clamped["limit_applied"].get<int>() == 200, "audit list(1000) limit_applied=200"); rc) return rc;
  if (int rc = expect(audits_clamped["audit_count_returned"].get<int>() == static_cast<int>(audits_clamped["audits"].size()), "audit list(1000) audit_count_returned matches audits size"); rc) return rc;

  auto audits_limit_one = eng.list_task_audits(t1, 1);
  if (int rc = expect(audits_limit_one["audits"].is_array(), "audit list(1) is array"); rc) return rc;
  if (int rc = expect(audits_limit_one["limit_applied"].get<int>() == 1, "audit list(1) limit_applied=1"); rc) return rc;
  if (int rc = expect(audits_limit_one["audits"].size() <= 1, "audit list(1) returns <=1 entry"); rc) return rc;
  if (int rc = expect(audits_limit_one["audit_count_returned"].get<int>() == static_cast<int>(audits_limit_one["audits"].size()), "audit list(1) audit_count_returned matches audits size"); rc) return rc;
  if (int rc = expect(audits_limit_one["audit_latest_transition"].get<std::string>() ==
                          audits_limit_one["audits"][0]["from_state"].get<std::string>() + "->" +
                              audits_limit_one["audits"][0]["to_state"].get<std::string>(),
                      "audit list(1) latest_transition matches first audit from/to");
      rc)
    return rc;
  if (int rc = expect(audits_limit_one["audit_latest_to_state"].get<std::string>() == "success", "audit list(1) latest_to_state=success"); rc) return rc;
  if (int rc = expect(audits_limit_one["audit_latest_to_state"].get<std::string>() == audits_limit_one["audits"][0]["to_state"].get<std::string>(), "audit list(1) latest_to_state matches first audit to_state"); rc) return rc;
  if (int rc = expect(audits_limit_one["audit_latest_state_category"].get<std::string>() == "terminal_success", "audit list(1) state_category=terminal_success"); rc) return rc;
  if (int rc = expect(audits_limit_one["audit_latest_state_rank"].get<int>() == 2, "audit list(1) state_rank=2 for terminal_success"); rc) return rc;
  if (int rc = expect(audits_limit_one["audit_latest_is_terminal"].get<bool>(), "audit list(1) is_terminal=true for terminal_success"); rc) return rc;
  if (int rc = expect(!audits_limit_one["audit_latest_is_non_terminal"].get<bool>(), "audit list(1) is_non_terminal=false for terminal_success"); rc) return rc;
  if (int rc = expect(audits_limit_one["audit_latest_is_active"].is_boolean(), "audit list(1) is_active is bool"); rc) return rc;
  if (int rc = expect(!audits_limit_one["audit_latest_is_active"].get<bool>(), "audit list(1) is_active=false for terminal_success"); rc) return rc;
  if (int rc = expect(audits_limit_one["audit_latest_is_active"].get<bool>() ==
                          audits_limit_one["audit_latest_is_non_terminal"].get<bool>(),
                      "audit list(1) audit_latest_is_active equals audit_latest_is_non_terminal");
      rc)
    return rc;
  if (int rc = expect(audits_limit_one["audit_latest_is_active"].get<bool>() ==
                          (audits_limit_one["audit_latest_is_queued"].get<bool>() ||
                           audits_limit_one["audit_latest_is_running"].get<bool>() ||
                           audits_limit_one["audit_latest_is_retrying"].get<bool>()),
                      "audit list(1) audit_latest_is_active equals queued/running/retrying disjunction");
      rc)
    return rc;
  if (int rc = expect(audits_limit_one["audit_latest_is_success"].get<bool>(), "audit list(1) is_success=true for terminal_success"); rc) return rc;
  if (int rc = expect(!audits_limit_one["audit_latest_is_failed"].get<bool>(), "audit list(1) is_failed=false for terminal_success"); rc) return rc;
  if (int rc = expect(audits_limit_one["audit_latest_is_done"].get<bool>() ==
                          (audits_limit_one["audit_latest_is_success"].get<bool>() ||
                           audits_limit_one["audit_latest_is_failed"].get<bool>() ||
                           audits_limit_one["audit_latest_is_cancelled"].get<bool>()),
                      "audit list(1) audit_latest_is_done equals success/failed/cancelled disjunction");
      rc)
    return rc;
  if (int rc = expect(audits_limit_one["audit_latest_is_failure_terminal"].get<bool>() ==
                          (audits_limit_one["audit_latest_is_failed"].get<bool>() ||
                           audits_limit_one["audit_latest_is_cancelled"].get<bool>()),
                      "audit list(1) audit_latest_is_failure_terminal equals failed/cancelled disjunction");
      rc)
    return rc;
  if (int rc = expect(audits_limit_one["audit_latest_is_in_progress"].is_boolean(), "audit list(1) is_in_progress is bool"); rc) return rc;
  if (int rc = expect(!audits_limit_one["audit_latest_is_in_progress"].get<bool>(), "audit list(1) is_in_progress=false for terminal_success"); rc) return rc;
  if (int rc = expect(audits_limit_one["audit_latest_is_in_progress"].get<bool>() ==
                          (audits_limit_one["audit_latest_is_running"].get<bool>() ||
                           audits_limit_one["audit_latest_is_retrying"].get<bool>()),
                      "audit list(1) audit_latest_is_in_progress equals running/retrying disjunction");
      rc)
    return rc;
  if (int rc = expect(audits_limit_one["audit_latest_is_done"].get<bool>(), "audit list(1) is_done=true for terminal_success"); rc) return rc;
  if (int rc = expect(!audits_limit_one["audit_latest_is_failure_terminal"].get<bool>(), "audit list(1) is_failure_terminal=false for terminal_success"); rc) return rc;
  if (int rc = expect(audits_limit_one["audit_latest_has_started"].get<bool>(), "audit list(1) has_started=true for terminal_success"); rc) return rc;
  if (int rc = expect(!audits_limit_one["audit_latest_has_error"].get<bool>(), "audit list(1) has_error=false for terminal_success"); rc) return rc;
  if (int rc = expect(audits_limit_one["audit_latest_code"].get<int>() == 0, "audit list(1) latest_code=0 for terminal_success"); rc) return rc;

  auto audits_missing_task = eng.list_task_audits("task-missing", 10);
  if (int rc = expect(audits_missing_task["task_id"] == "task-missing", "audit missing task id echo"); rc) return rc;
  if (int rc = expect(!audits_missing_task["exists"].get<bool>(), "audit missing task exists=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["limit_applied"].get<int>() == 10, "audit missing task limit_applied passthrough"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_count_returned"].get<int>() == 0, "audit missing task audit_count_returned=0"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_has_entries"].is_boolean(), "audit missing task audit_has_entries is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_has_entries"].get<bool>(), "audit missing task audit_has_entries=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_audit_id"].is_number_integer(), "audit missing task audit_latest_audit_id is integer"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_audit_id"].get<int>() == -1, "audit missing task audit_latest_audit_id=-1"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_from_state"].is_string(), "audit missing task audit_latest_from_state is string"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_from_state"].get<std::string>().empty(), "audit missing task audit_latest_from_state empty"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_to_state"].is_string(), "audit missing task audit_latest_to_state is string"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_to_state"].get<std::string>().empty(), "audit missing task audit_latest_to_state empty"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_transition"].is_string(), "audit missing task audit_latest_transition is string"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_transition"].get<std::string>().empty(), "audit missing task audit_latest_transition empty"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_terminal"].is_boolean(), "audit missing task audit_latest_is_terminal is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_is_terminal"].get<bool>(), "audit missing task audit_latest_is_terminal=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_non_terminal"].is_boolean(), "audit missing task audit_latest_is_non_terminal is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_is_non_terminal"].get<bool>(), "audit missing task audit_latest_is_non_terminal=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_active"].is_boolean(), "audit missing task audit_latest_is_active is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_is_active"].get<bool>(), "audit missing task audit_latest_is_active=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_active"].get<bool>() ==
                          audits_missing_task["audit_latest_is_non_terminal"].get<bool>(),
                      "audit missing task audit_latest_is_active equals audit_latest_is_non_terminal");
      rc)
    return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_active"].get<bool>() ==
                          (audits_missing_task["audit_latest_is_queued"].get<bool>() ||
                           audits_missing_task["audit_latest_is_running"].get<bool>() ||
                           audits_missing_task["audit_latest_is_retrying"].get<bool>()),
                      "audit missing task audit_latest_is_active equals queued/running/retrying disjunction");
      rc)
    return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_queued"].is_boolean(), "audit missing task audit_latest_is_queued is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_is_queued"].get<bool>(), "audit missing task audit_latest_is_queued=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_running"].is_boolean(), "audit missing task audit_latest_is_running is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_is_running"].get<bool>(), "audit missing task audit_latest_is_running=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_retrying"].is_boolean(), "audit missing task audit_latest_is_retrying is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_is_retrying"].get<bool>(), "audit missing task audit_latest_is_retrying=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_success"].is_boolean(), "audit missing task audit_latest_is_success is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_is_success"].get<bool>(), "audit missing task audit_latest_is_success=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_done"].get<bool>() ==
                          (audits_missing_task["audit_latest_is_success"].get<bool>() ||
                           audits_missing_task["audit_latest_is_failed"].get<bool>() ||
                           audits_missing_task["audit_latest_is_cancelled"].get<bool>()),
                      "audit missing task audit_latest_is_done equals success/failed/cancelled disjunction");
      rc)
    return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_in_progress"].is_boolean(), "audit missing task audit_latest_is_in_progress is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_is_in_progress"].get<bool>(), "audit missing task audit_latest_is_in_progress=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_in_progress"].get<bool>() ==
                          (audits_missing_task["audit_latest_is_running"].get<bool>() ||
                           audits_missing_task["audit_latest_is_retrying"].get<bool>()),
                      "audit missing task audit_latest_is_in_progress equals running/retrying disjunction");
      rc)
    return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_failed"].is_boolean(), "audit missing task audit_latest_is_failed is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_is_failed"].get<bool>(), "audit missing task audit_latest_is_failed=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_cancelled"].is_boolean(), "audit missing task audit_latest_is_cancelled is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_is_cancelled"].get<bool>(), "audit missing task audit_latest_is_cancelled=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_done"].is_boolean(), "audit missing task audit_latest_is_done is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_is_done"].get<bool>(), "audit missing task audit_latest_is_done=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_failure_terminal"].is_boolean(), "audit missing task audit_latest_is_failure_terminal is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_is_failure_terminal"].get<bool>(), "audit missing task audit_latest_is_failure_terminal=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_state_category"].is_string(), "audit missing task audit_latest_state_category is string"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_state_category"].get<std::string>() == "none", "audit missing task audit_latest_state_category=none"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_state_rank"].is_number_integer(), "audit missing task audit_latest_state_rank is integer"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_state_rank"].get<int>() == 0, "audit missing task audit_latest_state_rank=0"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_has_started"].is_boolean(), "audit missing task audit_latest_has_started is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_has_started"].get<bool>(), "audit missing task audit_latest_has_started=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_is_in_progress"].is_boolean(), "audit missing task audit_latest_is_in_progress is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_is_in_progress"].get<bool>(), "audit missing task audit_latest_is_in_progress=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_has_error"].is_boolean(), "audit missing task audit_latest_has_error is bool"); rc) return rc;
  if (int rc = expect(!audits_missing_task["audit_latest_has_error"].get<bool>(), "audit missing task audit_latest_has_error=false"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_code"].is_number_integer(), "audit missing task audit_latest_code is integer"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_code"].get<int>() == -1, "audit missing task audit_latest_code=-1"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_attempts"].is_number_integer(), "audit missing task audit_latest_attempts is integer"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_attempts"].get<int>() == -1, "audit missing task audit_latest_attempts=-1"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_message"].is_string(), "audit missing task audit_latest_message is string"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_message"].get<std::string>().empty(), "audit missing task audit_latest_message empty"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_created_at"].is_string(), "audit missing task audit_latest_created_at is string"); rc) return rc;
  if (int rc = expect(audits_missing_task["audit_latest_created_at"].get<std::string>().empty(), "audit missing task audit_latest_created_at empty"); rc) return rc;
  if (int rc = expect(audits_missing_task["audits"].is_array(), "audit missing task is array"); rc) return rc;
  if (int rc = expect(audits_missing_task["audits"].empty(), "audit missing task returns empty audits"); rc) return rc;

  auto replay = eng.replay_task(t1, "idem-replay-1");
  if (int rc = expect(replay.value("exists", false), "replay exists"); rc) return rc;
  if (int rc = expect(replay["replayed_task_id"].get<std::string>() != t1, "replay new task id"); rc) return rc;

  // reopen should continue task seq without PRIMARY KEY collision
  thin_agent::TaskEngine eng2(ex, cfg);
  if (int rc = expect(eng2.init(db_path), "engine reopen init"); rc) return rc;
  const std::string t3 = eng2.submit_task("capture_photo", nlohmann::json::object(), "idem-3");
  if (int rc = expect(t3 != t1 && t3 != t2, "task_id sequence resumes after reopen"); rc) return rc;

  auto cancel = eng.cancel_task(replay["replayed_task_id"].get<std::string>(), "user stop");
  if (int rc = expect(cancel.value("exists", false), "cancel target exists"); rc) return rc;
  if (int rc = expect(cancel.value("state", "") == "success", "cancel on terminal task should be rejected"); rc) return rc;
  if (int rc = expect(!cancel.value("cancelled", true), "cancel rejected flag on terminal task"); rc) return rc;

  auto lst = eng.list_tasks(10);
  if (int rc = expect(lst.is_array(), "list is array"); rc) return rc;
  if (int rc = expect(lst.size() >= 2, "list has tasks"); rc) return rc;

  auto lst_default_from_zero = eng.list_tasks(0);
  if (int rc = expect(lst_default_from_zero.is_array(), "list(0) is array"); rc) return rc;
  if (int rc = expect(lst_default_from_zero.size() >= 2, "list(0) uses default and returns tasks"); rc) return rc;

  auto lst_negative = eng.list_tasks(-3);
  if (int rc = expect(lst_negative.is_array(), "list(-3) is array"); rc) return rc;
  if (int rc = expect(lst_negative.size() == lst_default_from_zero.size(), "list negative limit default consistency"); rc) return rc;

  auto lst_clamped = eng.list_tasks(1000);
  if (int rc = expect(lst_clamped.is_array(), "list(1000) is array"); rc) return rc;
  if (int rc = expect(lst_clamped.size() == lst_default_from_zero.size(), "list limit is clamped to 200"); rc) return rc;

  auto lst_failed = eng.list_tasks(10, "failed", "");
  if (int rc = expect(lst_failed.is_array(), "list failed is array"); rc) return rc;
  if (int rc = expect(!lst_failed.empty(), "list failed not empty"); rc) return rc;
  for (const auto& it : lst_failed) {
    if (int rc = expect(it.value("state", "") == "failed", "list failed state filter"); rc) return rc;
  }

  auto lst_capture = eng.list_tasks(10, "", "capture_photo");
  if (int rc = expect(lst_capture.is_array(), "list capture is array"); rc) return rc;
  if (int rc = expect(!lst_capture.empty(), "list capture not empty"); rc) return rc;
  for (const auto& it : lst_capture) {
    if (int rc = expect(it.value("action", "") == "capture_photo", "list action filter"); rc) return rc;
  }

  auto lst_failed_action = eng.list_tasks(10, "failed", "not_allowed_action");
  if (int rc = expect(lst_failed_action.is_array(), "list failed+action is array"); rc) return rc;
  if (int rc = expect(lst_failed_action.size() == 1, "list failed+action has one task"); rc) return rc;

  int filtered_limit_applied_default = -1;
  auto lst_failed_action_default_limit = eng.list_tasks(0, "failed", "not_allowed_action", &filtered_limit_applied_default);
  if (int rc = expect(lst_failed_action_default_limit.is_array(), "list failed+action(0) is array"); rc) return rc;
  if (int rc = expect(filtered_limit_applied_default == 20, "list failed+action(0) limit_applied=20"); rc) return rc;
  if (int rc = expect(lst_failed_action_default_limit.size() == lst_failed_action.size(), "list failed+action(0) size equals explicit limit"); rc) return rc;

  int filtered_limit_applied_clamped = -1;
  auto lst_failed_action_clamped = eng.list_tasks(1000, "failed", "not_allowed_action", &filtered_limit_applied_clamped);
  if (int rc = expect(lst_failed_action_clamped.is_array(), "list failed+action(1000) is array"); rc) return rc;
  if (int rc = expect(filtered_limit_applied_clamped == 200, "list failed+action(1000) limit_applied=200"); rc) return rc;
  if (int rc = expect(lst_failed_action_clamped.size() == lst_failed_action.size(), "list failed+action(1000) size equals explicit limit"); rc) return rc;

  int filtered_limit_applied_negative = -1;
  auto lst_failed_action_negative = eng.list_tasks(-5, "failed", "not_allowed_action", &filtered_limit_applied_negative);
  if (int rc = expect(lst_failed_action_negative.is_array(), "list failed+action(-5) is array"); rc) return rc;
  if (int rc = expect(filtered_limit_applied_negative == 20, "list failed+action(-5) limit_applied=20"); rc) return rc;
  if (int rc = expect(lst_failed_action_negative.size() == lst_failed_action.size(), "list failed+action(-5) size equals explicit limit"); rc) return rc;

  std::cout << "unit:test_task_engine PASS\n";
  return 0;
}
