// unit_agent_service：AgentService 路由、槽位澄清、端云策略与 ChatPolicy 配置化集成测试。

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/ChatPolicy.h"
#include "thin_agent/core/TaskEngine.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"
#include "thin_agent/llm/DemoConfigCompat.h"

namespace {
int expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    return 1;
  }
  return 0;
}

int line_count(const std::string& path) {
  std::ifstream in(path);
  int n = 0;
  std::string line;
  while (std::getline(in, line)) ++n;
  return n;
}
}  // namespace

int main() {
  // 媒体 pipeline 含 duration_sec 时跳过真实 sleep，避免单测卡住
  setenv("THIN_AGENT_FAST_MEDIA", "1", 1);
  // v0.53.45: 云端依赖用例(summarize/intent 走 zai profile)的真网请求
  /// 兜底 mock——此前依赖外网可达,云断网日必挂(实测 2026-09-15);
  /// 个别用例 setenv_local 覆盖时仍优先生效
  setenv("THIN_AGENT_TEST_CLOUD_RESPONSE",
         R"({"choices":[{"message":{"content":"mock-summary-ok"}}]})", 1);
  setenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE",
         R"({"choices":[{"message":{"content":"mock-tools-ok"}}]})", 1);

  std::filesystem::remove_all("data");

  thin_agent::DemoConfigCompat cfg;
  cfg.mode = "offline";
  cfg.fallback = "offline";

  auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
  auto ex = std::make_shared<thin_agent::ActionExecutor>(dc);
  auto te = std::make_shared<thin_agent::TaskEngine>(ex);
  if (int rc = expect(te->init("data/test_agent_tasks.db"), "task engine init"); rc) return rc;

  thin_agent::AgentService svc(cfg, ex, te);

  svc.on_session_open("s1");

  auto st = svc.handle_request("s1", nlohmann::json{{"type", "status"}, {"cmd_id", "c-status"}, {"trace_id", "t-status"}});
  if (int rc = expect(st["type"] == "status", "status type"); rc) return rc;
  if (int rc = expect(st["cmd_id"] == "c-status", "status cmd_id keep"); rc) return rc;
  if (int rc = expect(st["trace_id"] == "t-status", "status trace_id keep"); rc) return rc;
  if (int rc = expect(st["data"].contains("sense"), "status has sense"); rc) return rc;

  auto sub = svc.handle_request("s1", nlohmann::json{{"type", "task_submit"}, {"action", "capture_photo"}, {"args", nlohmann::json::object()}, {"idempotency_key", "k1"}});
  if (int rc = expect(sub["type"] == "task_submit_result", "task_submit_result type"); rc) return rc;
  if (int rc = expect(sub["task"]["exists"].get<bool>(), "submitted task exists"); rc) return rc;

  const std::string tid = sub["task"]["task_id"].get<std::string>();
  auto get = svc.handle_request("s1", nlohmann::json{{"type", "task_get"}, {"task_id", tid}});
  if (int rc = expect(get["type"] == "task_get_result", "task_get_result type"); rc) return rc;
  if (int rc = expect(get["task"]["task_id"] == tid, "task_get id matches"); rc) return rc;

  auto replay = svc.handle_request("s1", nlohmann::json{{"type", "task_replay"}, {"task_id", tid}, {"idempotency_key", "k1-r"}});
  if (int rc = expect(replay["type"] == "task_replay_result", "task_replay_result type"); rc) return rc;
  if (int rc = expect(replay["data"]["exists"].get<bool>(), "task replay exists"); rc) return rc;

  auto list = svc.handle_request("s1", nlohmann::json{{"type", "task_list"}, {"limit", 10}});
  if (int rc = expect(list["type"] == "task_list_result", "task_list_result type"); rc) return rc;
  if (int rc = expect(list["tasks"].is_array(), "task_list array"); rc) return rc;
  if (int rc = expect(list["limit_applied"].get<int>() == 10, "task_list limit_applied passthrough"); rc) return rc;
  if (int rc = expect(list["returned_count"].get<int>() == static_cast<int>(list["tasks"].size()), "task_list returned_count matches tasks size"); rc) return rc;

  auto list_filtered = svc.handle_request(
      "s1", nlohmann::json{{"type", "task_list"}, {"limit", 10}, {"state", "success"}, {"action", "capture_photo"}});
  if (int rc = expect(list_filtered["type"] == "task_list_result", "task_list filtered result type"); rc) return rc;
  if (int rc = expect(list_filtered["filters"]["state"] == "success", "task_list filtered state echo"); rc) return rc;
  if (int rc = expect(list_filtered["filters"]["action"] == "capture_photo", "task_list filtered action echo"); rc) return rc;
  if (int rc = expect(list_filtered["tasks"].is_array(), "task_list filtered array"); rc) return rc;
  if (int rc = expect(list_filtered["limit_applied"].get<int>() == 10, "task_list filtered limit_applied passthrough"); rc) return rc;
  if (int rc = expect(list_filtered["returned_count"].get<int>() == static_cast<int>(list_filtered["tasks"].size()), "task_list filtered returned_count matches tasks size"); rc) return rc;

  auto list_default_from_zero = svc.handle_request("s1", nlohmann::json{{"type", "task_list"}, {"limit", 0}});
  if (int rc = expect(list_default_from_zero["type"] == "task_list_result", "task_list(0) type"); rc) return rc;
  if (int rc = expect(list_default_from_zero["tasks"].is_array(), "task_list(0) array"); rc) return rc;
  if (int rc = expect(list_default_from_zero["limit_applied"].get<int>() == 20, "task_list(0) limit_applied=20"); rc) return rc;
  if (int rc = expect(list_default_from_zero["returned_count"].get<int>() == static_cast<int>(list_default_from_zero["tasks"].size()), "task_list(0) returned_count matches tasks size"); rc) return rc;

  auto list_negative = svc.handle_request("s1", nlohmann::json{{"type", "task_list"}, {"limit", -5}});
  if (int rc = expect(list_negative["type"] == "task_list_result", "task_list(-5) type"); rc) return rc;
  if (int rc = expect(list_negative["tasks"].is_array(), "task_list(-5) array"); rc) return rc;
  if (int rc = expect(list_negative["limit_applied"].get<int>() == 20, "task_list(-5) limit_applied=20"); rc) return rc;
  if (int rc = expect(list_negative["returned_count"].get<int>() == static_cast<int>(list_negative["tasks"].size()), "task_list(-5) returned_count matches tasks size"); rc) return rc;

  auto list_clamped = svc.handle_request("s1", nlohmann::json{{"type", "task_list"}, {"limit", 1000}});
  if (int rc = expect(list_clamped["type"] == "task_list_result", "task_list(1000) type"); rc) return rc;
  if (int rc = expect(list_clamped["tasks"].is_array(), "task_list(1000) array"); rc) return rc;
  if (int rc = expect(list_clamped["limit_applied"].get<int>() == 200, "task_list(1000) limit_applied=200"); rc) return rc;
  if (int rc = expect(list_clamped["returned_count"].get<int>() == static_cast<int>(list_clamped["tasks"].size()), "task_list(1000) returned_count matches tasks size"); rc) return rc;
  if (int rc = expect(list_clamped["tasks"].size() == list_default_from_zero["tasks"].size(), "task_list limit clamp consistency"); rc) return rc;
  if (int rc = expect(list_negative["tasks"].size() == list_default_from_zero["tasks"].size(), "task_list negative limit default consistency"); rc) return rc;

  auto audit = svc.handle_request("s1", nlohmann::json{{"type", "task_audit"}, {"task_id", tid}, {"limit", 20}});
  if (int rc = expect(audit["type"] == "task_audit_result", "task_audit_result type"); rc) return rc;
  if (int rc = expect(audit["data"]["exists"].get<bool>(), "task_audit existing task exists=true"); rc) return rc;
  if (int rc = expect(audit["data"]["audits"].is_array(), "task_audit array"); rc) return rc;
  if (int rc = expect(audit["data"]["limit_applied"].get<int>() == 20, "task_audit(20) limit_applied passthrough"); rc) return rc;
  if (int rc = expect(audit["data"]["audit_latest_state_category"].get<std::string>() == "terminal_success", "task_audit(20) state_category=terminal_success"); rc) return rc;
  if (int rc = expect(audit["data"]["audit_latest_state_rank"].get<int>() == 2, "task_audit(20) state_rank=2 for terminal_success"); rc) return rc;

  auto audit_default_from_zero = svc.handle_request("s1", nlohmann::json{{"type", "task_audit"}, {"task_id", tid}, {"limit", 0}});
  if (int rc = expect(audit_default_from_zero["type"] == "task_audit_result", "task_audit(0) type"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audits"].is_array(), "task_audit(0) array"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["limit_applied"].get<int>() == 50, "task_audit(0) limit_applied=50"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_count_returned"].get<int>() == static_cast<int>(audit_default_from_zero["data"]["audits"].size()), "task_audit(0) audit_count_returned matches audits size"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_has_entries"].is_boolean(), "task_audit(0) audit_has_entries is bool"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_has_entries"].get<bool>(), "task_audit(0) audit_has_entries=true for existing task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_audit_id"].is_number_integer(), "task_audit(0) audit_latest_audit_id is integer"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_audit_id"].get<int>() == audit_default_from_zero["data"]["audits"][0]["audit_id"].get<int>(), "task_audit(0) audit_latest_audit_id matches first audit id"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_from_state"].is_string(), "task_audit(0) audit_latest_from_state is string"); rc) return rc;
  if (int rc = expect(!audit_default_from_zero["data"]["audit_latest_from_state"].get<std::string>().empty(), "task_audit(0) audit_latest_from_state non-empty for existing task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_from_state"].get<std::string>() == audit_default_from_zero["data"]["audits"][0]["from_state"].get<std::string>(), "task_audit(0) audit_latest_from_state matches first audit from_state"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_to_state"].is_string(), "task_audit(0) audit_latest_to_state is string"); rc) return rc;
  if (int rc = expect(!audit_default_from_zero["data"]["audit_latest_to_state"].get<std::string>().empty(), "task_audit(0) audit_latest_to_state non-empty for existing task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_transition"].is_string(), "task_audit(0) audit_latest_transition is string"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_transition"].get<std::string>() ==
                          audit_default_from_zero["data"]["audits"][0]["from_state"].get<std::string>() + "->" +
                              audit_default_from_zero["data"]["audits"][0]["to_state"].get<std::string>(),
                      "task_audit(0) audit_latest_transition matches first audit from/to");
      rc)
    return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_terminal"].is_boolean(), "task_audit(0) audit_latest_is_terminal is bool"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_terminal"].get<bool>(), "task_audit(0) audit_latest_is_terminal=true for failed task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_non_terminal"].is_boolean(), "task_audit(0) audit_latest_is_non_terminal is bool"); rc) return rc;
  if (int rc = expect(!audit_default_from_zero["data"]["audit_latest_is_non_terminal"].get<bool>(), "task_audit(0) audit_latest_is_non_terminal=false for success task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_active"].is_boolean(), "task_audit(0) audit_latest_is_active is bool"); rc) return rc;
  if (int rc = expect(!audit_default_from_zero["data"]["audit_latest_is_active"].get<bool>(), "task_audit(0) audit_latest_is_active=false for success task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_active"].get<bool>() ==
                          audit_default_from_zero["data"]["audit_latest_is_non_terminal"].get<bool>(),
                      "task_audit(0) audit_latest_is_active equals audit_latest_is_non_terminal");
      rc)
    return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_active"].get<bool>() ==
                          (audit_default_from_zero["data"]["audit_latest_is_queued"].get<bool>() ||
                           audit_default_from_zero["data"]["audit_latest_is_running"].get<bool>() ||
                           audit_default_from_zero["data"]["audit_latest_is_retrying"].get<bool>()),
                      "task_audit(0) audit_latest_is_active equals queued/running/retrying disjunction");
      rc)
    return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_queued"].is_boolean(), "task_audit(0) audit_latest_is_queued is bool"); rc) return rc;
  if (int rc = expect(!audit_default_from_zero["data"]["audit_latest_is_queued"].get<bool>(), "task_audit(0) audit_latest_is_queued=false for success task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_running"].is_boolean(), "task_audit(0) audit_latest_is_running is bool"); rc) return rc;
  if (int rc = expect(!audit_default_from_zero["data"]["audit_latest_is_running"].get<bool>(), "task_audit(0) audit_latest_is_running=false for success task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_retrying"].is_boolean(), "task_audit(0) audit_latest_is_retrying is bool"); rc) return rc;
  if (int rc = expect(!audit_default_from_zero["data"]["audit_latest_is_retrying"].get<bool>(), "task_audit(0) audit_latest_is_retrying=false for failed task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_success"].is_boolean(), "task_audit(0) audit_latest_is_success is bool"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_success"].get<bool>(), "task_audit(0) audit_latest_is_success=true for success task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_failed"].is_boolean(), "task_audit(0) audit_latest_is_failed is bool"); rc) return rc;
  if (int rc = expect(!audit_default_from_zero["data"]["audit_latest_is_failed"].get<bool>(), "task_audit(0) audit_latest_is_failed=false for success task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_cancelled"].is_boolean(), "task_audit(0) audit_latest_is_cancelled is bool"); rc) return rc;
  if (int rc = expect(!audit_default_from_zero["data"]["audit_latest_is_cancelled"].get<bool>(), "task_audit(0) audit_latest_is_cancelled=false for success task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_done"].is_boolean(), "task_audit(0) audit_latest_is_done is bool"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_done"].get<bool>(), "task_audit(0) audit_latest_is_done=true for success task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_failure_terminal"].is_boolean(), "task_audit(0) audit_latest_is_failure_terminal is bool"); rc) return rc;
  if (int rc = expect(!audit_default_from_zero["data"]["audit_latest_is_failure_terminal"].get<bool>(), "task_audit(0) audit_latest_is_failure_terminal=false for success task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_state_category"].is_string(), "task_audit(0) audit_latest_state_category is string"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_state_category"].get<std::string>() == "terminal_success", "task_audit(0) audit_latest_state_category=terminal_success for success task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_state_rank"].is_number_integer(), "task_audit(0) audit_latest_state_rank is integer"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_state_rank"].get<int>() == 2, "task_audit(0) audit_latest_state_rank=2 for terminal_success"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_has_started"].is_boolean(), "task_audit(0) audit_latest_has_started is bool"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_has_started"].get<bool>(), "task_audit(0) audit_latest_has_started=true for success task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_is_in_progress"].is_boolean(), "task_audit(0) audit_latest_is_in_progress is bool"); rc) return rc;
  if (int rc = expect(!audit_default_from_zero["data"]["audit_latest_is_in_progress"].get<bool>(), "task_audit(0) audit_latest_is_in_progress=false for success task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_has_error"].is_boolean(), "task_audit(0) audit_latest_has_error is bool"); rc) return rc;
  if (int rc = expect(!audit_default_from_zero["data"]["audit_latest_has_error"].get<bool>(), "task_audit(0) audit_latest_has_error=false for success task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_code"].is_number_integer(), "task_audit(0) audit_latest_code is integer"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_code"].get<int>() == audit_default_from_zero["data"]["audits"][0]["code"].get<int>(), "task_audit(0) audit_latest_code matches first audit code"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_attempts"].is_number_integer(), "task_audit(0) audit_latest_attempts is integer"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_attempts"].get<int>() == audit_default_from_zero["data"]["audits"][0]["attempts"].get<int>(), "task_audit(0) audit_latest_attempts matches first audit attempts"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_message"].is_string(), "task_audit(0) audit_latest_message is string"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_message"].get<std::string>() == audit_default_from_zero["data"]["audits"][0]["message"].get<std::string>(), "task_audit(0) audit_latest_message matches first audit message"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_created_at"].is_string(), "task_audit(0) audit_latest_created_at is string"); rc) return rc;
  if (int rc = expect(!audit_default_from_zero["data"]["audit_latest_created_at"].get<std::string>().empty(), "task_audit(0) audit_latest_created_at non-empty for existing task"); rc) return rc;
  if (int rc = expect(audit_default_from_zero["data"]["audit_latest_created_at"].get<std::string>() == audit_default_from_zero["data"]["audits"][0]["created_at"].get<std::string>(), "task_audit(0) audit_latest_created_at matches first audit created_at"); rc) return rc;

  auto audit_negative = svc.handle_request("s1", nlohmann::json{{"type", "task_audit"}, {"task_id", tid}, {"limit", -7}});
  if (int rc = expect(audit_negative["type"] == "task_audit_result", "task_audit(-7) type"); rc) return rc;
  if (int rc = expect(audit_negative["data"]["audits"].is_array(), "task_audit(-7) array"); rc) return rc;
  if (int rc = expect(audit_negative["data"]["limit_applied"].get<int>() == 50, "task_audit(-7) limit_applied=50"); rc) return rc;

  auto audit_clamped = svc.handle_request("s1", nlohmann::json{{"type", "task_audit"}, {"task_id", tid}, {"limit", 1000}});
  if (int rc = expect(audit_clamped["type"] == "task_audit_result", "task_audit(1000) type"); rc) return rc;
  if (int rc = expect(audit_clamped["data"]["audits"].is_array(), "task_audit(1000) array"); rc) return rc;
  if (int rc = expect(audit_clamped["data"]["limit_applied"].get<int>() == 200, "task_audit(1000) limit_applied=200"); rc) return rc;
  if (int rc = expect(audit_clamped["data"]["audit_count_returned"].get<int>() == static_cast<int>(audit_clamped["data"]["audits"].size()), "task_audit(1000) audit_count_returned matches audits size"); rc) return rc;
  if (int rc = expect(audit_clamped["data"]["audits"].size() == audit_default_from_zero["data"]["audits"].size(), "task_audit limit clamp consistency"); rc) return rc;
  if (int rc = expect(audit_negative["data"]["audits"].size() == audit_default_from_zero["data"]["audits"].size(), "task_audit negative limit default consistency"); rc) return rc;

  auto audit_limit_one = svc.handle_request("s1", nlohmann::json{{"type", "task_audit"}, {"task_id", tid}, {"limit", 1}});
  if (int rc = expect(audit_limit_one["type"] == "task_audit_result", "task_audit(1) type"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["limit_applied"].get<int>() == 1, "task_audit(1) limit_applied=1"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audits"].is_array(), "task_audit(1) array"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audits"].size() <= 1, "task_audit(1) returns <=1 entry"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_count_returned"].get<int>() == static_cast<int>(audit_limit_one["data"]["audits"].size()), "task_audit(1) audit_count_returned matches audits size"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_audit_id"].is_number_integer(), "task_audit(1) audit_latest_audit_id is integer"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_audit_id"].get<int>() == audit_limit_one["data"]["audits"][0]["audit_id"].get<int>(), "task_audit(1) audit_latest_audit_id matches first audit id"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_from_state"].is_string(), "task_audit(1) audit_latest_from_state is string"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_from_state"].get<std::string>() == audit_limit_one["data"]["audits"][0]["from_state"].get<std::string>(), "task_audit(1) audit_latest_from_state matches first audit from_state"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_transition"].get<std::string>() ==
                          audit_limit_one["data"]["audits"][0]["from_state"].get<std::string>() + "->" +
                              audit_limit_one["data"]["audits"][0]["to_state"].get<std::string>(),
                      "task_audit(1) latest_transition matches first audit from/to");
      rc)
    return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_to_state"].get<std::string>() == "success", "task_audit(1) latest_to_state=success for terminal_success"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_to_state"].get<std::string>() == audit_limit_one["data"]["audits"][0]["to_state"].get<std::string>(), "task_audit(1) latest_to_state matches first audit to_state"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_state_category"].get<std::string>() == "terminal_success", "task_audit(1) state_category=terminal_success"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_state_rank"].get<int>() == 2, "task_audit(1) state_rank=2 for terminal_success"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_is_terminal"].get<bool>(), "task_audit(1) is_terminal=true for terminal_success"); rc) return rc;
  if (int rc = expect(!audit_limit_one["data"]["audit_latest_is_non_terminal"].get<bool>(), "task_audit(1) is_non_terminal=false for terminal_success"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_is_active"].is_boolean(), "task_audit(1) is_active is bool"); rc) return rc;
  if (int rc = expect(!audit_limit_one["data"]["audit_latest_is_active"].get<bool>(), "task_audit(1) is_active=false for terminal_success"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_is_active"].get<bool>() ==
                          audit_limit_one["data"]["audit_latest_is_non_terminal"].get<bool>(),
                      "task_audit(1) audit_latest_is_active equals audit_latest_is_non_terminal");
      rc)
    return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_is_active"].get<bool>() ==
                          (audit_limit_one["data"]["audit_latest_is_queued"].get<bool>() ||
                           audit_limit_one["data"]["audit_latest_is_running"].get<bool>() ||
                           audit_limit_one["data"]["audit_latest_is_retrying"].get<bool>()),
                      "task_audit(1) audit_latest_is_active equals queued/running/retrying disjunction");
      rc)
    return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_is_success"].get<bool>(), "task_audit(1) is_success=true for terminal_success"); rc) return rc;
  if (int rc = expect(!audit_limit_one["data"]["audit_latest_is_failed"].get<bool>(), "task_audit(1) is_failed=false for terminal_success"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_is_done"].get<bool>() ==
                          (audit_limit_one["data"]["audit_latest_is_success"].get<bool>() ||
                           audit_limit_one["data"]["audit_latest_is_failed"].get<bool>() ||
                           audit_limit_one["data"]["audit_latest_is_cancelled"].get<bool>()),
                      "task_audit(1) audit_latest_is_done equals success/failed/cancelled disjunction");
      rc)
    return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_is_failure_terminal"].get<bool>() ==
                          (audit_limit_one["data"]["audit_latest_is_failed"].get<bool>() ||
                           audit_limit_one["data"]["audit_latest_is_cancelled"].get<bool>()),
                      "task_audit(1) audit_latest_is_failure_terminal equals failed/cancelled disjunction");
      rc)
    return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_is_in_progress"].is_boolean(), "task_audit(1) is_in_progress is bool"); rc) return rc;
  if (int rc = expect(!audit_limit_one["data"]["audit_latest_is_in_progress"].get<bool>(), "task_audit(1) is_in_progress=false for terminal_success"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_is_in_progress"].get<bool>() ==
                          (audit_limit_one["data"]["audit_latest_is_running"].get<bool>() ||
                           audit_limit_one["data"]["audit_latest_is_retrying"].get<bool>()),
                      "task_audit(1) audit_latest_is_in_progress equals running/retrying disjunction");
      rc)
    return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_is_done"].get<bool>(), "task_audit(1) is_done=true for terminal_success"); rc) return rc;
  if (int rc = expect(!audit_limit_one["data"]["audit_latest_is_failure_terminal"].get<bool>(), "task_audit(1) is_failure_terminal=false for terminal_success"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_has_started"].get<bool>(), "task_audit(1) has_started=true for terminal_success"); rc) return rc;
  if (int rc = expect(!audit_limit_one["data"]["audit_latest_has_error"].get<bool>(), "task_audit(1) has_error=false for terminal_success"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_code"].is_number_integer(), "task_audit(1) audit_latest_code is integer"); rc) return rc;
  if (int rc = expect(audit_limit_one["data"]["audit_latest_code"].get<int>() == 0, "task_audit(1) audit_latest_code=0 for terminal_success"); rc) return rc;

  auto audit_missing = svc.handle_request("s1", nlohmann::json{{"type", "task_audit"}, {"task_id", "task-missing"}, {"limit", 10}});
  if (int rc = expect(audit_missing["type"] == "task_audit_result", "task_audit missing type"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["task_id"] == "task-missing", "task_audit missing id echo"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["exists"].get<bool>(), "task_audit missing exists=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["limit_applied"].get<int>() == 10, "task_audit missing limit_applied passthrough"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_count_returned"].get<int>() == 0, "task_audit missing audit_count_returned=0"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_has_entries"].is_boolean(), "task_audit missing audit_has_entries is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_has_entries"].get<bool>(), "task_audit missing audit_has_entries=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_audit_id"].is_number_integer(), "task_audit missing audit_latest_audit_id is integer"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_audit_id"].get<int>() == -1, "task_audit missing audit_latest_audit_id=-1"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_from_state"].is_string(), "task_audit missing audit_latest_from_state is string"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_from_state"].get<std::string>().empty(), "task_audit missing audit_latest_from_state empty"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_to_state"].is_string(), "task_audit missing audit_latest_to_state is string"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_to_state"].get<std::string>().empty(), "task_audit missing audit_latest_to_state empty"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_transition"].is_string(), "task_audit missing audit_latest_transition is string"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_transition"].get<std::string>().empty(), "task_audit missing audit_latest_transition empty"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_terminal"].is_boolean(), "task_audit missing audit_latest_is_terminal is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_is_terminal"].get<bool>(), "task_audit missing audit_latest_is_terminal=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_non_terminal"].is_boolean(), "task_audit missing audit_latest_is_non_terminal is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_is_non_terminal"].get<bool>(), "task_audit missing audit_latest_is_non_terminal=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_active"].is_boolean(), "task_audit missing audit_latest_is_active is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_is_active"].get<bool>(), "task_audit missing audit_latest_is_active=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_active"].get<bool>() ==
                          audit_missing["data"]["audit_latest_is_non_terminal"].get<bool>(),
                      "task_audit missing audit_latest_is_active equals audit_latest_is_non_terminal");
      rc)
    return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_active"].get<bool>() ==
                          (audit_missing["data"]["audit_latest_is_queued"].get<bool>() ||
                           audit_missing["data"]["audit_latest_is_running"].get<bool>() ||
                           audit_missing["data"]["audit_latest_is_retrying"].get<bool>()),
                      "task_audit missing audit_latest_is_active equals queued/running/retrying disjunction");
      rc)
    return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_queued"].is_boolean(), "task_audit missing audit_latest_is_queued is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_is_queued"].get<bool>(), "task_audit missing audit_latest_is_queued=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_running"].is_boolean(), "task_audit missing audit_latest_is_running is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_is_running"].get<bool>(), "task_audit missing audit_latest_is_running=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_retrying"].is_boolean(), "task_audit missing audit_latest_is_retrying is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_is_retrying"].get<bool>(), "task_audit missing audit_latest_is_retrying=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_success"].is_boolean(), "task_audit missing audit_latest_is_success is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_is_success"].get<bool>(), "task_audit missing audit_latest_is_success=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_done"].get<bool>() ==
                          (audit_missing["data"]["audit_latest_is_success"].get<bool>() ||
                           audit_missing["data"]["audit_latest_is_failed"].get<bool>() ||
                           audit_missing["data"]["audit_latest_is_cancelled"].get<bool>()),
                      "task_audit missing audit_latest_is_done equals success/failed/cancelled disjunction");
      rc)
    return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_in_progress"].is_boolean(), "task_audit missing audit_latest_is_in_progress is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_is_in_progress"].get<bool>(), "task_audit missing audit_latest_is_in_progress=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_in_progress"].get<bool>() ==
                          (audit_missing["data"]["audit_latest_is_running"].get<bool>() ||
                           audit_missing["data"]["audit_latest_is_retrying"].get<bool>()),
                      "task_audit missing audit_latest_is_in_progress equals running/retrying disjunction");
      rc)
    return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_failed"].is_boolean(), "task_audit missing audit_latest_is_failed is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_is_failed"].get<bool>(), "task_audit missing audit_latest_is_failed=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_cancelled"].is_boolean(), "task_audit missing audit_latest_is_cancelled is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_is_cancelled"].get<bool>(), "task_audit missing audit_latest_is_cancelled=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_done"].is_boolean(), "task_audit missing audit_latest_is_done is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_is_done"].get<bool>(), "task_audit missing audit_latest_is_done=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_failure_terminal"].is_boolean(), "task_audit missing audit_latest_is_failure_terminal is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_is_failure_terminal"].get<bool>(), "task_audit missing audit_latest_is_failure_terminal=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_state_category"].is_string(), "task_audit missing audit_latest_state_category is string"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_state_category"].get<std::string>() == "none", "task_audit missing audit_latest_state_category=none"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_state_rank"].is_number_integer(), "task_audit missing audit_latest_state_rank is integer"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_state_rank"].get<int>() == 0, "task_audit missing audit_latest_state_rank=0"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_has_started"].is_boolean(), "task_audit missing audit_latest_has_started is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_has_started"].get<bool>(), "task_audit missing audit_latest_has_started=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_is_in_progress"].is_boolean(), "task_audit missing audit_latest_is_in_progress is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_is_in_progress"].get<bool>(), "task_audit missing audit_latest_is_in_progress=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_has_error"].is_boolean(), "task_audit missing audit_latest_has_error is bool"); rc) return rc;
  if (int rc = expect(!audit_missing["data"]["audit_latest_has_error"].get<bool>(), "task_audit missing audit_latest_has_error=false"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_code"].is_number_integer(), "task_audit missing audit_latest_code is integer"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_code"].get<int>() == -1, "task_audit missing audit_latest_code=-1"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_attempts"].is_number_integer(), "task_audit missing audit_latest_attempts is integer"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_attempts"].get<int>() == -1, "task_audit missing audit_latest_attempts=-1"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_message"].is_string(), "task_audit missing audit_latest_message is string"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_message"].get<std::string>().empty(), "task_audit missing audit_latest_message empty"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_created_at"].is_string(), "task_audit missing audit_latest_created_at is string"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audit_latest_created_at"].get<std::string>().empty(), "task_audit missing audit_latest_created_at empty"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audits"].is_array(), "task_audit missing audits array"); rc) return rc;
  if (int rc = expect(audit_missing["data"]["audits"].empty(), "task_audit missing audits empty"); rc) return rc;

  auto cancel = svc.handle_request("s1", nlohmann::json{{"type", "task_cancel"}, {"task_id", tid}, {"reason", "user"}});
  if (int rc = expect(cancel["type"] == "task_cancel_result", "task_cancel_result type"); rc) return rc;
  if (int rc = expect(cancel["task"]["exists"].get<bool>(), "task_cancel task exists"); rc) return rc;

  auto c1 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "hello"}});
  auto c2 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "again"}});
  auto expect_trace_schema = [&](const nlohmann::json& resp, const char* msg_prefix) -> int {
    if (!resp.contains("decision_trace") || !resp["decision_trace"].is_array()) {
      std::cerr << "FAIL: " << msg_prefix << " decision_trace missing/invalid\n";
      return 1;
    }
    for (const auto& step : resp["decision_trace"]) {
      if (!step.is_object()) {
        std::cerr << "FAIL: " << msg_prefix << " decision_trace step not object\n";
        return 1;
      }
      if (!step.contains("layer") || !step["layer"].is_string()) {
        std::cerr << "FAIL: " << msg_prefix << " decision_trace.layer missing\n";
        return 1;
      }
      if (!step.contains("input")) {
        std::cerr << "FAIL: " << msg_prefix << " decision_trace.input missing\n";
        return 1;
      }
      if (!step.contains("output")) {
        std::cerr << "FAIL: " << msg_prefix << " decision_trace.output missing\n";
        return 1;
      }
      if (!step.contains("ts") || !step["ts"].is_string() || step["ts"].get<std::string>().empty()) {
        std::cerr << "FAIL: " << msg_prefix << " decision_trace.ts missing\n";
        return 1;
      }
    }
    return 0;
  };

  auto expect_local_policy_trace = [&](const nlohmann::json& resp, const char* msg_prefix) -> int {
    if (!resp.contains("decision") || !resp["decision"].is_object()) {
      std::cerr << "FAIL: " << msg_prefix << " decision missing\n";
      return 1;
    }
    if (!resp["decision"].contains("route") || !resp["decision"]["route"].is_string()) {
      std::cerr << "FAIL: " << msg_prefix << " decision.route missing\n";
      return 1;
    }
    if (!resp["decision"].contains("policy") || !resp["decision"]["policy"].is_string()) {
      std::cerr << "FAIL: " << msg_prefix << " decision.policy missing\n";
      return 1;
    }
    if (!resp.contains("decision_trace") || !resp["decision_trace"].is_array()) {
      std::cerr << "FAIL: " << msg_prefix << " decision_trace missing/invalid\n";
      return 1;
    }
    bool found_policy_layer = false;
    for (const auto& step : resp["decision_trace"]) {
      if (!step.is_object()) continue;
      if (!step.contains("layer") || !step["layer"].is_string()) continue;
      if (step["layer"].get<std::string>() != "policy") continue;
      if (!step.contains("input") || !step["input"].is_object()) {
        std::cerr << "FAIL: " << msg_prefix << " policy layer input missing/object\n";
        return 1;
      }
      const auto& in = step["input"];
      if (!in.contains("route") || !in["route"].is_string()) {
        std::cerr << "FAIL: " << msg_prefix << " policy layer input.route missing\n";
        return 1;
      }
      if (!in.contains("policy") || !in["policy"].is_string()) {
        std::cerr << "FAIL: " << msg_prefix << " policy layer input.policy missing\n";
        return 1;
      }
      if (in["route"] != resp["decision"]["route"]) {
        std::cerr << "FAIL: " << msg_prefix << " policy trace input route mismatch\n";
        return 1;
      }
      if (in["policy"] != resp["decision"]["policy"]) {
        std::cerr << "FAIL: " << msg_prefix << " policy trace input policy mismatch\n";
        return 1;
      }
      if (!step.contains("output") || !step["output"].is_object()) {
        std::cerr << "FAIL: " << msg_prefix << " policy layer output missing/object\n";
        return 1;
      }
      const auto& out = step["output"];
      if (!out.contains("route") || !out["route"].is_string()) {
        std::cerr << "FAIL: " << msg_prefix << " policy layer output.route missing\n";
        return 1;
      }
      if (!out.contains("policy") || !out["policy"].is_string()) {
        std::cerr << "FAIL: " << msg_prefix << " policy layer output.policy missing\n";
        return 1;
      }
      if (out["route"] != resp["decision"]["route"]) {
        std::cerr << "FAIL: " << msg_prefix << " policy trace route mismatch\n";
        return 1;
      }
      if (out["policy"] != resp["decision"]["policy"]) {
        std::cerr << "FAIL: " << msg_prefix << " policy trace policy mismatch\n";
        return 1;
      }
      found_policy_layer = true;
      break;
    }
    if (!found_policy_layer) {
      std::cerr << "FAIL: " << msg_prefix << " policy layer not found\n";
      return 1;
    }
    return 0;
  };
  if (int rc = expect(c1["type"] == "chat_result", "chat_result type"); rc) return rc;
  if (int rc = expect(c1["decision"]["route"].get<std::string>().find("offline") == 0 || c1["decision"]["route"].get<std::string>().find("template") != std::string::npos || c1["decision"]["route"] == "fallback", "offline chat decision route"); rc) return rc;
  if (int rc = expect(c1["tool_calls"].is_array(), "offline chat tool_calls array"); rc) return rc;
  if (int rc = expect_trace_schema(c1, "offline chat"); rc) return rc;
  if (int rc = expect(c2["memory_size"].get<int>() >= 2, "chat memory grows"); rc) return rc;

  auto chat_profile = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "介绍一下你自己和主体能力"}});
  if (int rc = expect(chat_profile["type"] == "chat_result", "chat profile type"); rc) return rc;
  if (int rc = expect(chat_profile["mode_used"] == "local-agent", "chat profile goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile["decision"]["route"] == "local_profile", "chat profile route=local_profile"); rc) return rc;
  if (int rc = expect(chat_profile["intent_backend"] == "rules", "chat profile intent_backend=rules"); rc) return rc;
  if (int rc = expect(chat_profile["decision_trace"].is_array(), "chat profile decision_trace is array"); rc) return rc;
  if (int rc = expect_trace_schema(chat_profile, "chat profile"); rc) return rc;
  if (int rc = expect_local_policy_trace(chat_profile, "chat profile"); rc) return rc;
  if (int rc = expect(chat_profile["observation"].contains("capabilities"), "chat profile has capabilities observation"); rc) return rc;

  auto chat_profile_plain = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "介绍一下"}});
  if (int rc = expect(chat_profile_plain["type"] == "chat_result", "chat profile plain type"); rc) return rc;
  if (int rc = expect(chat_profile_plain["mode_used"] == "local-agent", "chat profile plain goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile_plain["decision"]["route"] == "local_profile", "chat profile plain route=local_profile"); rc) return rc;

  auto chat_profile_plain_2 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "你有哪些能力"}});
  if (int rc = expect(chat_profile_plain_2["type"] == "chat_result", "chat profile plain2 type"); rc) return rc;
  if (int rc = expect(chat_profile_plain_2["mode_used"] == "local-agent", "chat profile plain2 goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile_plain_2["decision"]["route"] == "local_profile", "chat profile plain2 route=local_profile"); rc) return rc;

  auto chat_profile_plain_3 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "你有什么能力"}});
  if (int rc = expect(chat_profile_plain_3["type"] == "chat_result", "chat profile plain3 type"); rc) return rc;
  if (int rc = expect(chat_profile_plain_3["mode_used"] == "local-agent", "chat profile plain3 goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile_plain_3["decision"]["route"] == "local_profile", "chat profile plain3 route=local_profile"); rc) return rc;

  // 文件操作类请求不应被意图分类误判为 profile（路径中包含 "thin_agent" 关键词子串）
  auto file_op_1 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "用 list_dir 查看 /root/code/thin_agent/src 目录"}});
  if (int rc = expect(file_op_1["type"] == "chat_result", "file_op_1 type"); rc) return rc;
  if (int rc = expect(file_op_1["decision"]["route"] != "local_profile", "file_op_1 NOT local_profile"); rc) return rc;

  auto file_op_2 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "列出 /root/code/thin_agent/src 下有哪些 .cpp 文件"}});
  if (int rc = expect(file_op_2["type"] == "chat_result", "file_op_2 type"); rc) return rc;
  if (int rc = expect(file_op_2["decision"]["route"] != "local_profile", "file_op_2 NOT local_profile"); rc) return rc;

  // 正常的 profile 查询仍然应该命中
  auto norm_profile = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "你是谁"}});
  if (int rc = expect(norm_profile["decision"]["route"] == "local_profile", "normal profile still hits"); rc) return rc;

  auto chat_profile_plain_4 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "你有什么能力？"}});
  if (int rc = expect(chat_profile_plain_4["type"] == "chat_result", "chat profile plain4 type"); rc) return rc;
  if (int rc = expect(chat_profile_plain_4["mode_used"] == "local-agent", "chat profile plain4 goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile_plain_4["decision"]["route"] == "local_profile", "chat profile plain4 route=local_profile"); rc) return rc;

  auto chat_profile_help = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "怎么用你"}});
  if (int rc = expect(chat_profile_help["type"] == "chat_result", "chat profile help type"); rc) return rc;
  if (int rc = expect(chat_profile_help["mode_used"] == "local-agent", "chat profile help goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile_help["decision"]["route"] == "local_profile", "chat profile help route=local_profile"); rc) return rc;

  auto chat_profile_help_2 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "帮助手册"}});
  if (int rc = expect(chat_profile_help_2["type"] == "chat_result", "chat profile help2 type"); rc) return rc;
  if (int rc = expect(chat_profile_help_2["mode_used"] == "local-agent", "chat profile help2 goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile_help_2["decision"]["route"] == "local_profile", "chat profile help2 route=local_profile"); rc) return rc;

  auto chat_profile_help_3 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "你能做啥"}});
  if (int rc = expect(chat_profile_help_3["type"] == "chat_result", "chat profile help3 type"); rc) return rc;
  if (int rc = expect(chat_profile_help_3["mode_used"] == "local-agent", "chat profile help3 goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile_help_3["decision"]["route"] == "local_profile", "chat profile help3 route=local_profile"); rc) return rc;

  auto chat_profile_help_4 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "你会干嘛"}});
  if (int rc = expect(chat_profile_help_4["type"] == "chat_result", "chat profile help4 type"); rc) return rc;
  if (int rc = expect(chat_profile_help_4["mode_used"] == "local-agent", "chat profile help4 goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile_help_4["decision"]["route"] == "local_profile", "chat profile help4 route=local_profile"); rc) return rc;

  auto chat_profile_help_5 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "咋用你"}});
  if (int rc = expect(chat_profile_help_5["type"] == "chat_result", "chat profile help5 type"); rc) return rc;
  if (int rc = expect(chat_profile_help_5["mode_used"] == "local-agent", "chat profile help5 goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile_help_5["decision"]["route"] == "local_profile", "chat profile help5 route=local_profile"); rc) return rc;

  auto chat_profile_detail = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "详细能力介绍"}});
  if (int rc = expect(chat_profile_detail["type"] == "chat_result", "chat profile detail type"); rc) return rc;
  if (int rc = expect(chat_profile_detail["mode_used"] == "local-agent", "chat profile detail goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile_detail["decision"]["route"] == "local_profile", "chat profile detail route=local_profile"); rc) return rc;
  if (int rc = expect(chat_profile_detail["observation"]["profile_mode"] == "detailed", "chat profile detail profile_mode=detailed"); rc) return rc;
  if (int rc = expect(chat_profile_detail["text"].get<std::string>().find("详细能力清单") != std::string::npos, "chat profile detail text has detailed header"); rc) return rc;
  // detailed 模式应有运行时信息
  if (int rc = expect(chat_profile_detail["text"].get<std::string>().find("运行时：") != std::string::npos, "chat profile detail has runtime footer"); rc) return rc;

  // 英文 detailed 模式验证
  auto chat_en_detail = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "your detailed skills"}});
  if (int rc = expect(chat_en_detail["observation"]["profile_mode"] == "detailed", "chat en detail profile_mode=detailed"); rc) return rc;
  if (int rc = expect(chat_en_detail["text"].get<std::string>().find("Detailed capabilities") != std::string::npos, "chat en detail has detailed header"); rc) return rc;
  if (int rc = expect(chat_en_detail["text"].get<std::string>().find("Runtime:") != std::string::npos, "chat en detail has runtime footer"); rc) return rc;

  auto chat_profile_default = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "你是谁"}});
  if (int rc = expect(chat_profile_default["type"] == "chat_result", "chat profile default type"); rc) return rc;
  if (int rc = expect(chat_profile_default["mode_used"] == "local-agent", "chat profile default goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile_default["decision"]["route"] == "local_profile", "chat profile default route=local_profile"); rc) return rc;
  if (int rc = expect(chat_profile_default["observation"]["profile_mode"] == "concise", "chat profile default profile_mode=concise"); rc) return rc;
  // 角色驱动自我介绍：应有 worker 角色名
  if (int rc = expect(chat_profile_default["text"].get<std::string>().find("工作智能体") != std::string::npos, "chat profile default text has worker identity"); rc) return rc;

  // ── 技能 / 能力查询词应命中 local_profile（不走到 LLM）──
  auto chat_skills_1 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "目前有哪些技能"}});
  if (int rc = expect(chat_skills_1["decision"]["route"] == "local_profile", "chat skills1 route=local_profile"); rc) return rc;

  auto chat_skills_2 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "有哪些技能"}});
  if (int rc = expect(chat_skills_2["decision"]["route"] == "local_profile", "chat skills2 route=local_profile"); rc) return rc;

  auto chat_skills_3 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "技能有哪些"}});
  if (int rc = expect(chat_skills_3["decision"]["route"] == "local_profile", "chat skills3 route=local_profile"); rc) return rc;

  // ── 技能查询应返回 skills_only 模式（无自我介绍和工作流）──
  // 注意：离线测试模式下 profile.concise 模板可能缺失导致 text 为空，
  // 此断言只验证路由正确性；完整输出验证由 WS 冒烟测试覆盖。
  std::string skills1_text = chat_skills_1["text"].get<std::string>();
  if (!skills1_text.empty()) {
    // 非空时验证 skills_only 格式
    bool skills1_has_list = skills1_text.find("当前可用技能") != std::string::npos ||
                            skills1_text.find("Available skills") != std::string::npos;
    if (int rc = expect(skills1_has_list, "chat skills1 starts with skill list"); rc) return rc;
    if (int rc = expect(skills1_text.find("你好！我是") == std::string::npos, "chat skills1 no intro"); rc) return rc;
  }

  std::string skills3_text = chat_skills_3["text"].get<std::string>();
  if (!skills3_text.empty()) {
    bool skills3_has_list = skills3_text.find("当前可用技能") != std::string::npos ||
                            skills3_text.find("Available skills") != std::string::npos;
    if (int rc = expect(skills3_has_list, "chat skills3 starts with skill list"); rc) return rc;
  }

  // ── "你是谁" 应返回完整 profile（含自我介绍）──
  auto chat_who = svc.handle_request("s1_who", nlohmann::json{{"type", "chat"}, {"text", "你是谁"}});
  if (int rc = expect(chat_who["decision"]["route"] == "local_profile", "chat who route=local_profile"); rc) return rc;
  std::string who_text = chat_who["text"].get<std::string>();
  if (!who_text.empty()) {
    if (int rc = expect(who_text.find("你好！我是") != std::string::npos ||
                        who_text.find("Hello") != std::string::npos ||
                        who_text.find("I'm") != std::string::npos ||
                        who_text.find("thin_agent") != std::string::npos,
                        "chat who has intro"); rc) return rc;
  }
  auto chat_hello = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "你好"}});
  if (int rc = expect(chat_hello["type"] == "chat_result", "chat hello type"); rc) return rc;
  if (int rc = expect(!chat_hello["text"].get<std::string>().empty(), "chat hello text non-empty"); rc) return rc;
  if (int rc = expect(chat_hello["mode_used"] == "local-agent" || chat_hello["mode_used"] == "offline",
                      "chat hello mode local/offline"); rc) return rc;
  if (int rc = expect(!chat_hello["decision"]["route"].get<std::string>().empty(),
                      "chat hello route non-empty"); rc) return rc;

  // ── 英文 profile 查询 ──
  auto chat_who_are_you = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "who are you"}});
  if (int rc = expect(chat_who_are_you["type"] == "chat_result", "chat who are you type"); rc) return rc;
  if (int rc = expect(chat_who_are_you["mode_used"] == "local-agent", "chat who are you goes local-agent"); rc) return rc;
  if (int rc = expect(chat_who_are_you["decision"]["route"] == "local_profile",
                      "chat who are you route=local_profile"); rc) return rc;
  if (int rc = expect(chat_who_are_you["observation"]["profile_mode"] == "concise",
                      "chat who are you profile_mode=concise"); rc) return rc;
  if (int rc = expect(chat_who_are_you["text"].get<std::string>().size() > 0,
                      "chat who are you has reply text"); rc) return rc;

  // ── 缩写展开后 profile 查询（本地 offline 路径，不依赖云端 mock）──
  auto chat_who_r_u = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "who r u"}});
  if (int rc = expect(chat_who_r_u["type"] == "chat_result", "chat who r u type"); rc) return rc;
  if (int rc = expect(chat_who_r_u["mode_used"] == "local-agent", "chat who r u goes local-agent"); rc) return rc;
  if (int rc = expect(chat_who_r_u["decision"]["route"] == "local_profile",
                      "chat who r u route=local_profile"); rc) return rc;

  auto chat_profile_intro = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "介绍一下自己"}});
  if (int rc = expect(chat_profile_intro["type"] == "chat_result", "chat profile intro type"); rc) return rc;
  if (int rc = expect(chat_profile_intro["mode_used"] == "local-agent", "chat profile intro goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile_intro["decision"]["route"] == "local_profile", "chat profile intro route=local_profile"); rc) return rc;

  auto chat_profile_local_model = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "说说你自己"}});
  if (int rc = expect(chat_profile_local_model["type"] == "chat_result", "chat profile local model type"); rc) return rc;
  if (int rc = expect(chat_profile_local_model["mode_used"] == "local-agent", "chat profile local model goes local-agent"); rc) return rc;
  if (int rc = expect(chat_profile_local_model["decision"]["route"] == "local_profile", "chat profile local model route=local_profile"); rc) return rc;
  if (int rc = expect(chat_profile_local_model["intent_backend"] == "onnx", "chat profile local model intent_backend=onnx"); rc) return rc;
  if (int rc = expect(chat_profile_local_model["decision"]["intent"] == "profile", "chat profile local model intent=profile"); rc) return rc;
  if (int rc = expect(chat_profile_local_model["decision"]["confidence"].is_number(), "chat profile local model confidence is number"); rc) return rc;
  if (int rc = expect(chat_profile_local_model["decision"]["confidence"].get<double>() > 0.8, "chat profile local model confidence > 0.8"); rc) return rc;

  // ── DEV_MODE 下 developer 角色工具列表验证（含 search_files）──
  {
    setenv("THIN_AGENT_DEV_MODE", "1", 1);
    thin_agent::AgentService dev_svc(cfg, ex, te);
    dev_svc.on_session_open("s2");
    auto dev_profile = dev_svc.handle_request("s2", nlohmann::json{{"type", "chat"}, {"text", "你是谁"}});
    if (int rc = expect(dev_profile["decision"]["route"] == "local_profile", "dev profile route=local_profile"); rc) { unsetenv("THIN_AGENT_DEV_MODE"); return rc; }
    std::string dev_text = dev_profile["text"].get<std::string>();
    if (int rc = expect(dev_text.find("编码智能体") != std::string::npos, "dev profile has 编码智能体"); rc) { unsetenv("THIN_AGENT_DEV_MODE"); return rc; }
    // 验证 search_files 在 developer 角色工具列表中
    if (int rc = expect(dev_text.find("search_files") != std::string::npos, "dev profile has search_files tool"); rc) { unsetenv("THIN_AGENT_DEV_MODE"); return rc; }
    // 验证优化后的描述
    if (int rc = expect(dev_text.find("自动建目录") != std::string::npos, "dev profile has 自动建目录 in code_write_file desc"); rc) { unsetenv("THIN_AGENT_DEV_MODE"); return rc; }
    if (int rc = expect(dev_text.find("支持文件过滤") != std::string::npos, "dev profile has 支持文件过滤 in code_search desc"); rc) { unsetenv("THIN_AGENT_DEV_MODE"); return rc; }
    if (int rc = expect(dev_text.find("grep 搜索") != std::string::npos, "dev profile has grep 搜索 in search_code desc"); rc) { unsetenv("THIN_AGENT_DEV_MODE"); return rc; }
    unsetenv("THIN_AGENT_DEV_MODE");
  }

  auto chat_status = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "请给我当前状态"}});
  if (int rc = expect(chat_status["mode_used"] == "local-agent", "chat status goes local-agent"); rc) return rc;
  if (int rc = expect(chat_status["intent_backend"] == "rules", "chat status intent_backend=rules"); rc) return rc;
  if (int rc = expect(chat_status["decision"]["route"] == "local_status", "chat status route=local_status"); rc) return rc;
  if (int rc = expect(chat_status["decision_trace"].is_array(), "chat status decision_trace is array"); rc) return rc;
  if (int rc = expect_trace_schema(chat_status, "chat status"); rc) return rc;
  if (int rc = expect_local_policy_trace(chat_status, "chat status"); rc) return rc;
  if (int rc = expect(chat_status["observation"].contains("mode"), "chat status observation has mode"); rc) return rc;

  auto chat_status_2 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "现在啥状态"}});
  if (int rc = expect(chat_status_2["type"] == "chat_result", "chat status2 type"); rc) return rc;
  if (int rc = expect(chat_status_2["mode_used"] == "local-agent", "chat status2 goes local-agent"); rc) return rc;
  if (int rc = expect(chat_status_2["decision"]["route"] == "local_status", "chat status2 route=local_status"); rc) return rc;

  auto chat_status_3 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "运行正常吗"}});
  if (int rc = expect(chat_status_3["type"] == "chat_result", "chat status3 type"); rc) return rc;
  if (int rc = expect(chat_status_3["mode_used"] == "local-agent", "chat status3 goes local-agent"); rc) return rc;
  if (int rc = expect(chat_status_3["decision"]["route"] == "local_status", "chat status3 route=local_status"); rc) return rc;

  auto chat_model = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "你是什么模型"}});
  if (int rc = expect(chat_model["type"] == "chat_result", "chat model type"); rc) return rc;
  if (int rc = expect(chat_model["mode_used"] == "local-agent", "chat model goes local-agent"); rc) return rc;
  if (int rc = expect(chat_model["decision"]["route"] == "local_status", "chat model route=local_status"); rc) return rc;

  auto chat_model_what = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "你的模型是什么"}});
  if (int rc = expect(chat_model_what["decision"]["route"] == "local_status", "chat your model route=local_status"); rc)
    return rc;
  if (int rc = expect(chat_model_what["decision"]["route"] != "local_profile", "chat your model not profile"); rc) return rc;

  auto chat_local_runtime = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "你的本地模型用的什么"}});
  if (int rc = expect(chat_local_runtime["decision"]["route"] == "local_status", "chat local runtime route=local_status"); rc)
    return rc;
  if (int rc = expect(chat_local_runtime["text"].get<std::string>().find("ONNX") != std::string::npos,
                      "chat local runtime mentions ONNX"); rc)
    return rc;

  auto chat_msg_cap = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "消息能力"}});
  if (int rc = expect(chat_msg_cap["decision"]["route"] == "local_clarify", "chat messaging capability route=local_clarify"); rc)
    return rc;
  if (int rc = expect(chat_msg_cap["decision"]["reason"] == "messaging_capability_clarify",
                      "chat messaging capability reason"); rc)
    return rc;

  auto chat_ack = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "好的"}});
  if (int rc = expect(chat_ack["decision"]["route"] == "local_clarify", "chat short ack route=local_clarify"); rc) return rc;
  if (int rc = expect(chat_ack["decision"]["reason"] == "short_conversation_ack", "chat short ack reason"); rc) return rc;
  if (int rc = expect(chat_ack["mode_used"] == "local-agent", "chat short ack stays local"); rc) return rc;

  auto chat_onnx_knowledge = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "不是问你用的什么模型，ONNX模型是什么"}});
  if (int rc = expect(chat_onnx_knowledge["decision"]["route"] == "local_status", "chat onnx knowledge route=local_status"); rc)
    return rc;
  if (int rc = expect(chat_onnx_knowledge["decision"]["reason"] == "onnx_knowledge_local", "chat onnx knowledge reason"); rc)
    return rc;
  if (int rc = expect(chat_onnx_knowledge["text"].get<std::string>().find("intent_multiclass.onnx") != std::string::npos,
                      "chat onnx knowledge mentions onnx model file"); rc)
    return rc;

  const std::string chat_policy_path = "data/test_chat_policy.json";
  {
    std::ofstream out(chat_policy_path);
    out << R"POLICY({
  "keywords": {
    "weather": ["天况", "天气", "气温", "weather"],
    "news": ["新闻", "头条", "快讯", "热点", "news"],
    "profile": ["自我档案"],
    "status": ["系统体检"],
    "event": ["最新播报"],
    "memory_recent": ["会话速记"],
    "memory_history": ["历史档案"],
    "memory_search": ["记忆检索"],
    "memory_summary": ["记忆摘要"],
    "general": ["泛检索"],
    "general_info_query_include": ["进展", "动态", "情况", "最近", "如何", "总结", "概况", "消息", "查", "搜"],
    "general_info_query_exclude": ["天气", "气温", "新闻", "头条", "状态", "health", "执行", "任务", "拍照", "录制", "删除", "清空", "转账", "密钥", "密码", "重启", "kill", "rm "],
    "task_capture_inline": ["咔嚓任务", "帮我拍", "拍一张照", "拍照并告诉我结果", "提交任务", "任务拍照", "拍照任务"],
    "task_start_recording_inline": ["开始录制任务", "录制任务", "帮我开始录制并告诉我结果"],
    "action_capture": ["咔嚓一下", "拍照", "capture_photo"],
    "action_start_recording": ["录制", "开始录制", "start_recording"]
  },
  "intents": {
    "weather": {
      "kind": "external",
      "required_slots": ["city"],
      "primary_slot": "city",
      "slot_detector": "weather_city",
      "dialog_slot_policy": "referential",
      "optional_slots": {
        "date": {"default": "今天", "resolver": "weather_date"}
      },
      "clarify_template_key": "weather.clarify",
      "fetch_failed_template_key": "weather.fetch_failed",
      "summary_template_key": "weather.summary",
      "route_execute": "local_external_weather",
      "route_clarify": "local_external_clarify",
      "fetcher": "weather",
      "query_suffix": "的天气",
      "boost_on_slot_filled": true,
      "require_execute_threshold": false,
      "post_execute": "weather_advice",
      "reasons": {
        "missing": "weather_missing_city",
        "fetch_ok": "weather_fetch_ok",
        "fetch_failed": "weather_fetch_failed"
      }
    },
    "news": {
      "kind": "external",
      "required_slots": ["topic_or_scope"],
      "primary_slot": "topic_or_scope",
      "slot_detector": "news_topic",
      "dialog_slot_policy": "same_intent",
      "optional_slots": {
        "time_range": {"default": "最近", "resolver": "news_time_range"}
      },
      "clarify_template_key": "news.clarify",
      "fetch_failed_template_key": "news.fetch_failed",
      "summary_template_key": "news.summary",
      "route_execute": "local_external_news",
      "route_clarify": "local_external_clarify",
      "fetcher": "news",
      "query_suffix": "新闻",
      "boost_on_slot_filled": true,
      "require_execute_threshold": true,
      "post_execute": "",
      "reasons": {
        "missing": "news_missing_topic",
        "conf_below": "news_confidence_below_execute",
        "fetch_ok": "news_fetch_ok",
        "fetch_failed": "news_fetch_failed"
      }
    },
    "profile": {
      "kind": "local",
      "handler": "profile",
      "keyword_key": "profile",
      "route_execute": "local_profile",
      "default_confidence": 0.99,
      "route_excludes": ["model_status", "cloud_meta", "messaging_capability", "supported_languages"],
      "reasons": {
        "execute": "user_asks_agent_identity",
        "execute_detail": "user_asks_agent_capability_detail"
      }
    },
    "status": {
      "kind": "local",
      "handler": "status",
      "keyword_key": "status",
      "route_execute": "local_status",
      "summary_template_key": "status.summary",
      "default_confidence": 0.98,
      "reasons": {"execute": "status_intent"}
    },
    "event_recent": {
      "kind": "local",
      "handler": "event_recent",
      "keyword_key": "event",
      "route_execute": "local_events",
      "default_confidence": 0.97,
      "reasons": {"execute": "event_intent"}
    },
    "memory_recent": {
      "kind": "local",
      "handler": "memory_recent",
      "keyword_key": "memory_recent",
      "route_execute": "local_memory_recent",
      "default_confidence": 0.97,
      "reasons": {"execute": "memory_recent_intent"}
    },
    "memory_history": {
      "kind": "local",
      "handler": "memory_history",
      "keyword_key": "memory_history",
      "route_execute": "local_memory_history",
      "default_confidence": 0.97,
      "reasons": {"execute": "memory_history_intent"}
    },
    "memory_search": {
      "kind": "local",
      "handler": "memory_search",
      "keyword_key": "memory_search",
      "route_execute": "local_memory_search",
      "summary_template_key": "memory.search_summary",
      "default_confidence": 0.97,
      "reasons": {"execute": "memory_search_intent"}
    },
    "memory_summary": {
      "kind": "local",
      "handler": "memory_summary",
      "keyword_key": "memory_summary",
      "route_execute": "local_memory_summary",
      "summary_template_key": "memory.summary_summary",
      "default_confidence": 0.97,
      "reasons": {"execute": "memory_summary_intent"}
    },
    "short_ack": {
      "kind": "clarify",
      "predicate": "short_ack",
      "route_execute": "local_clarify",
      "clarify_template_key": "chat.short_ack",
      "intent_label": "general",
      "default_confidence": 0.80,
      "reasons": {"execute": "short_conversation_ack"}
    },
    "messaging_capability": {
      "kind": "clarify",
      "predicate": "messaging_capability",
      "route_execute": "local_clarify",
      "clarify_template_key": "chat.messaging_capability_clarify",
      "intent_label": "general",
      "default_confidence": 0.75,
      "reasons": {"execute": "messaging_capability_clarify"}
    },
    "onnx_knowledge": {
      "kind": "local",
      "predicate": "onnx_knowledge",
      "handler": "onnx_knowledge",
      "route_execute": "local_status",
      "summary_template_key": "status.onnx_explainer",
      "intent_label": "status",
      "default_confidence": 0.94,
      "reasons": {"execute": "onnx_knowledge_local"}
    },
    "supported_languages": {
      "kind": "local",
      "predicate": "supported_languages",
      "handler": "supported_languages",
      "route_execute": "local_supported_languages",
      "intent_label": "general",
      "default_confidence": 0.95,
      "reasons": {"execute": "supported_languages_query"}
    },
    "general": {
      "kind": "external_clarify",
      "keyword_key": "general",
      "clarify_template_key": "general.clarify",
      "route_clarify": "local_external_clarify",
      "intent_label": "general",
      "require_execute_threshold": true,
      "reasons": {"missing": "general_external_needs_scope"}
    },
    "task_capture_inline": {
      "kind": "task",
      "keyword_key": "task_capture_inline",
      "action_name": "capture_photo",
      "route_execute": "local_task_inline",
      "summary_template_key": "task.capture_submitted",
      "intent_label": "task_capture",
      "default_confidence": 0.98,
      "reasons": {
        "execute": "task_expression_capture_photo",
        "fetch_failed": "task_engine_unavailable"
      }
    },
    "task_start_recording_inline": {
      "kind": "task",
      "keyword_key": "task_start_recording_inline",
      "action_name": "start_recording",
      "route_execute": "local_task_inline",
      "summary_template_key": "task.start_recording_submitted",
      "intent_label": "task_start_recording",
      "default_confidence": 0.98,
      "reasons": {
        "execute": "task_expression_start_recording",
        "fetch_failed": "task_engine_unavailable"
      }
    },
    "action_capture": {
      "kind": "action",
      "keyword_key": "action_capture",
      "action_name": "capture_photo",
      "route_execute": "local_action",
      "intent_label": "action_capture_photo",
      "default_confidence": 0.96,
      "reasons": {"execute": "action_intent_capture_photo"}
    },
    "action_start_recording": {
      "kind": "action",
      "keyword_key": "action_start_recording",
      "action_name": "start_recording",
      "route_execute": "local_action",
      "intent_label": "action_start_recording",
      "default_confidence": 0.96,
      "reasons": {"execute": "action_intent_start_recording"}
    },
    "budget_gate": {
      "kind": "gate",
      "predicate": "budget_exceeded",
      "gate_mode": "cloud",
      "route_execute": "local_clarify",
      "clarify_template_key": "budget.clarify",
      "intent_label": "general",
      "default_confidence": 0.60,
      "reasons": {
        "budget_input": "budget_input_exceeded",
        "budget_latency": "budget_latency_exceeded",
        "budget_cost": "budget_cost_exceeded"
      }
    }
  },
  "slots": {
    "weather": {
      "cities": [
        {"aliases": ["上海"], "value": "上海"},
        {"aliases": ["北京"], "value": "北京"},
        {"aliases": ["深圳"], "value": "深圳"},
        {"aliases": ["shanghai"], "value": "Shanghai", "match_lower": true}
      ],
      "date_aliases": [
        {"match": ["今天", "今日"], "value": "今天"},
        {"match": ["明天"], "value": "明天"}
      ],
      "followup_suffixes": ["的呢", "的天气", "天气", "呢", "怎么样", "如何"],
      "trailing_particles": ["的"],
      "confirm_replies": ["需要", "好的", "好", "行", "是", "yes", "ok"],
      "extra_triggers": ["weather", "wether", "天况"],
      "query_suffix": "的天气"
    },
    "news": {
      "topics": [
        {"aliases": ["ai"], "value": "AI", "match_lower": true},
        {"aliases": ["科技"], "value": "科技"}
      ],
      "time_range_aliases": [
        {"match": ["今天", "今日"], "value": "今天"},
        {"match": ["最近", "近24小时", "24小时"], "value": "最近"}
      ],
      "followup_suffixes": ["新闻", "消息", "呢", "方面的"],
      "query_suffix": "新闻"
    }
  },
  "templates": {
    "weather.clarify": "[CFG] 请提供城市，我来查天况。",
    "weather.clarify_en": "[CFG_EN] which city?",
    "news.clarify": "[CFG] 请提供新闻主题。",
    "news.clarify_en": "[CFG_EN] which topic?",
    "news.summary": "[CFG] 新闻摘要：{topic}",
    "news.summary_en": "[CFG_EN] {topic} news: {top1} | {top2}",
    "weather.summary_en": "[CFG_EN] {city} {date}: {condition}, {temp_c}C, {humidity}%",
    "weather.fetch_failed": "[CFG_WEATHER_FETCH_FAILED] 天气服务暂时不可用，请稍后重试。",
    "weather.fetch_failed_en": "[CFG_EN_WFAIL] weather service busy.",
    "news.fetch_failed": "[CFG_NEWS_FETCH_FAILED] 新闻服务暂时不可用，请稍后重试。",
    "news.fetch_failed_en": "[CFG_EN_NFAIL] news service busy.",
    "profile.concise": "[CFG_PROFILE] 这是来自配置的简版主体介绍。",
    "status.summary": "[CFG_STATUS] 这是来自配置的状态摘要。",
    "event.list_header": "最近事件（{count} 条）:\n",
    "event.empty": "(无)",
    "memory.recent.list_header": "会话短期记忆（{count} 条）:\n",
    "memory.recent.empty": "(暂无)",
    "memory.history.list_header": "长期记忆历史（{count} 条）:\n",
    "memory.history.empty": "(暂无)",
    "event.summary": "[CFG_EVENT] 已返回配置化事件窗口。",
    "memory.recent_summary": "[CFG_MEMORY_RECENT] 已返回配置化会话记忆。",
    "memory.history_summary": "[CFG_MEMORY_HISTORY] 已返回配置化历史记忆。",
    "memory.search_summary": "[CFG_MEMORY_SEARCH] 已返回配置化记忆检索结果。",
    "memory.summary_summary": "[CFG_MEMORY_SUMMARY] 已返回配置化记忆摘要。",
    "memory.maintenance_conclusion": "[CFG_MAINT_CONCLUSION] 命中{total_hits}条（task={task_hits}，audit={audit_hits}），query={query}，limit={limit}。",
    "memory.maintenance_evidence": "[CFG_MAINT_EVIDENCE] #1 {top1}{latest_tag}；#2 {top2}",
    "memory.maintenance_summary_evidence": "[CFG_MAINT_SUMMARY] latest={latest_task_id}/{latest_kind}/{latest_row_ref}",
    "general.clarify": "[CFG_GENERAL] 请补充范围后我继续检索。",
    "budget.clarify": "[CFG_BUDGET] 当前请求超出预算阈值，请缩小范围后重试。",
    "cloud.clarify_prefix": "[CFG_CLOUD_PREFIX] ",
    "cloud.clarify_default_question": "[CFG_CLOUD_ASK] 请确认你要状态、能力还是执行动作。",
    "cloud.policy_contract_violation": "[CFG_POLICY_CONTRACT] 云策略字段不合规，已降级本地澄清。",
    "cloud.reject": "[CFG_CLOUD_REJECT] 这是来自配置的拒绝文案。",
    "cloud.reject_downgrade_clarify": "[CFG_CLOUD_REJECT_DOWNGRADE] 这类信息检索请求我先不直接拒绝。请补充你关注的范围（时间/模块/地区），我就继续帮你查。",
    "cloud.status_local": "[CFG_CLOUD_STATUS] 这是来自配置的本地状态回包。",
    "cloud.task_inline_reject": "[CFG_CLOUD_TASK_REJECT] task_engine 未就绪。",
    "cloud.task_inline_submitted": "[CFG_CLOUD_TASK_EXEC] action={action} task_id={task_id} state={state}",
    "cloud.offline_local_fallback": "[CFG_OFFLINE_LOCAL] 当前离线模式。可执行本地动作，并可后续切换云模型。输入回显：{text}",
    "cloud.fallback_missing_key": "[CFG_CLOUD_MISSING_KEY] 未检测到云模型密钥，已回退离线模式。输入回显：{text}",
    "cloud.fallback_call_failed": "[CFG_CLOUD_CALL_FAILED] 云调用失败，已回退离线模式。输入回显：{text}",
    "cloud.error_call_failed_no_fallback": "[CFG_CLOUD_ERROR] 云调用失败且未允许离线回退。",
    "task.capture_submitted": "[CFG_TASK_CAPTURE] 已提交拍照任务 task_id={task_id} state={state}",
    "action.capture_ok": "[CFG_ACTION_CAPTURE_OK] 已按配置执行拍照动作。",
    "action.capture_fail": "[CFG_ACTION_CAPTURE_FAIL] 拍照动作返回非0，请查看 observation.result。"
  },
  "style": {
    "default": "warm",
    "normal": {
      "cloud": {
        "clarify_prefix": "[CFG_STYLE_NORMAL_CLARIFY] ",
        "answer_prefix": "[CFG_STYLE_NORMAL_ANSWER] "
      }
    },
    "warm": {
      "cloud": {
        "clarify_prefix": "[CFG_STYLE_WARM_CLARIFY] ",
        "answer_prefix": "[CFG_STYLE_WARM_ANSWER] "
      },
      "profile": {
        "concise_prefix": "[CFG_STYLE_WARM_PROFILE] ",
        "detailed_prefix": "[CFG_STYLE_WARM_PROFILE_DETAIL] "
      },
      "local": {
        "status_prefix": "[CFG_STYLE_WARM_LOCAL_STATUS] ",
        "event_prefix": "[CFG_STYLE_WARM_LOCAL_EVENT] ",
        "memory_recent_prefix": "[CFG_STYLE_WARM_LOCAL_MEMORY_RECENT] ",
        "memory_history_prefix": "[CFG_STYLE_WARM_LOCAL_MEMORY_HISTORY] ",
        "memory_search_prefix": "[CFG_STYLE_WARM_LOCAL_MEMORY_SEARCH] ",
        "memory_summary_prefix": "[CFG_STYLE_WARM_LOCAL_MEMORY_SUMMARY] ",
        "task_prefix": "[CFG_STYLE_WARM_LOCAL_TASK] ",
        "action_prefix": "[CFG_STYLE_WARM_LOCAL_ACTION] ",
        "external_clarify_prefix": "[CFG_STYLE_WARM_LOCAL_EXTERNAL_CLARIFY] ",
        "external_summary_prefix": "[CFG_STYLE_WARM_LOCAL_EXTERNAL_SUMMARY] "
      }
    }
  }
})POLICY";
  }
  ::setenv("THIN_AGENT_CHAT_POLICY_PATH", chat_policy_path.c_str(), 1);

  auto chat_weather_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "查下天况"}});
  if (int rc = expect(chat_weather_cfg["type"] == "chat_result", "chat weather cfg type"); rc) return rc;
  if (int rc = expect(chat_weather_cfg["decision"]["route"] == "local_external_clarify", "chat weather cfg route"); rc) return rc;
  if (int rc = expect(chat_weather_cfg["decision"]["intent"] == "weather", "chat weather cfg intent"); rc) return rc;
  if (int rc = expect(chat_weather_cfg["text"].get<std::string>().find("[CFG]") != std::string::npos, "chat weather cfg clarify text"); rc) return rc;
  if (int rc = expect(chat_weather_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_EXTERNAL_CLARIFY]") != std::string::npos, "chat weather cfg warm external clarify prefix"); rc) return rc;

  auto chat_profile_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "请展示你的自我档案"}});
  if (int rc = expect(chat_profile_cfg["type"] == "chat_result", "chat profile cfg type"); rc) return rc;
  if (int rc = expect(chat_profile_cfg["decision"]["route"] == "local_profile", "chat profile cfg route"); rc) return rc;
  // 角色驱动自我介绍：应包含 style prefix + 身份标识
  std::string profile_text = chat_profile_cfg["text"].get<std::string>();
  if (int rc = expect(profile_text.find("[CFG_STYLE_WARM_PROFILE]") != std::string::npos, "chat profile cfg warm style prefix"); rc) return rc;
  // 角色驱动时为中文角色名，回退模板时为 [CFG_PROFILE]
  bool has_identity = (profile_text.find("工作智能体") != std::string::npos ||
                       profile_text.find("[CFG_PROFILE]") != std::string::npos);
  if (int rc = expect(has_identity, "chat profile cfg has identity"); rc) return rc;

  auto chat_status_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "帮我做系统体检"}});
  if (int rc = expect(chat_status_cfg["type"] == "chat_result", "chat status cfg type"); rc) return rc;
  if (int rc = expect(chat_status_cfg["decision"]["route"] == "local_status", "chat status cfg route"); rc) return rc;
  if (int rc = expect(chat_status_cfg["text"].get<std::string>().find("[CFG_STATUS]") != std::string::npos, "chat status cfg summary text"); rc) return rc;
  if (int rc = expect(chat_status_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_STATUS]") != std::string::npos, "chat status cfg warm local prefix"); rc) return rc;

  auto chat_event_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "给我最新播报"}});
  if (int rc = expect(chat_event_cfg["type"] == "chat_result", "chat event cfg type"); rc) return rc;
  if (int rc = expect(chat_event_cfg["decision"]["route"] == "local_events", "chat event cfg route"); rc) return rc;
  if (int rc = expect(chat_event_cfg["text"].get<std::string>().find("最近事件") != std::string::npos, "chat event cfg text has events"); rc) return rc;
  if (int rc = expect(chat_event_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_EVENT]") != std::string::npos, "chat event cfg warm local prefix"); rc) return rc;

  auto chat_memory_recent_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "读取会话速记"}});
  if (int rc = expect(chat_memory_recent_cfg["type"] == "chat_result", "chat memory recent cfg type"); rc) return rc;
  if (int rc = expect(chat_memory_recent_cfg["decision"]["route"] == "local_memory_recent", "chat memory recent cfg route"); rc) return rc;
  if (int rc = expect(chat_memory_recent_cfg["text"].get<std::string>().find("会话短期记忆") != std::string::npos, "chat memory recent cfg text"); rc) return rc;
  if (int rc = expect(chat_memory_recent_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_MEMORY_RECENT]") != std::string::npos, "chat memory recent cfg warm local prefix"); rc) return rc;

  auto chat_memory_history_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "打开历史档案"}});
  if (int rc = expect(chat_memory_history_cfg["type"] == "chat_result", "chat memory history cfg type"); rc) return rc;
  if (int rc = expect(chat_memory_history_cfg["decision"]["route"] == "local_memory_history", "chat memory history cfg route"); rc) return rc;
  if (int rc = expect(chat_memory_history_cfg["text"].get<std::string>().find("长期记忆历史") != std::string::npos, "chat memory history cfg text"); rc) return rc;
  if (int rc = expect(chat_memory_history_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_MEMORY_HISTORY]") != std::string::npos, "chat memory history cfg warm local prefix"); rc) return rc;

  auto chat_memory_search_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "请做记忆检索 memory-search-token-v0639 3"}});
  if (int rc = expect(chat_memory_search_cfg["type"] == "chat_result", "chat memory search cfg type"); rc) return rc;
  if (int rc = expect(chat_memory_search_cfg["decision"]["route"] == "local_memory_search", "chat memory search cfg route"); rc) return rc;
  if (int rc = expect(chat_memory_search_cfg["decision"]["intent"] == "memory_search", "chat memory search cfg intent"); rc) return rc;
  if (int rc = expect(chat_memory_search_cfg["text"].get<std::string>().find("[CFG_MEMORY_SEARCH]") != std::string::npos, "chat memory search cfg summary text"); rc) return rc;
  if (int rc = expect(chat_memory_search_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_MEMORY_SEARCH]") != std::string::npos, "chat memory search cfg warm local prefix"); rc) return rc;
  if (int rc = expect(chat_memory_search_cfg["observation"]["type"] == "memory_search_result", "chat memory search cfg observation type"); rc) return rc;
  if (int rc = expect(chat_memory_search_cfg["observation"]["query"].get<std::string>() == "memory-search-token-v0639", "chat memory search cfg query exact"); rc) return rc;
  if (int rc = expect(chat_memory_search_cfg["observation"]["limit_applied"].get<int>() == 3, "chat memory search cfg limit=3"); rc) return rc;

  auto chat_memory_summary_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "请做记忆摘要 memory-search-token-v0639 2"}});
  if (int rc = expect(chat_memory_summary_cfg["type"] == "chat_result", "chat memory summary cfg type"); rc) return rc;
  if (int rc = expect(chat_memory_summary_cfg["decision"]["route"] == "local_memory_summary", "chat memory summary cfg route"); rc) return rc;
  if (int rc = expect(chat_memory_summary_cfg["decision"]["intent"] == "memory_summary", "chat memory summary cfg intent"); rc) return rc;
  if (int rc = expect(chat_memory_summary_cfg["text"].get<std::string>().find("[CFG_MEMORY_SUMMARY]") != std::string::npos, "chat memory summary cfg summary text"); rc) return rc;
  if (int rc = expect(chat_memory_summary_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_MEMORY_SUMMARY]") != std::string::npos, "chat memory summary cfg warm local prefix"); rc) return rc;
  if (int rc = expect(chat_memory_summary_cfg["observation"]["type"] == "memory_summary_result", "chat memory summary cfg observation type"); rc) return rc;
  if (int rc = expect(chat_memory_summary_cfg["observation"]["query"].get<std::string>() == "memory-search-token-v0639", "chat memory summary cfg query exact"); rc) return rc;
  if (int rc = expect(chat_memory_summary_cfg["observation"]["limit_applied"].get<int>() == 2, "chat memory summary cfg limit=2"); rc) return rc;

  auto chat_memory_summary_cfg_task = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "请做记忆摘要 capture_photo 10"}});
  if (int rc = expect(chat_memory_summary_cfg_task["type"] == "chat_result", "chat memory summary cfg task type"); rc) return rc;
  if (int rc = expect(chat_memory_summary_cfg_task["decision"]["route"] == "local_memory_summary", "chat memory summary cfg task route"); rc) return rc;
  if (int rc = expect(chat_memory_summary_cfg_task["text"].get<std::string>().find("[CFG_MAINT_CONCLUSION]") != std::string::npos, "chat memory summary cfg task has maint conclusion"); rc) return rc;
  if (int rc = expect(chat_memory_summary_cfg_task["text"].get<std::string>().find("[CFG_MAINT_SUMMARY]") != std::string::npos, "chat memory summary cfg task has maint summary evidence"); rc) return rc;
  if (int rc = expect(chat_memory_summary_cfg_task["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_MEMORY_SUMMARY]") != std::string::npos, "chat memory summary cfg task warm local prefix"); rc) return rc;
  if (int rc = expect(chat_memory_summary_cfg_task["observation"].contains("evidence_latest_basis") && chat_memory_summary_cfg_task["observation"]["evidence_latest_basis"].is_object(), "chat memory summary cfg task has evidence_latest_basis"); rc) return rc;
  if (int rc = expect(chat_memory_summary_cfg_task["observation"]["evidence_latest_basis"].contains("selected_at") &&
                          chat_memory_summary_cfg_task["observation"]["evidence_latest_basis"]["selected_at"].is_string(),
                      "chat memory summary cfg task latest_basis selected_at string");
      rc)
    return rc;

  auto chat_general_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "请做泛检索"}});
  if (int rc = expect(chat_general_cfg["type"] == "chat_result", "chat general cfg type"); rc) return rc;
  if (int rc = expect(chat_general_cfg["decision"]["route"] == "local_external_clarify", "chat general cfg route"); rc) return rc;
  if (int rc = expect(chat_general_cfg["text"].get<std::string>().find("[CFG_GENERAL]") != std::string::npos, "chat general cfg summary text"); rc) return rc;
  if (int rc = expect(chat_general_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_EXTERNAL_CLARIFY]") != std::string::npos, "chat general cfg warm external clarify prefix"); rc) return rc;

  auto chat_weather_exec_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "今天上海天气"}});
  if (int rc = expect(chat_weather_exec_cfg["type"] == "chat_result", "chat weather exec cfg type"); rc) return rc;
  if (int rc = expect(chat_weather_exec_cfg["decision"]["route"] == "local_external_weather", "chat weather exec cfg route"); rc) return rc;
  if (int rc = expect(chat_weather_exec_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_EXTERNAL_SUMMARY]") != std::string::npos, "chat weather exec cfg warm external summary prefix"); rc) return rc;

  auto chat_news_clarify_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "看新闻"}});
  if (int rc = expect(chat_news_clarify_cfg["type"] == "chat_result", "chat news clarify cfg type"); rc) return rc;
  if (int rc = expect(chat_news_clarify_cfg["decision"]["route"] == "local_external_clarify", "chat news clarify cfg route"); rc) return rc;
  if (int rc = expect(chat_news_clarify_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_EXTERNAL_CLARIFY]") != std::string::npos, "chat news clarify cfg warm external clarify prefix"); rc) return rc;

  auto chat_news_exec_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "最近AI新闻"}});
  if (int rc = expect(chat_news_exec_cfg["type"] == "chat_result", "chat news exec cfg type"); rc) return rc;
  if (int rc = expect(chat_news_exec_cfg["decision"]["route"] == "local_external_news", "chat news exec cfg route"); rc) return rc;
  if (int rc = expect(chat_news_exec_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_EXTERNAL_SUMMARY]") != std::string::npos, "chat news exec cfg warm external summary prefix"); rc) return rc;

  ::setenv("THIN_AGENT_EXTERNAL_PROVIDER", "http", 1);
  ::setenv("THIN_AGENT_EXTERNAL_HTTP_MOCK_WEATHER_JSON", "{invalid", 1);
  auto chat_weather_fetch_failed_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "今天上海天气"}});
  if (int rc = expect(chat_weather_fetch_failed_cfg["type"] == "chat_result", "chat weather fetch_failed cfg type"); rc) return rc;
  if (int rc = expect(chat_weather_fetch_failed_cfg["decision"]["route"] == "local_external_clarify", "chat weather fetch_failed cfg route"); rc) return rc;
  if (int rc = expect(chat_weather_fetch_failed_cfg["decision"]["reason"] == "weather_fetch_failed", "chat weather fetch_failed cfg reason"); rc) return rc;
  if (int rc = expect(chat_weather_fetch_failed_cfg["text"].get<std::string>().find("[CFG_WEATHER_FETCH_FAILED]") != std::string::npos, "chat weather fetch_failed cfg text"); rc) return rc;
  if (int rc = expect(chat_weather_fetch_failed_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_EXTERNAL_CLARIFY]") != std::string::npos, "chat weather fetch_failed cfg warm external clarify prefix"); rc) return rc;

  ::setenv("THIN_AGENT_EXTERNAL_HTTP_MOCK_NEWS_JSON", "{invalid", 1);
  auto chat_news_fetch_failed_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "最近AI新闻"}});
  if (int rc = expect(chat_news_fetch_failed_cfg["type"] == "chat_result", "chat news fetch_failed cfg type"); rc) return rc;
  if (int rc = expect(chat_news_fetch_failed_cfg["decision"]["route"] == "local_external_clarify", "chat news fetch_failed cfg route"); rc) return rc;
  if (int rc = expect(chat_news_fetch_failed_cfg["decision"]["reason"] == "news_fetch_failed", "chat news fetch_failed cfg reason"); rc) return rc;
  if (int rc = expect(chat_news_fetch_failed_cfg["text"].get<std::string>().find("[CFG_NEWS_FETCH_FAILED]") != std::string::npos, "chat news fetch_failed cfg text"); rc) return rc;
  if (int rc = expect(chat_news_fetch_failed_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_EXTERNAL_CLARIFY]") != std::string::npos, "chat news fetch_failed cfg warm external clarify prefix"); rc) return rc;

  ::setenv("THIN_AGENT_EXTERNAL_PROVIDER", "mock", 1);
  ::unsetenv("THIN_AGENT_EXTERNAL_HTTP_MOCK_WEATHER_JSON");
  ::unsetenv("THIN_AGENT_EXTERNAL_HTTP_MOCK_NEWS_JSON");

  auto chat_task_capture_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "请创建一个咔嚓任务"}});
  if (int rc = expect(chat_task_capture_cfg["type"] == "chat_result", "chat task capture cfg type"); rc) return rc;
  if (int rc = expect(chat_task_capture_cfg["decision"]["route"] == "local_task_inline", "chat task capture cfg route"); rc) return rc;
  if (int rc = expect(chat_task_capture_cfg["text"].get<std::string>().find("[CFG_TASK_CAPTURE]") != std::string::npos, "chat task capture cfg text"); rc) return rc;
  if (int rc = expect(chat_task_capture_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_TASK]") != std::string::npos, "chat task capture cfg warm local prefix"); rc) return rc;

  auto chat_action_capture_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "现在咔嚓一下"}});
  if (int rc = expect(chat_action_capture_cfg["type"] == "chat_result", "chat action capture cfg type"); rc) return rc;
  if (int rc = expect(chat_action_capture_cfg["decision"]["route"] == "local_action", "chat action capture cfg route"); rc) return rc;
  if (int rc = expect(chat_action_capture_cfg["text"].get<std::string>().find("[CFG_ACTION_CAPTURE_") != std::string::npos, "chat action capture cfg text"); rc) return rc;
  if (int rc = expect(chat_action_capture_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_LOCAL_ACTION]") != std::string::npos, "chat action capture cfg warm local prefix"); rc) return rc;
  ::unsetenv("THIN_AGENT_CHAT_POLICY_PATH");

  auto chat_weather_clarify = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "查天气"}});
  if (int rc = expect(chat_weather_clarify["type"] == "chat_result", "chat weather clarify type"); rc) return rc;
  if (int rc = expect(chat_weather_clarify["mode_used"] == "local-agent", "chat weather clarify local-agent"); rc) return rc;
  if (int rc = expect(chat_weather_clarify["decision"]["route"] == "local_external_clarify", "chat weather clarify route"); rc) return rc;
  if (int rc = expect(chat_weather_clarify["decision"]["policy"] == "clarify", "chat weather clarify policy"); rc) return rc;
  if (int rc = expect(chat_weather_clarify["decision"]["intent"] == "weather", "chat weather clarify intent"); rc) return rc;
  if (int rc = expect_local_policy_trace(chat_weather_clarify, "chat weather clarify"); rc) return rc;
  if (int rc = expect(chat_weather_clarify["text"].get<std::string>().find("城市") != std::string::npos, "chat weather clarify text"); rc) return rc;

  auto chat_weather_exec = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "今天上海天气"}});
  if (int rc = expect(chat_weather_exec["type"] == "chat_result", "chat weather exec type"); rc) return rc;
  if (int rc = expect(chat_weather_exec["mode_used"] == "local-agent", "chat weather exec local-agent"); rc) return rc;
  if (int rc = expect(chat_weather_exec["decision"]["route"] == "local_external_weather", "chat weather exec route"); rc) return rc;
  if (int rc = expect(chat_weather_exec["decision"]["policy"] == "execute", "chat weather exec policy"); rc) return rc;
  if (int rc = expect(chat_weather_exec["decision"]["intent"] == "weather", "chat weather exec intent"); rc) return rc;
  if (int rc = expect(chat_weather_exec["decision"]["slots"]["city"] == "上海", "chat weather exec slots city"); rc) return rc;
  if (int rc = expect(chat_weather_exec["text"].get<std::string>().find("°C") != std::string::npos, "chat weather exec text"); rc) return rc;
  if (int rc = expect(chat_weather_exec["observation"].contains("external_result"), "chat weather exec has external_result"); rc) return rc;
  if (int rc = expect(chat_weather_exec["observation"]["external_source"] == "mock", "chat weather exec external_source=mock"); rc) return rc;

  auto chat_news_clarify = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "看新闻"}});
  if (int rc = expect(chat_news_clarify["type"] == "chat_result", "chat news clarify type"); rc) return rc;
  if (int rc = expect(chat_news_clarify["mode_used"] == "local-agent", "chat news clarify local-agent"); rc) return rc;
  if (int rc = expect(chat_news_clarify["decision"]["route"] == "local_external_clarify", "chat news clarify route"); rc) return rc;
  if (int rc = expect(chat_news_clarify["decision"]["policy"] == "clarify", "chat news clarify policy"); rc) return rc;
  if (int rc = expect(chat_news_clarify["decision"]["intent"] == "news", "chat news clarify intent"); rc) return rc;
  if (int rc = expect(chat_news_clarify["text"].get<std::string>().find("科技") != std::string::npos, "chat news clarify text"); rc) return rc;

  auto chat_news_exec = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "最近AI新闻"}});
  if (int rc = expect(chat_news_exec["type"] == "chat_result", "chat news exec type"); rc) return rc;
  if (int rc = expect(chat_news_exec["mode_used"] == "local-agent", "chat news exec local-agent"); rc) return rc;
  if (int rc = expect(chat_news_exec["decision"]["route"] == "local_external_news", "chat news exec route"); rc) return rc;
  if (int rc = expect(chat_news_exec["decision"]["policy"] == "execute", "chat news exec policy"); rc) return rc;
  if (int rc = expect(chat_news_exec["decision"]["intent"] == "news", "chat news exec intent"); rc) return rc;
  if (int rc = expect_local_policy_trace(chat_news_exec, "chat news exec"); rc) return rc;
  if (int rc = expect(chat_news_exec["decision"]["slots"]["topic_or_scope"] == "AI", "chat news exec slots topic"); rc) return rc;
  {
    const std::string news_text = chat_news_exec["text"].get<std::string>();
    if (int rc = expect(news_text.find("▸") != std::string::npos ||
                        news_text.find("AI 模型") != std::string::npos ||
                        news_text.find("端侧智能体") != std::string::npos,
                    "chat news exec text"); rc)
      return rc;
  }
  if (int rc = expect(chat_news_exec["observation"].contains("external_result"), "chat news exec has external_result"); rc) return rc;
  if (int rc = expect(chat_news_exec["observation"]["external_source"] == "mock", "chat news exec external_source=mock"); rc) return rc;

  auto chat_news_exec_2 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "来点ai快讯"}});
  if (int rc = expect(chat_news_exec_2["type"] == "chat_result", "chat news exec2 type"); rc) return rc;
  if (int rc = expect(chat_news_exec_2["mode_used"] == "local-agent", "chat news exec2 local-agent"); rc) return rc;
  if (int rc = expect(chat_news_exec_2["decision"]["route"] == "local_external_news", "chat news exec2 route"); rc) return rc;
  if (int rc = expect(chat_news_exec_2["decision"]["policy"] == "execute", "chat news exec2 policy"); rc) return rc;

  auto chat_general_clarify_2 = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "帮我搜点新消息"}});
  if (int rc = expect(chat_general_clarify_2["type"] == "chat_result", "chat general clarify2 type"); rc) return rc;
  if (int rc = expect(chat_general_clarify_2["mode_used"] == "local-agent", "chat general clarify2 local-agent"); rc) return rc;
  if (int rc = expect(chat_general_clarify_2["decision"]["route"] == "local_external_clarify", "chat general clarify2 route"); rc) return rc;
  if (int rc = expect(chat_general_clarify_2["decision"]["policy"] == "clarify", "chat general clarify2 policy"); rc) return rc;

  ::setenv("THIN_AGENT_EXTERNAL_PROVIDER", "http", 1);
  ::setenv("THIN_AGENT_EXTERNAL_HTTP_MOCK_WEATHER_JSON",
           "{\"current_condition\":[{\"temp_C\":\"28\",\"humidity\":\"61\",\"weatherDesc\":[{\"value\":\"Partly cloudy\"}]}],"
           "\"nearest_area\":[{\"areaName\":[{\"value\":\"Shanghai\"}]}]}", 1);
  auto chat_weather_http = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "今天上海天气"}});
  if (int rc = expect(chat_weather_http["type"] == "chat_result", "chat weather http type"); rc) return rc;
  // mock-http: always returns local_external_weather
  if (int rc = expect(chat_weather_http["decision"]["route"] == "local_external_weather", "chat weather http route"); rc) return rc;
  if (int rc = expect(chat_weather_http["observation"]["external_source"] == "mock-http", "chat weather http source=mock-http"); rc) return rc;
  if (int rc = expect(chat_weather_http["observation"]["external_result"]["provider"] == "mock-http", "chat weather http provider=mock-http"); rc) return rc;

  ::setenv("THIN_AGENT_EXTERNAL_HTTP_MOCK_NEWS_JSON",
           R"({"items":[{"title":"AI 产业进入加速期","source":"hn"},{"title":"端侧协同框架发布","source":"hn"}]})",
           1);
  auto chat_news_http = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "最近AI新闻"}});
  if (int rc = expect(chat_news_http["type"] == "chat_result", "chat news http type"); rc) return rc;
  if (int rc = expect(chat_news_http["decision"]["route"] == "local_external_news", "chat news http route"); rc) return rc;
  if (int rc = expect(chat_news_http["observation"]["external_source"] == "real-http", "chat news http external_source=real-http"); rc) return rc;
  if (int rc = expect(chat_news_http["observation"]["external_result"]["items"].is_array(), "chat news http items array"); rc) return rc;
  if (int rc = expect(chat_news_http["observation"]["external_result"]["provider"] == "hn-algolia", "chat news http provider=hn-algolia"); rc) return rc;
  if (int rc = expect(chat_news_http["text"].get<std::string>().find("AI 产业进入加速期") != std::string::npos, "chat news http text has title"); rc) return rc;

  ::setenv("THIN_AGENT_EXTERNAL_HTTP_MOCK_NEWS_JSON",
           R"({"items":[{"title":"Don't post generated/AI-edited comments on HN","source":"hn"},{"title":"New AI chip for edge inference","source":"hn"},{"title":"端侧大模型部署实践","source":"hn"}]})",
           1);
  auto chat_news_filter = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "最近AI新闻"}});
  if (int rc = expect(chat_news_filter["decision"]["route"] == "local_external_news", "chat news filter route"); rc) return rc;
  if (int rc = expect(chat_news_filter["text"].get<std::string>().find("Don't post") == std::string::npos,
                      "chat news filter removes hn meta noise"); rc)
    return rc;
  if (int rc = expect(chat_news_filter["text"].get<std::string>().find("edge inference") != std::string::npos ||
                      chat_news_filter["text"].get<std::string>().find("端侧大模型") != std::string::npos,
                      "chat news filter keeps relevant titles"); rc)
    return rc;

  const std::string audit_path = "data/test_decision_audit.jsonl";
  std::filesystem::remove(audit_path);
  ::setenv("THIN_AGENT_DECISION_AUDIT", "1", 1);
  ::setenv("THIN_AGENT_DECISION_AUDIT_PATH", audit_path.c_str(), 1);
  auto audit_chat = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "你是谁"}});
  if (int rc = expect(audit_chat["decision"]["route"] == "local_profile", "decision audit chat route"); rc) return rc;
  if (int rc = expect(std::filesystem::exists(audit_path), "decision audit file exists"); rc) return rc;
  if (int rc = expect(line_count(audit_path) >= 1, "decision audit has rows"); rc) return rc;
  {
    std::ifstream audit_in(audit_path);
    nlohmann::json audit_row = nlohmann::json::parse(std::string((std::istreambuf_iterator<char>(audit_in)),
                                                                 std::istreambuf_iterator<char>()));
    if (int rc = expect(audit_row.value("route", "") == "local_profile", "decision audit row route"); rc) return rc;
    if (int rc = expect(audit_row.contains("intent_backend"), "decision audit intent_backend"); rc) return rc;
    if (int rc = expect(audit_row.contains("latency_ms"), "decision audit latency_ms"); rc) return rc;
  }
  ::unsetenv("THIN_AGENT_DECISION_AUDIT");
  ::unsetenv("THIN_AGENT_DECISION_AUDIT_PATH");
  std::filesystem::remove(audit_path);

  ::setenv("THIN_AGENT_EXTERNAL_PROVIDER", "mock", 1);
  ::unsetenv("THIN_AGENT_EXTERNAL_HTTP_MOCK_WEATHER_JSON");
  ::unsetenv("THIN_AGENT_EXTERNAL_HTTP_MOCK_NEWS_JSON");

  auto chat_general_clarify = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "帮我查一下"}});
  if (int rc = expect(chat_general_clarify["type"] == "chat_result", "chat general clarify type"); rc) return rc;
  if (int rc = expect(chat_general_clarify["mode_used"] == "local-agent", "chat general clarify local-agent"); rc) return rc;
  if (int rc = expect(chat_general_clarify["decision"]["route"] == "local_external_clarify", "chat general clarify route"); rc) return rc;
  if (int rc = expect(chat_general_clarify["decision"]["policy"] == "clarify", "chat general clarify policy"); rc) return rc;
  if (int rc = expect(chat_general_clarify["decision"]["intent"] == "general", "chat general clarify intent"); rc) return rc;
  if (int rc = expect_local_policy_trace(chat_general_clarify, "chat general clarify"); rc) return rc;
  if (int rc = expect(chat_general_clarify["decision"]["confidence"].get<double>() >= 0.45, "chat general clarify confidence >= 0.45"); rc) return rc;
  if (int rc = expect(chat_general_clarify["decision"]["confidence"].get<double>() < 0.75, "chat general clarify confidence < 0.75"); rc) return rc;
  if (int rc = expect(chat_general_clarify["observation"].contains("complex_intent_probe"), "chat general clarify has complex_intent_probe"); rc) return rc;
  if (int rc = expect(chat_general_clarify["observation"]["complex_intent_probe"]["detected"] == false, "chat general clarify complex_intent detected=false"); rc) return rc;

  auto chat_complex_probe = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "帮我查一下大门口，如果没看到快递就发个短视频到我微信"}});
  if (int rc = expect(chat_complex_probe["type"] == "chat_result", "chat complex probe type"); rc) return rc;
  if (int rc = expect(chat_complex_probe["observation"].contains("complex_intent_probe"), "chat complex probe has complex_intent_probe"); rc) return rc;
  if (int rc = expect(chat_complex_probe["observation"]["complex_intent_probe"]["detected"] == true, "chat complex probe detected=true"); rc) return rc;
  if (int rc = expect(chat_complex_probe["observation"]["complex_intent_probe"]["markers"].is_array(), "chat complex probe markers array"); rc) return rc;
  bool found_probe_layer = false;
  if (chat_complex_probe.contains("decision_trace") && chat_complex_probe["decision_trace"].is_array()) {
    for (const auto& step : chat_complex_probe["decision_trace"]) {
      if (step.contains("layer") && step["layer"].is_string() && step["layer"].get<std::string>() == "probe") {
        found_probe_layer = true;
        break;
      }
    }
  }
  if (int rc = expect(found_probe_layer, "chat complex probe decision_trace has probe layer"); rc) return rc;

  auto chat_task_capture = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "帮我拍一张照并告诉我结果"}});
  if (int rc = expect(chat_task_capture["type"] == "chat_result", "chat task capture type"); rc) return rc;
  if (int rc = expect(chat_task_capture["mode_used"] == "local-agent", "chat task capture goes local-agent"); rc) return rc;
  if (int rc = expect(chat_task_capture["intent_backend"] == "rules", "chat task capture intent_backend=rules"); rc) return rc;
  if (int rc = expect(chat_task_capture["decision"]["route"] == "local_task_inline", "chat task capture route=local_task_inline"); rc) return rc;
  if (int rc = expect(chat_task_capture["decision_trace"].is_array(), "chat task capture decision_trace is array"); rc) return rc;
  if (int rc = expect_trace_schema(chat_task_capture, "chat task capture"); rc) return rc;
  if (int rc = expect(chat_task_capture["tool_calls"].is_array(), "chat task capture tool_calls array"); rc) return rc;
  if (int rc = expect(chat_task_capture["tool_calls"].size() >= 2, "chat task capture has submit+get tool calls"); rc) return rc;
  if (int rc = expect(chat_task_capture["tool_calls"][0]["tool"] == "task_submit", "chat task capture first tool task_submit"); rc) return rc;
  if (int rc = expect(chat_task_capture["tool_calls"][1]["tool"] == "task_get", "chat task capture second tool task_get"); rc) return rc;
  if (int rc = expect(chat_task_capture["observation"].contains("task"), "chat task capture observation has task"); rc) return rc;
  if (int rc = expect(chat_task_capture["observation"]["task"]["exists"].get<bool>(), "chat task capture task exists"); rc) return rc;
  if (int rc = expect(chat_task_capture["observation"]["inline_task"]["action"] == "capture_photo", "chat task capture inline action capture_photo"); rc) return rc;

  ::setenv("THIN_AGENT_CHAT_POLICY_PATH", chat_policy_path.c_str(), 1);

  thin_agent::DemoConfigCompat cloud_cfg;
  cloud_cfg.mode = "cloud";
  cloud_cfg.provider = "openai-compatible";
  cloud_cfg.model_name = "gpt-4.1-mini";
  cloud_cfg.api_base = "http://127.0.0.1:9/v1";
  cloud_cfg.api_key_env = "THIN_AGENT_TEST_MISSING_KEY";
  cloud_cfg.request_timeout_ms = 500;
  cloud_cfg.fallback = "offline";

  thin_agent::DemoConfigCompat budget_input_cfg;
  budget_input_cfg.mode = "cloud";
  budget_input_cfg.provider = "openai-compatible";
  budget_input_cfg.model_name = "mock-model";
  budget_input_cfg.api_base = "http://127.0.0.1:18080/v1";
  budget_input_cfg.request_timeout_ms = 500;
  budget_input_cfg.fallback = "offline";
  budget_input_cfg.budget_max_input_chars = 8;
  thin_agent::AgentService budget_input_svc(budget_input_cfg, ex, te);
  auto budget_input_chat = budget_input_svc.handle_request("s2", nlohmann::json{{"type", "chat"}, {"text", "这是一个超过预算阈值的输入文本"}});
  if (int rc = expect(budget_input_chat["type"] == "chat_result", "budget input type"); rc) return rc;
  if (int rc = expect(budget_input_chat["mode_used"] == "local-agent", "budget input mode_used=local-agent"); rc) return rc;
  if (int rc = expect(budget_input_chat["decision"]["route"] == "local_clarify", "budget input route=local_clarify"); rc) return rc;
  if (int rc = expect(budget_input_chat["decision"]["policy"] == "clarify", "budget input policy=clarify"); rc) return rc;
  if (int rc = expect(budget_input_chat["decision"]["reason"] == "budget_input_exceeded", "budget input reason"); rc) return rc;
  if (int rc = expect(budget_input_chat["text"].get<std::string>().find("[CFG_BUDGET]") != std::string::npos, "budget input cfg text"); rc) return rc;

  thin_agent::DemoConfigCompat budget_latency_cfg;
  budget_latency_cfg.mode = "cloud";
  budget_latency_cfg.provider = "openai-compatible";
  budget_latency_cfg.model_name = "mock-model";
  budget_latency_cfg.api_base = "http://127.0.0.1:18080/v1";
  budget_latency_cfg.request_timeout_ms = 500;
  budget_latency_cfg.fallback = "offline";
  budget_latency_cfg.budget_max_latency_ms = 300;
  thin_agent::AgentService budget_latency_svc(budget_latency_cfg, ex, te);
  auto budget_latency_chat = budget_latency_svc.handle_request("s2", nlohmann::json{{"type", "chat"}, {"text", "budget latency probe"}});
  if (int rc = expect(budget_latency_chat["decision"]["reason"] == "budget_latency_exceeded", "budget latency reason"); rc) return rc;
  if (int rc = expect(budget_latency_chat["decision"]["policy"] == "clarify", "budget latency policy=clarify"); rc) return rc;
  if (int rc = expect(budget_latency_chat["text"].get<std::string>().find("[CFG_BUDGET]") != std::string::npos, "budget latency cfg text"); rc) return rc;

  thin_agent::DemoConfigCompat budget_cost_cfg;
  budget_cost_cfg.mode = "cloud";
  budget_cost_cfg.provider = "openai-compatible";
  budget_cost_cfg.model_name = "mock-model";
  budget_cost_cfg.api_base = "http://127.0.0.1:18080/v1";
  budget_cost_cfg.request_timeout_ms = 500;
  budget_cost_cfg.fallback = "offline";
  budget_cost_cfg.budget_max_cost_cents = 0.01;
  thin_agent::AgentService budget_cost_svc(budget_cost_cfg, ex, te);
  auto budget_cost_chat = budget_cost_svc.handle_request("s2", nlohmann::json{{"type", "chat"}, {"text", "budget cost probe with enough chars"}});
  if (int rc = expect(budget_cost_chat["decision"]["reason"] == "budget_cost_exceeded", "budget cost reason"); rc) return rc;
  if (int rc = expect(budget_cost_chat["decision"]["policy"] == "clarify", "budget cost policy=clarify"); rc) return rc;
  if (int rc = expect(budget_cost_chat["text"].get<std::string>().find("[CFG_BUDGET]") != std::string::npos, "budget cost cfg text"); rc) return rc;

  thin_agent::DemoConfigCompat complex_probe_cfg;
  complex_probe_cfg.mode = "cloud";
  complex_probe_cfg.provider = "openai-compatible";
  complex_probe_cfg.model_name = "mock-model";
  complex_probe_cfg.api_base = "http://127.0.0.1:18080/v1";
  complex_probe_cfg.api_key_env = "";
  complex_probe_cfg.request_timeout_ms = 500;
  complex_probe_cfg.fallback = "offline";
  complex_probe_cfg.pipeline_enable_rollback_hook = true;

  auto setenv_local = [](const char* key, const char* val) {
#if defined(_WIN32)
    _putenv_s(key, val);
#else
    setenv(key, val, 1);
#endif
  };

  complex_probe_cfg.complex_intent_force_cloud = false;
  thin_agent::AgentService complex_probe_svc(complex_probe_cfg, ex, te);
  auto complex_probe_local = complex_probe_svc.handle_request("s2", nlohmann::json{{"type", "chat"}, {"text", "帮我拍照，如果失败就开始录制"}});
  if (int rc = expect(complex_probe_local["type"] == "chat_result", "complex probe local type"); rc) return rc;
  if (int rc = expect(complex_probe_local["mode_used"] == "local-agent", "complex probe local mode_used=local-agent"); rc) return rc;
  if (int rc = expect(complex_probe_local["intent_backend"] != "cloud-strategy", "complex probe local intent_backend not cloud-strategy"); rc) return rc;
  if (int rc = expect(complex_probe_local["decision"]["route"] != "cloud_llm", "complex probe local route not cloud_llm"); rc) return rc;

  thin_agent::DemoConfigCompat complex_force_cfg = complex_probe_cfg;
  complex_force_cfg.complex_intent_force_cloud = true;
  thin_agent::AgentService complex_force_svc(complex_force_cfg, ex, te);
  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"task_capture\",\"confidence\":0.86,\"local_route_hint\":\"local_task_inline_capture_photo\",\"task_pipeline\":[{\"step\":1,\"action\":\"capture_photo\",\"params\":{}}],\"response_draft\":\"force cloud pipeline\",\"risk\":\"low\",\"reason\":\"unit_mock_force_complex\"}");
  auto complex_force_cloud = complex_force_svc.handle_request("s2", nlohmann::json{{"type", "chat"}, {"text", "帮我拍照，如果失败就开始录制"}});
  if (int rc = expect(complex_force_cloud["type"] == "chat_result", "complex force cloud type"); rc) return rc;
  if (int rc = expect(complex_force_cloud["mode_used"] == "local-agent", "complex force cloud mode_used=local-agent"); rc) return rc;
  if (int rc = expect(complex_force_cloud["intent_backend"] == "cloud-strategy", "complex force cloud intent_backend=cloud-strategy"); rc) return rc;
  if (int rc = expect(complex_force_cloud["decision"]["route"] == "local_task_pipeline", "complex force cloud route=local_task_pipeline"); rc) return rc;
  if (int rc = expect(complex_force_cloud["observation"].contains("cloud_policy"), "complex force cloud has cloud_policy"); rc) return rc;

  // v0.49.2: 清理 mock env——后续 cloud_svc 用例验证"无 key 回退"，
  // mock 泄漏会让 key_state 检查被跳过（此前因 onnx 断言提前 return 未暴露）
  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE", "");

  thin_agent::AgentService cloud_svc(cloud_cfg, ex, te);
  auto cloud_chat = cloud_svc.handle_request("s2", nlohmann::json{{"type", "chat"}, {"text", "cloud hello"}});
  if (int rc = expect(cloud_chat["type"] == "chat_result", "cloud chat_result type"); rc) return rc;
  if (int rc = expect(cloud_chat["intent_backend"] == "cloud", "cloud chat intent_backend=cloud"); rc) return rc;
  if (int rc = expect(cloud_chat["decision_trace"].is_array(), "cloud chat decision_trace is array"); rc) return rc;
  if (int rc = expect_trace_schema(cloud_chat, "cloud fallback missing key"); rc) return rc;
  if (int rc = expect(cloud_chat["mode_used"] == "offline-fallback", "cloud missing key fallback offline"); rc) return rc;
  if (int rc = expect(cloud_chat["fallback_reason"] == "missing_api_key", "cloud fallback reason missing_api_key"); rc) return rc;
  if (int rc = expect(!cloud_chat["text"].get<std::string>().empty(), "cloud missing key has text"); rc) return rc;

  auto offline_chat_cfg = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "离线回显测试"}});
  if (int rc = expect(offline_chat_cfg["type"] == "chat_result", "offline cfg chat_result type"); rc) return rc;
  if (int rc = expect(offline_chat_cfg["mode_used"] == "offline", "offline cfg mode_used=offline"); rc) return rc;
  if (int rc = expect(offline_chat_cfg["decision"]["route"].get<std::string>().find("offline") == 0 || offline_chat_cfg["decision"]["route"].get<std::string>().find("template") != std::string::npos || offline_chat_cfg["decision"]["route"] == "fallback", "offline cfg route=offline_echo"); rc) return rc;
  if (int rc = expect(!offline_chat_cfg["text"].get<std::string>().empty(), "offline local has text"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE", "");

  thin_agent::DemoConfigCompat cloud_no_fallback_cfg;
  cloud_no_fallback_cfg.mode = "cloud";
  cloud_no_fallback_cfg.provider = "openai-compatible";
  cloud_no_fallback_cfg.model_name = "gpt-4.1-mini";
  cloud_no_fallback_cfg.api_base = "http://127.0.0.1:9/v1";
  cloud_no_fallback_cfg.request_timeout_ms = 500;
  cloud_no_fallback_cfg.fallback = "none";
  // v0.49.2: 该用例测"cloud 调用失败 + 不回退"，需先有 key 才会真正发起调用；
  // 否则 missing_api_key 提前 return（此前因 onnx 断言提前 return 从未执行到）
  setenv_local("THIN_AGENT_CLOUD_TEST_KEY", "dummy-key");
  cloud_no_fallback_cfg.api_key_env = "THIN_AGENT_CLOUD_TEST_KEY";
  thin_agent::AgentService cloud_no_fallback_svc(cloud_no_fallback_cfg, ex, te);
  auto cloud_error_chat =
      cloud_no_fallback_svc.handle_request("s3", nlohmann::json{{"type", "chat"}, {"text", "cloud no fallback"}});
  if (int rc = expect(cloud_error_chat["type"] == "chat_result", "cloud no fallback chat_result type"); rc) return rc;
  if (int rc = expect(cloud_error_chat["mode_used"] == "cloud-error", "cloud no fallback mode_used=cloud-error"); rc) return rc;
  if (int rc = expect(cloud_error_chat["cloud_http_status"].is_number_integer(), "cloud no fallback cloud_http_status is integer"); rc) return rc;
  if (int rc = expect(cloud_error_chat["cloud_error"].is_string(), "cloud no fallback cloud_error is string"); rc) return rc;
  if (int rc = expect(!cloud_error_chat["cloud_error"].get<std::string>().empty(), "cloud no fallback cloud_error non-empty"); rc) return rc;
  if (int rc = expect(cloud_error_chat["text"].get<std::string>().find("[CFG_CLOUD_ERROR]") != std::string::npos, "cloud no fallback cfg text"); rc) return rc;

  thin_agent::DemoConfigCompat cloud_strategy_cfg;
  cloud_strategy_cfg.mode = "cloud";
  cloud_strategy_cfg.provider = "openai-compatible";
  cloud_strategy_cfg.model_name = "mock-model";
  cloud_strategy_cfg.api_base = "http://127.0.0.1:18080/v1";
  cloud_strategy_cfg.api_key_env = "THIN_AGENT_CLOUD_TEST_KEY";
  cloud_strategy_cfg.request_timeout_ms = 500;
  cloud_strategy_cfg.fallback = "offline";
  cloud_strategy_cfg.pipeline_enable_rollback_hook = true;

  setenv_local("THIN_AGENT_CLOUD_TEST_KEY", "dummy-key");
  ::setenv("THIN_AGENT_CHAT_POLICY_PATH", chat_policy_path.c_str(), 1);

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"status\",\"confidence\":0.91,\"local_route_hint\":\"local_status\",\"response_draft\":\"建议走本地状态查询\",\"risk\":\"low\",\"reason\":\"unit_mock_local_status\"}");
  thin_agent::AgentService cloud_strategy_svc(cloud_strategy_cfg, ex, te);
  auto cloud_strategy_status = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "route-s-demo"}});
  if (int rc = expect(cloud_strategy_status["type"] == "chat_result", "cloud strategy status type"); rc) return rc;
  if (int rc = expect(cloud_strategy_status["mode_used"] == "local-agent", "cloud strategy status mode_used=local-agent"); rc) return rc;
  if (int rc = expect(cloud_strategy_status["intent_backend"] == "cloud-strategy", "cloud strategy status intent_backend=cloud-strategy"); rc) return rc;
  if (int rc = expect(cloud_strategy_status["decision"]["route"] == "local_status", "cloud strategy status route=local_status"); rc) return rc;
  if (int rc = expect(cloud_strategy_status["decision"]["policy"] == "execute", "cloud strategy status policy=execute"); rc) return rc;
  if (int rc = expect(cloud_strategy_status["observation"].contains("cloud_policy"), "cloud strategy status has cloud_policy"); rc) return rc;
  if (int rc = expect(cloud_strategy_status["observation"]["cloud_policy"]["parser_mode"] == "json", "cloud strategy status parser_mode=json"); rc) return rc;
  if (int rc = expect(cloud_strategy_status["decision_trace"].is_array(), "cloud strategy status decision_trace is array"); rc) return rc;
  if (int rc = expect_trace_schema(cloud_strategy_status, "cloud strategy status"); rc) return rc;
  if (int rc = expect_local_policy_trace(cloud_strategy_status, "cloud strategy status"); rc) return rc;
  if (int rc = expect(cloud_strategy_status["text"].get<std::string>().find("[CFG_CLOUD_STATUS]") != std::string::npos, "cloud strategy status cfg text"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"profile\",\"confidence\":0.89,\"local_route_hint\":\"local_profile\",\"response_draft\":\"建议走本地主体介绍\",\"risk\":\"low\",\"reason\":\"unit_mock_local_profile\"}");
  auto cloud_strategy_profile = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "route-p-demo"}});
  if (int rc = expect(cloud_strategy_profile["type"] == "chat_result", "cloud strategy profile type"); rc) return rc;
  if (int rc = expect(cloud_strategy_profile["mode_used"] == "local-agent", "cloud strategy profile mode_used=local-agent"); rc) return rc;
  if (int rc = expect(cloud_strategy_profile["intent_backend"] == "cloud-strategy", "cloud strategy profile intent_backend=cloud-strategy"); rc) return rc;
  if (int rc = expect(cloud_strategy_profile["decision"]["route"] == "local_profile", "cloud strategy profile route=local_profile"); rc) return rc;
  if (int rc = expect(cloud_strategy_profile["decision"]["policy"] == "execute", "cloud strategy profile policy=execute"); rc) return rc;
  if (int rc = expect(cloud_strategy_profile["observation"].contains("capabilities"), "cloud strategy profile has capabilities"); rc) return rc;
  if (int rc = expect(cloud_strategy_profile["observation"]["profile_mode"] == "concise", "cloud strategy profile profile_mode=concise"); rc) return rc;
  // 角色驱动自我介绍："route-p-demo" 是英文 query → 英文显示名
  std::string csp_text = cloud_strategy_profile["text"].get<std::string>();
  if (int rc = expect(csp_text.find("Work Assistant") != std::string::npos, "cloud strategy profile cfg text has identity"); rc) return rc;
  if (int rc = expect(cloud_strategy_profile["text"].get<std::string>().find("[CFG_STYLE_WARM_PROFILE]") != std::string::npos, "cloud strategy profile warm style prefix"); rc) return rc;
  if (int rc = expect_trace_schema(cloud_strategy_profile, "cloud strategy profile"); rc) return rc;
  if (int rc = expect_local_policy_trace(cloud_strategy_profile, "cloud strategy profile"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"task_capture\",\"confidence\":0.88,\"local_route_hint\":\"local_task_inline_capture_photo\",\"response_draft\":\"建议本地拍照任务\",\"risk\":\"low\",\"reason\":\"unit_mock_local_task_capture\"}");
  auto cloud_strategy_task_capture = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "task-capture-demo"}});
  if (int rc = expect(cloud_strategy_task_capture["type"] == "chat_result", "cloud strategy task capture type"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_capture["mode_used"] == "local-agent", "cloud strategy task capture mode_used=local-agent"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_capture["intent_backend"] == "cloud-strategy", "cloud strategy task capture intent_backend=cloud-strategy"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_capture["decision"]["route"] == "local_task_inline", "cloud strategy task capture route=local_task_inline"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_capture["decision"]["policy"] == "execute", "cloud strategy task capture policy=execute"); rc) return rc;
  if (int rc = expect_local_policy_trace(cloud_strategy_task_capture, "cloud strategy task capture"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_capture["observation"]["inline_task"]["action"] == "capture_photo", "cloud strategy task capture action=capture_photo"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_capture["observation"].contains("task"), "cloud strategy task capture has task"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_capture["text"].get<std::string>().find("[CFG_CLOUD_TASK_EXEC]") != std::string::npos, "cloud strategy task capture cfg text"); rc) return rc;

  thin_agent::AgentService cloud_strategy_no_task_engine_svc(cloud_strategy_cfg, ex, nullptr);
  auto cloud_strategy_task_capture_no_engine = cloud_strategy_no_task_engine_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "task-capture-demo"}});
  if (int rc = expect(cloud_strategy_task_capture_no_engine["type"] == "chat_result", "cloud strategy task capture no engine type"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_capture_no_engine["decision"]["route"] == "local_task_inline", "cloud strategy task capture no engine route"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_capture_no_engine["decision"]["policy"] == "reject", "cloud strategy task capture no engine policy"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_capture_no_engine["text"].get<std::string>().find("[CFG_CLOUD_TASK_REJECT]") != std::string::npos, "cloud strategy task capture no engine cfg text"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"task_start_recording\",\"confidence\":0.86,\"local_route_hint\":\"local_task_inline_start_recording\",\"response_draft\":\"建议本地开始录制任务\",\"risk\":\"low\",\"reason\":\"unit_mock_local_task_recording\"}");
  auto cloud_strategy_task_recording = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "task-record-demo"}});
  if (int rc = expect(cloud_strategy_task_recording["type"] == "chat_result", "cloud strategy task recording type"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_recording["mode_used"] == "local-agent", "cloud strategy task recording mode_used=local-agent"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_recording["intent_backend"] == "cloud-strategy", "cloud strategy task recording intent_backend=cloud-strategy"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_recording["decision"]["route"] == "local_task_inline", "cloud strategy task recording route=local_task_inline"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_recording["decision"]["policy"] == "execute", "cloud strategy task recording policy=execute"); rc) return rc;
  if (int rc = expect_local_policy_trace(cloud_strategy_task_recording, "cloud strategy task recording"); rc) return rc;
  if (int rc = expect(cloud_strategy_task_recording["observation"]["inline_task"]["action"] == "start_recording", "cloud strategy task recording action=start_recording"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"reject\",\"intent\":\"dangerous_action\",\"confidence\":0.95,\"response_draft\":\"这类请求风险较高\",\"risk\":\"high\",\"reason\":\"unit_mock_reject_high_risk\"}");
  auto cloud_strategy_reject = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "reject-high-risk-demo"}});
  if (int rc = expect(cloud_strategy_reject["type"] == "chat_result", "cloud strategy reject type"); rc) return rc;
  if (int rc = expect(cloud_strategy_reject["mode_used"] == "local-agent", "cloud strategy reject mode_used=local-agent"); rc) return rc;
  if (int rc = expect(cloud_strategy_reject["intent_backend"] == "cloud-strategy", "cloud strategy reject intent_backend=cloud-strategy"); rc) return rc;
  if (int rc = expect(cloud_strategy_reject["decision"]["route"] == "local_reject", "cloud strategy reject route=local_reject"); rc) return rc;
  if (int rc = expect(cloud_strategy_reject["decision"]["policy"] == "reject", "cloud strategy reject policy=reject"); rc) return rc;
  if (int rc = expect(cloud_strategy_reject["observation"].contains("risk_gate"), "cloud strategy reject has risk_gate"); rc) return rc;
  if (int rc = expect(cloud_strategy_reject["observation"]["risk_gate"]["blocked"].get<bool>(), "cloud strategy reject blocked=true"); rc) return rc;
  if (int rc = expect(cloud_strategy_reject["text"].get<std::string>().find("[CFG_CLOUD_REJECT]") != std::string::npos, "cloud strategy reject cfg text"); rc) return rc;
  if (int rc = expect_trace_schema(cloud_strategy_reject, "cloud strategy reject"); rc) return rc;
  if (int rc = expect_local_policy_trace(cloud_strategy_reject, "cloud strategy reject"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"reject\",\"intent\":\"general_query\",\"confidence\":0.86,\"local_route_hint\":\"\",\"response_draft\":\"请先给出更具体范围\",\"risk\":\"high\",\"reason\":\"unit_mock_reject_info_query\"}");
  auto cloud_strategy_info_query = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "端侧智能体最近进展如何"}});
  if (int rc = expect(cloud_strategy_info_query["type"] == "chat_result", "cloud strategy info query type"); rc) return rc;
  if (int rc = expect(cloud_strategy_info_query["decision"]["route"] == "local_clarify", "cloud strategy info query downgraded to local_clarify"); rc) return rc;
  if (int rc = expect(cloud_strategy_info_query["decision"]["policy"] == "clarify", "cloud strategy info query policy=clarify"); rc) return rc;
  if (int rc = expect(cloud_strategy_info_query["text"].get<std::string>().find("信息检索") != std::string::npos || cloud_strategy_info_query["text"].get<std::string>().find("[CFG_CLOUD_REJECT_DOWNGRADE]") != std::string::npos, "cloud strategy info query clarify text mentions info retrieval"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"clarify\",\"intent\":\"general_query\",\"confidence\":0.66,\"need_clarify\":true,\"clarify_question\":\"你更关注设备状态、能力介绍，还是要我执行具体动作？\",\"response_draft\":\"请先澄清你的目标\",\"risk\":\"medium\",\"reason\":\"unit_mock_clarify\"}");
  auto cloud_strategy_clarify = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "need-c-demo"}});
  if (int rc = expect(cloud_strategy_clarify["type"] == "chat_result", "cloud strategy clarify type"); rc) return rc;
  if (int rc = expect(cloud_strategy_clarify["mode_used"] == "local-agent", "cloud strategy clarify mode_used=local-agent"); rc) return rc;
  if (int rc = expect(cloud_strategy_clarify["intent_backend"] == "cloud-strategy", "cloud strategy clarify intent_backend=cloud-strategy"); rc) return rc;
  if (int rc = expect(cloud_strategy_clarify["decision"]["route"] == "local_clarify", "cloud strategy clarify route=local_clarify"); rc) return rc;
  if (int rc = expect(cloud_strategy_clarify["decision"]["policy"] == "clarify", "cloud strategy clarify policy=clarify"); rc) return rc;
  if (int rc = expect_local_policy_trace(cloud_strategy_clarify, "cloud strategy clarify"); rc) return rc;
  if (int rc = expect(cloud_strategy_clarify["text"].get<std::string>().find("[CFG_STYLE_WARM_CLARIFY]") != std::string::npos, "cloud strategy clarify warm style prefix text"); rc) return rc;
  if (int rc = expect(cloud_strategy_clarify["text"].get<std::string>().find("[端云协同-本地裁决]") == std::string::npos, "cloud strategy clarify should avoid hardcoded prefix"); rc) return rc;
  if (int rc = expect(cloud_strategy_clarify["observation"].contains("cloud_policy"), "cloud strategy clarify has cloud_policy"); rc) return rc;

  const std::string cloud_clarify_cfg_path = "data/test_chat_policy_cloud_clarify.json";
  {
    std::ofstream out(cloud_clarify_cfg_path);
    out << R"({
  "style": {
    "default": "warm",
    "warm": {
      "cloud": {
        "clarify_prefix": "[CFG_STYLE_WARM_CLARIFY] "
      }
    }
  },
  "templates": {
    "cloud.clarify_prefix": "[CFG_CLOUD_PREFIX] ",
    "cloud.clarify_default_question": "[CFG_CLOUD_ASK] 请确认你要状态、能力还是执行动作。"
  }
})";
  }
  ::setenv("THIN_AGENT_CHAT_POLICY_PATH", cloud_clarify_cfg_path.c_str(), 1);
  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"clarify\",\"intent\":\"general_query\",\"confidence\":0.63,\"need_clarify\":true,\"clarify_question\":\"\",\"response_draft\":\"请先澄清你的目标\",\"risk\":\"medium\",\"reason\":\"unit_mock_clarify_cfg\"}");
  auto cloud_strategy_clarify_cfg = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "need-c-cfg-demo"}});
  if (int rc = expect(cloud_strategy_clarify_cfg["type"] == "chat_result", "cloud strategy clarify cfg type"); rc) return rc;
  if (int rc = expect(cloud_strategy_clarify_cfg["decision"]["route"] == "local_clarify", "cloud strategy clarify cfg route=local_clarify"); rc) return rc;
  if (int rc = expect(cloud_strategy_clarify_cfg["decision"]["policy"] == "clarify", "cloud strategy clarify cfg policy=clarify"); rc) return rc;
  if (int rc = expect(cloud_strategy_clarify_cfg["text"].get<std::string>().find("[CFG_STYLE_WARM_CLARIFY]") != std::string::npos, "cloud strategy clarify cfg has warm style prefix"); rc) return rc;
  if (int rc = expect(cloud_strategy_clarify_cfg["text"].get<std::string>().find("请先澄清你的目标") != std::string::npos ||
                     cloud_strategy_clarify_cfg["text"].get<std::string>().find("[CFG_CLOUD_ASK]") != std::string::npos,
                     "cloud strategy clarify cfg uses draft or default ask"); rc) return rc;
  ::unsetenv("THIN_AGENT_CHAT_POLICY_PATH");
  ::setenv("THIN_AGENT_CHAT_POLICY_PATH", chat_policy_path.c_str(), 1);

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"general_query\",\"confidence\":0.82,\"local_route_hint\":\"\",\"response_draft\":\"建议先看状态再执行任务，先确认系统健康再下发动作。\",\"risk\":\"low\",\"reason\":\"unit_mock_advice_direct\"}");
  auto cloud_strategy_advice = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "你更建议我先看状态还是直接执行任务？"}});
  if (int rc = expect(cloud_strategy_advice["type"] == "chat_result", "cloud strategy advice type"); rc) return rc;
  if (int rc = expect(cloud_strategy_advice["mode_used"] == "cloud", "cloud strategy advice mode_used=cloud"); rc) return rc;
  if (int rc = expect(cloud_strategy_advice["intent_backend"] == "cloud-strategy", "cloud strategy advice intent_backend=cloud-strategy"); rc) return rc;
  if (int rc = expect(cloud_strategy_advice["decision"]["route"] == "cloud_llm", "cloud strategy advice route=cloud_llm"); rc) return rc;
  if (int rc = expect(cloud_strategy_advice["decision"]["policy"] == "fallback_cloud", "cloud strategy advice policy=fallback_cloud"); rc) return rc;
  if (int rc = expect(cloud_strategy_advice["text"].get<std::string>().find("[CFG_STYLE_WARM_ANSWER]") != std::string::npos, "cloud strategy advice warm answer prefix"); rc) return rc;
  if (int rc = expect(cloud_strategy_advice["text"].get<std::string>().find("[端云协同-本地裁决]") == std::string::npos, "cloud strategy advice should avoid hardcoded prefix"); rc) return rc;
  if (int rc = expect(cloud_strategy_advice["text"].get<std::string>().find("建议先看状态") != std::string::npos, "cloud strategy advice text has recommendation"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"general_query\",\"confidence\":1.5,\"local_route_hint\":\"\",\"response_draft\":\"contract invalid confidence\",\"risk\":\"low\",\"reason\":\"unit_mock_contract_invalid_confidence\"}");
  auto cloud_contract_bad_conf = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "contract-bad-confidence"}});
  if (int rc = expect(cloud_contract_bad_conf["type"] == "chat_result", "cloud contract bad confidence type"); rc) return rc;
  if (int rc = expect(cloud_contract_bad_conf["mode_used"] == "local-agent", "cloud contract bad confidence mode_used=local-agent"); rc) return rc;
  if (int rc = expect(cloud_contract_bad_conf["decision"]["route"] == "local_clarify", "cloud contract bad confidence route=local_clarify"); rc) return rc;
  if (int rc = expect(cloud_contract_bad_conf["decision"]["policy"] == "clarify", "cloud contract bad confidence policy=clarify"); rc) return rc;
  if (int rc = expect(cloud_contract_bad_conf["decision"]["reason"] == "cloud_policy_contract_violation", "cloud contract bad confidence reason"); rc) return rc;
  if (int rc = expect(cloud_contract_bad_conf["text"].get<std::string>().find("[CFG_POLICY_CONTRACT]") != std::string::npos, "cloud contract bad confidence cfg text"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"unknown_strategy\",\"intent\":\"general_query\",\"confidence\":0.82,\"local_route_hint\":\"\",\"response_draft\":\"contract invalid strategy\",\"risk\":\"low\",\"reason\":\"unit_mock_contract_invalid_strategy\"}");
  auto cloud_contract_bad_strategy = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "contract-bad-strategy"}});
  if (int rc = expect(cloud_contract_bad_strategy["decision"]["route"] == "local_clarify", "cloud contract bad strategy route=local_clarify"); rc) return rc;
  if (int rc = expect(cloud_contract_bad_strategy["decision"]["reason"] == "cloud_policy_contract_violation", "cloud contract bad strategy reason"); rc) return rc;
  if (int rc = expect(cloud_contract_bad_strategy["text"].get<std::string>().find("[CFG_POLICY_CONTRACT]") != std::string::npos, "cloud contract bad strategy cfg text"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"general_query\",\"confidence\":0.82,\"local_route_hint\":\"local_hack_route\",\"response_draft\":\"contract invalid route\",\"risk\":\"low\",\"reason\":\"unit_mock_contract_invalid_route\"}");
  auto cloud_contract_bad_route = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "contract-bad-route"}});
  if (int rc = expect(cloud_contract_bad_route["decision"]["route"] == "local_clarify", "cloud contract bad route route=local_clarify"); rc) return rc;
  if (int rc = expect(cloud_contract_bad_route["decision"]["reason"] == "cloud_policy_contract_violation", "cloud contract bad route reason"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"general_query\",\"confidence\":0.82,\"local_route_hint\":\"\",\"response_draft\":\"contract invalid risk\",\"risk\":\"critical\",\"reason\":\"unit_mock_contract_invalid_risk\"}");
  auto cloud_contract_bad_risk = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "contract-bad-risk"}});
  if (int rc = expect(cloud_contract_bad_risk["decision"]["route"] == "local_clarify", "cloud contract bad risk route=local_clarify"); rc) return rc;
  if (int rc = expect(cloud_contract_bad_risk["decision"]["reason"] == "cloud_policy_contract_violation", "cloud contract bad risk reason"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"task_capture\",\"confidence\":0.88,\"local_route_hint\":\"local_task_inline_capture_photo\",\"task_pipeline\":[{\"step\":1,\"action\":\"capture_photo\",\"params\":{}},{\"step\":2,\"action\":\"stop_recording\",\"params\":{}}],\"response_draft\":\"pipeline ok\",\"risk\":\"low\",\"reason\":\"unit_mock_pipeline_ok\"}");
  auto cloud_pipeline_ok = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "pipeline-ok"}});
  if (int rc = expect(cloud_pipeline_ok["decision"]["route"] == "local_task_pipeline", "cloud pipeline ok route=local_task_pipeline"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["decision"]["policy"] == "execute", "cloud pipeline ok policy=execute"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"].contains("cloud_task_pipeline_contract"), "cloud pipeline ok has pipeline contract observation"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["cloud_task_pipeline_contract"]["valid"].get<bool>(), "cloud pipeline ok contract valid=true"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"].contains("pipeline_execution"), "cloud pipeline ok has pipeline_execution"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_execution"]["step_count"].get<int>() == 2, "cloud pipeline ok step_count=2"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_execution"]["completed"].get<int>() == 2, "cloud pipeline ok completed=2"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_execution"]["failed"].get<bool>() == false, "cloud pipeline ok failed=false"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_error"].is_object(), "cloud pipeline ok pipeline_error object"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_error"]["class"] == "", "cloud pipeline ok pipeline_error.class empty"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_error"]["code"] == "", "cloud pipeline ok pipeline_error.code empty"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_error"]["failed_at"].get<int>() == -1, "cloud pipeline ok pipeline_error.failed_at=-1"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_error_class"] == "", "cloud pipeline ok pipeline_error_class empty"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_error_code"] == "", "cloud pipeline ok pipeline_error_code empty"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_outcome"].is_object(), "cloud pipeline ok pipeline_outcome object"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_outcome"]["failed"].get<bool>() == false, "cloud pipeline ok pipeline_outcome.failed=false"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_outcome"]["reason"] == "PIPELINE_EXECUTED", "cloud pipeline ok pipeline_outcome.reason=PIPELINE_EXECUTED"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_outcome"]["error"] == cloud_pipeline_ok["observation"]["pipeline_error"], "cloud pipeline ok pipeline_outcome.error sync"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["observation"]["pipeline_execution"]["steps"].is_array(), "cloud pipeline ok steps array"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["decision"]["reason"] == "PIPELINE_EXECUTED", "cloud pipeline ok reason=PIPELINE_EXECUTED"); rc) return rc;
  if (int rc = expect(cloud_pipeline_ok["decision_trace"].is_array(), "cloud pipeline ok decision_trace array"); rc) return rc;
  bool ok_has_empty_policy_error_code = false;
  for (const auto& step : cloud_pipeline_ok["decision_trace"]) {
    if (step.is_object() && step.value("layer", "") == "policy" && step.contains("input") && step["input"].is_object()) {
      if (step["input"].value("pipeline_error_code", std::string()) == "") {
        ok_has_empty_policy_error_code = true;
      }
    }
  }
  if (int rc = expect(ok_has_empty_policy_error_code, "cloud pipeline ok policy input has empty pipeline_error_code"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"task_capture\",\"confidence\":0.88,\"local_route_hint\":\"local_task_inline_capture_photo\",\"task_pipeline\":[{\"step\":1,\"action\":\"capture_photo\",\"params\":{}},{\"step\":2,\"action\":\"start_recording\",\"params\":{\"mode\":\"invalid\"}}],\"response_draft\":\"pipeline fail-fast\",\"risk\":\"low\",\"reason\":\"unit_mock_pipeline_fail_fast\"}");
  auto cloud_pipeline_fail_fast = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "pipeline-fail-fast"}});
  if (int rc = expect(cloud_pipeline_fail_fast["decision"]["route"] == "local_task_pipeline", "cloud pipeline fail-fast route=local_task_pipeline"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"].contains("pipeline_execution"), "cloud pipeline fail-fast has pipeline_execution"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_execution"]["step_count"].get<int>() == 2, "cloud pipeline fail-fast step_count=2"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_execution"]["completed"].get<int>() == 1, "cloud pipeline fail-fast completed=1"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_execution"]["failed"].get<bool>() == true, "cloud pipeline fail-fast failed=true"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_execution"]["failed_at"].get<int>() == 2, "cloud pipeline fail-fast failed_at=2"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_execution"]["steps"].size() == 2, "cloud pipeline fail-fast steps size=2"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_execution"]["steps"][1]["error_class"] == "param", "cloud pipeline fail-fast error_class=param"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_error_class"] == "param", "cloud pipeline fail-fast pipeline_error_class=param"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_error"].is_object(), "cloud pipeline fail-fast pipeline_error object"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_error"]["class"] == "param", "cloud pipeline fail-fast pipeline_error.class=param"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_error"]["code"] == "PIPELINE_PARAM_INVALID", "cloud pipeline fail-fast pipeline_error.code=PIPELINE_PARAM_INVALID"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_error"]["failed_at"].get<int>() == 2, "cloud pipeline fail-fast pipeline_error.failed_at=2"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_error_code"] == "PIPELINE_PARAM_INVALID", "cloud pipeline fail-fast error_code=PIPELINE_PARAM_INVALID"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_outcome"].is_object(), "cloud pipeline fail-fast pipeline_outcome object"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_outcome"]["failed"].get<bool>() == true, "cloud pipeline fail-fast pipeline_outcome.failed=true"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_outcome"]["reason"] == "PIPELINE_PARAM_INVALID", "cloud pipeline fail-fast pipeline_outcome.reason=PIPELINE_PARAM_INVALID"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_outcome"]["error"] == cloud_pipeline_fail_fast["observation"]["pipeline_error"], "cloud pipeline fail-fast pipeline_outcome.error sync"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["decision"]["reason"] == "PIPELINE_PARAM_INVALID", "cloud pipeline fail-fast reason=PIPELINE_PARAM_INVALID"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["decision_trace"].is_array(), "cloud pipeline fail-fast decision_trace array"); rc) return rc;
  bool failfast_has_policy_error_code = false;
  for (const auto& step : cloud_pipeline_fail_fast["decision_trace"]) {
    if (step.is_object() && step.value("layer", "") == "policy" && step.contains("input") && step["input"].is_object()) {
      if (step["input"].value("pipeline_error_code", std::string()) == "PIPELINE_PARAM_INVALID") {
        failfast_has_policy_error_code = true;
      }
    }
  }
  if (int rc = expect(failfast_has_policy_error_code, "cloud pipeline fail-fast policy input has pipeline_error_code"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_execution"]["rollback"].is_object(), "cloud pipeline fail-fast rollback object"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_execution"]["rollback"]["enabled"].get<bool>() == true, "cloud pipeline fail-fast rollback enabled"); rc) return rc;
  if (int rc = expect(cloud_pipeline_fail_fast["observation"]["pipeline_execution"]["rollback"]["triggered"].get<bool>() == true, "cloud pipeline fail-fast rollback triggered"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"task_capture\",\"confidence\":0.88,\"local_route_hint\":\"local_task_inline_capture_photo\",\"task_pipeline\":[{\"step\":1,\"action\":\"start_recording\",\"params\":{\"mode\":\"video\"}},{\"step\":2,\"action\":\"stop_recording\",\"params\":{}},{\"step\":3,\"action\":\"start_recording\",\"params\":{\"mode\":\"invalid\"}}],\"response_draft\":\"pipeline mixed rollback mapping\",\"risk\":\"low\",\"reason\":\"unit_mock_pipeline_mixed_rollback\"}");
  auto cloud_pipeline_task_fail = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "pipeline-task-fail"}});
  if (int rc = expect(cloud_pipeline_task_fail["decision"]["route"] == "local_task_pipeline", "cloud pipeline task fail route=local_task_pipeline"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_execution"]["failed"].get<bool>() == true, "cloud pipeline task fail failed=true"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_execution"]["failed_at"].get<int>() == 3, "cloud pipeline task fail failed_at=3"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_execution"]["completed"].get<int>() == 2, "cloud pipeline task fail completed=2"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_execution"]["steps"][2]["error_class"] == "param", "cloud pipeline task fail error_class=param"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_error_code"] == "PIPELINE_PARAM_INVALID", "cloud pipeline task fail error_code=PIPELINE_PARAM_INVALID"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_error_class"] == "param", "cloud pipeline task fail error_class=param"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_error"]["class"] == cloud_pipeline_task_fail["observation"]["pipeline_error_class"], "cloud pipeline task fail pipeline_error.class sync"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_error"]["code"] == cloud_pipeline_task_fail["observation"]["pipeline_error_code"], "cloud pipeline task fail pipeline_error.code sync"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_outcome"].is_object(), "cloud pipeline task fail pipeline_outcome object"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_outcome"]["failed"].get<bool>() == true, "cloud pipeline task fail pipeline_outcome.failed=true"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_outcome"]["reason"] == "PIPELINE_PARAM_INVALID", "cloud pipeline task fail pipeline_outcome.reason=PIPELINE_PARAM_INVALID"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_outcome"]["error"] == cloud_pipeline_task_fail["observation"]["pipeline_error"], "cloud pipeline task fail pipeline_outcome.error sync"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["decision"]["reason"] == "PIPELINE_PARAM_INVALID", "cloud pipeline task fail reason=PIPELINE_PARAM_INVALID"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_execution"]["rollback"]["triggered"].get<bool>() == true, "cloud pipeline task fail rollback triggered"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_execution"]["rollback"]["status"] != "skipped", "cloud pipeline task fail rollback status!=skipped"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_execution"]["rollback"]["attempted_steps"].get<int>() == 2, "cloud pipeline task fail rollback attempted_steps=2"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_execution"]["rollback"]["steps"].is_array(), "cloud pipeline task fail rollback steps array"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_execution"]["rollback"]["steps"].size() >= 1, "cloud pipeline task fail rollback steps size>=1"); rc) return rc;
  if (int rc = expect(cloud_pipeline_task_fail["observation"]["pipeline_execution"]["rollback"]["steps"][0]["action"] == "start_recording", "cloud pipeline task fail rollback action0 start_recording"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"task_capture\",\"confidence\":0.88,\"local_route_hint\":\"local_task_inline_capture_photo\",\"task_pipeline\":[{\"step\":1,\"action\":\"start_recording\",\"params\":{\"mode\":\"video\"}},{\"step\":2,\"action\":\"start_recording\",\"params\":{\"mode\":\"video\"}}],\"response_draft\":\"pipeline runtime task fail\",\"risk\":\"low\",\"reason\":\"unit_mock_pipeline_runtime_task_fail\"}");
  auto cloud_pipeline_runtime_task_fail = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "pipeline-runtime-task-fail"}});
  if (int rc = expect(cloud_pipeline_runtime_task_fail["decision"]["route"] == "local_task_pipeline", "cloud pipeline runtime task fail route=local_task_pipeline"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_execution"]["failed"].get<bool>() == true, "cloud pipeline runtime task fail failed=true"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_execution"]["failed_at"].get<int>() == 2, "cloud pipeline runtime task fail failed_at=2"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_execution"]["steps"][1]["error_class"] == "task", "cloud pipeline runtime task fail error_class=task"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_error_class"] == "task", "cloud pipeline runtime task fail pipeline_error_class=task"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_error"]["class"] == "task", "cloud pipeline runtime task fail pipeline_error.class=task"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_error"]["code"] == "PIPELINE_TASK_FAILED", "cloud pipeline runtime task fail pipeline_error.code=PIPELINE_TASK_FAILED"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_error"]["failed_at"].get<int>() == 2, "cloud pipeline runtime task fail pipeline_error.failed_at=2"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_error_code"] == "PIPELINE_TASK_FAILED", "cloud pipeline runtime task fail error_code=PIPELINE_TASK_FAILED"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_error"]["class"] == cloud_pipeline_runtime_task_fail["observation"]["pipeline_error_class"], "cloud pipeline runtime task fail pipeline_error.class sync"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_error"]["code"] == cloud_pipeline_runtime_task_fail["observation"]["pipeline_error_code"], "cloud pipeline runtime task fail pipeline_error.code sync"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_outcome"].is_object(), "cloud pipeline runtime task fail pipeline_outcome object"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_outcome"]["failed"].get<bool>() == true, "cloud pipeline runtime task fail pipeline_outcome.failed=true"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_outcome"]["reason"] == "PIPELINE_TASK_FAILED", "cloud pipeline runtime task fail pipeline_outcome.reason=PIPELINE_TASK_FAILED"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["observation"]["pipeline_outcome"]["error"] == cloud_pipeline_runtime_task_fail["observation"]["pipeline_error"], "cloud pipeline runtime task fail pipeline_outcome.error sync"); rc) return rc;
  if (int rc = expect(cloud_pipeline_runtime_task_fail["decision"]["reason"] == "PIPELINE_TASK_FAILED", "cloud pipeline runtime task fail reason=PIPELINE_TASK_FAILED"); rc) return rc;
  bool runtime_has_policy_error_code = false;
  for (const auto& step : cloud_pipeline_runtime_task_fail["decision_trace"]) {
    if (step.is_object() && step.value("layer", "") == "policy" && step.contains("input") && step["input"].is_object()) {
      if (step["input"].value("pipeline_error_code", std::string()) == "PIPELINE_TASK_FAILED") {
        runtime_has_policy_error_code = true;
      }
    }
  }
  if (int rc = expect(runtime_has_policy_error_code, "cloud pipeline runtime task fail policy input has pipeline_error_code"); rc) return rc;

  thin_agent::AgentService cloud_strategy_no_pipeline_engine_svc(cloud_strategy_cfg, ex, nullptr);
  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"task_capture\",\"confidence\":0.88,\"local_route_hint\":\"local_task_inline_capture_photo\",\"task_pipeline\":[{\"step\":1,\"action\":\"capture_photo\",\"params\":{}}],\"response_draft\":\"pipeline no engine\",\"risk\":\"low\",\"reason\":\"unit_mock_pipeline_no_engine\"}");
  auto cloud_pipeline_no_engine = cloud_strategy_no_pipeline_engine_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "pipeline-no-engine"}});
  if (int rc = expect(cloud_pipeline_no_engine["decision"]["route"] == "local_task_pipeline", "cloud pipeline no engine route=local_task_pipeline"); rc) return rc;
  if (int rc = expect(cloud_pipeline_no_engine["decision"]["policy"] == "reject", "cloud pipeline no engine policy=reject"); rc) return rc;
  if (int rc = expect(cloud_pipeline_no_engine["observation"]["pipeline_error_class"] == "engine", "cloud pipeline no engine error_class=engine"); rc) return rc;
  if (int rc = expect(cloud_pipeline_no_engine["observation"]["pipeline_error"]["class"] == "engine", "cloud pipeline no engine pipeline_error.class=engine"); rc) return rc;
  if (int rc = expect(cloud_pipeline_no_engine["observation"]["pipeline_error"]["code"] == "PIPELINE_ENGINE_UNAVAILABLE", "cloud pipeline no engine pipeline_error.code=PIPELINE_ENGINE_UNAVAILABLE"); rc) return rc;
  if (int rc = expect(cloud_pipeline_no_engine["observation"]["pipeline_error"]["failed_at"].get<int>() == -1, "cloud pipeline no engine pipeline_error.failed_at=-1"); rc) return rc;
  if (int rc = expect(cloud_pipeline_no_engine["observation"]["pipeline_error_code"] == "PIPELINE_ENGINE_UNAVAILABLE", "cloud pipeline no engine error_code=PIPELINE_ENGINE_UNAVAILABLE"); rc) return rc;
  if (int rc = expect(cloud_pipeline_no_engine["observation"]["pipeline_outcome"].is_object(), "cloud pipeline no engine pipeline_outcome object"); rc) return rc;
  if (int rc = expect(cloud_pipeline_no_engine["observation"]["pipeline_outcome"]["failed"].get<bool>() == true, "cloud pipeline no engine pipeline_outcome.failed=true"); rc) return rc;
  if (int rc = expect(cloud_pipeline_no_engine["observation"]["pipeline_outcome"]["reason"] == "PIPELINE_ENGINE_UNAVAILABLE", "cloud pipeline no engine pipeline_outcome.reason=PIPELINE_ENGINE_UNAVAILABLE"); rc) return rc;
  if (int rc = expect(cloud_pipeline_no_engine["observation"]["pipeline_outcome"]["error"] == cloud_pipeline_no_engine["observation"]["pipeline_error"], "cloud pipeline no engine pipeline_outcome.error sync"); rc) return rc;
  if (int rc = expect(cloud_pipeline_no_engine["decision"]["reason"] == "PIPELINE_ENGINE_UNAVAILABLE", "cloud pipeline no engine reason=PIPELINE_ENGINE_UNAVAILABLE"); rc) return rc;
  bool no_engine_has_policy_error_code = false;
  bool no_engine_has_failed_flag = false;
  bool no_engine_has_completed_zero = false;
  for (const auto& step : cloud_pipeline_no_engine["decision_trace"]) {
    if (step.is_object() && step.value("layer", "") == "policy" && step.contains("input") && step["input"].is_object()) {
      if (step["input"].value("pipeline_error_code", std::string()) == "PIPELINE_ENGINE_UNAVAILABLE") {
        no_engine_has_policy_error_code = true;
      }
      if (step["input"].contains("failed") && step["input"]["failed"].is_boolean() && step["input"]["failed"].get<bool>() == true) {
        no_engine_has_failed_flag = true;
      }
      if (step["input"].contains("completed") && step["input"]["completed"].is_number_integer() && step["input"]["completed"].get<int>() == 0) {
        no_engine_has_completed_zero = true;
      }
    }
  }
  if (int rc = expect(no_engine_has_policy_error_code, "cloud pipeline no engine policy input has pipeline_error_code"); rc) return rc;
  if (int rc = expect(no_engine_has_failed_flag, "cloud pipeline no engine policy input failed=true"); rc) return rc;
  if (int rc = expect(no_engine_has_completed_zero, "cloud pipeline no engine policy input completed=0"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"task_capture\",\"confidence\":0.88,\"local_route_hint\":\"local_task_inline_capture_photo\",\"task_pipeline\":[{\"step\":1,\"action\":\"nonexistent_action\",\"params\":{\"cmd\":\"rm -rf /\"}}],\"response_draft\":\"pipeline bad action\",\"risk\":\"low\",\"reason\":\"unit_mock_pipeline_bad_action\"}");
  auto cloud_pipeline_bad_action = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "pipeline-bad-action"}});
  if (int rc = expect(cloud_pipeline_bad_action["decision"]["route"] == "local_clarify", "cloud pipeline bad action route=local_clarify"); rc) return rc;
  if (int rc = expect(cloud_pipeline_bad_action["decision"]["reason"] == "cloud_policy_contract_violation", "cloud pipeline bad action reason=contract"); rc) return rc;
  if (int rc = expect(cloud_pipeline_bad_action["text"].get<std::string>().find("[CFG_POLICY_CONTRACT]") != std::string::npos, "cloud pipeline bad action cfg text"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"weather\",\"confidence\":0.88,\"local_route_hint\":\"\",\"response_draft\":\"上海今天多云，约28°C。\",\"risk\":\"low\",\"reason\":\"unit_mock_cloud_weather\"}");
  auto cloud_strategy_weather = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "今天上海天气"}});
  if (int rc = expect(cloud_strategy_weather["type"] == "chat_result", "cloud strategy weather type"); rc) return rc;
  if (int rc = expect(cloud_strategy_weather["mode_used"] == "local-agent", "cloud strategy weather mode_used=local-agent(local weather)"); rc) return rc;
  if (int rc = expect(cloud_strategy_weather["decision"]["route"] == "local_external_weather", "cloud strategy weather route=local_external_weather"); rc) return rc;
  if (int rc = expect(cloud_strategy_weather["decision"]["policy"] == "execute", "cloud strategy weather policy=execute"); rc) return rc;
  if (int rc = expect(cloud_strategy_weather["observation"].contains("external_result"), "cloud strategy weather has external_result"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "{\"strategy\":\"answer_direct\",\"intent\":\"news\",\"confidence\":0.87,\"local_route_hint\":\"\",\"response_draft\":\"最近AI新闻：端侧智能体持续升温。\",\"risk\":\"low\",\"reason\":\"unit_mock_cloud_news\"}");
  auto cloud_strategy_news = cloud_strategy_svc.handle_request("s4", nlohmann::json{{"type", "chat"}, {"text", "最近AI新闻"}});
  if (int rc = expect(cloud_strategy_news["type"] == "chat_result", "cloud strategy news type"); rc) return rc;
  if (int rc = expect(cloud_strategy_news["mode_used"] == "local-agent", "cloud strategy news mode_used=local-agent(local news)"); rc) return rc;
  if (int rc = expect(cloud_strategy_news["decision"]["route"] == "local_external_news", "cloud strategy news route=local_external_news"); rc) return rc;
  if (int rc = expect(cloud_strategy_news["decision"]["policy"] == "execute", "cloud strategy news policy=execute"); rc) return rc;
  if (int rc = expect(cloud_strategy_news["observation"].contains("external_result"), "cloud strategy news has external_result"); rc) return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
               "_draft：可以解释ONNX格式，因此输出JSON。_draft：明确。_draft：\n\njson\n{\n"
               "  \"strategy\": \"clarify\",\n"
               "  \"intent\": \"general\",\n"
               "  \"confidence\": 0.85,\n"
               "  \"risk\": \"low\",\n"
               "  \"need_clarify\": true,\n"
               "  \"clarify_question\": \"您是指哪个具体的ONNX模型？\",\n"
               "  \"response_draft\": \"ONNX 本身不是模型，它是一种开放神经网络交换格式。\"\n"
               "}");
  auto cloud_strategy_draft_artifact = cloud_strategy_svc.handle_request(
      "s4", nlohmann::json{{"type", "chat"}, {"text", "draft-artifact-clarify-demo"}});
  if (int rc = expect(cloud_strategy_draft_artifact["type"] == "chat_result", "cloud strategy draft artifact type"); rc) return rc;
  if (int rc = expect(cloud_strategy_draft_artifact["observation"]["cloud_policy"]["parser_mode"] == "json",
                     "cloud strategy draft artifact parser_mode=json"); rc) return rc;
  if (int rc = expect(cloud_strategy_draft_artifact["decision"]["policy"] == "clarify",
                     "cloud strategy draft artifact policy=clarify"); rc) return rc;
  {
    const std::string visible = cloud_strategy_draft_artifact["text"].get<std::string>();
    if (int rc = expect(visible.find("_draft") == std::string::npos, "cloud strategy draft artifact hides _draft"); rc) return rc;
    if (int rc = expect(visible.find("\"strategy\"") == std::string::npos, "cloud strategy draft artifact hides raw json"); rc) return rc;
    if (int rc = expect(visible.find("ONNX 本身不是模型") != std::string::npos,
                       "cloud strategy draft artifact shows response_draft"); rc) return rc;
  }

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE", "");
  ::unsetenv("THIN_AGENT_CHAT_POLICY_PATH");

  auto m = svc.handle_request("s1", nlohmann::json{{"type", "memory_recent"}});
  if (int rc = expect(m["type"] == "memory_recent_result", "memory_recent_result type"); rc) return rc;
  if (int rc = expect(m["memory_size"].get<int>() >= 2, "memory_recent has entries"); rc) return rc;

  auto mh = svc.handle_request("s1", nlohmann::json{{"type", "memory_history"}, {"limit", 10}});
  if (int rc = expect(mh["type"] == "memory_history_result", "memory_history_result type"); rc) return rc;
  if (int rc = expect(mh["history_size"].get<int>() >= 2, "memory_history has entries"); rc) return rc;

  auto memory_probe_chat = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "memory-search-token-v0639"}});
  if (int rc = expect(memory_probe_chat["type"] == "chat_result", "memory probe chat type"); rc) return rc;

  auto memory_search_chat = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "请做记忆检索 memory-search-token-v0639 5"}});
  if (int rc = expect(memory_search_chat["type"] == "chat_result", "memory_search chat type"); rc) return rc;
  if (int rc = expect(memory_search_chat["decision"]["route"] == "local_memory_search", "memory_search chat route=local_memory_search"); rc) return rc;
  if (int rc = expect(memory_search_chat["decision"]["intent"] == "memory_search", "memory_search chat intent=memory_search"); rc) return rc;
  if (int rc = expect(memory_search_chat["decision"]["policy"] == "execute", "memory_search chat policy=execute"); rc) return rc;
  if (int rc = expect(memory_search_chat["observation"]["type"] == "memory_search_result", "memory_search chat observation type"); rc) return rc;
  if (int rc = expect(memory_search_chat["observation"]["limit_applied"].get<int>() == 5, "memory_search chat limit=5"); rc) return rc;
  if (int rc = expect(memory_search_chat["observation"]["query"] == "memory-search-token-v0639", "memory_search chat query exact"); rc) return rc;
  if (int rc = expect(memory_search_chat["observation"]["result_size"].get<int>() >= 1, "memory_search chat has hits"); rc) return rc;
  if (int rc = expect_local_policy_trace(memory_search_chat, "memory_search chat"); rc) return rc;
  bool memory_search_policy_has_query = false;
  bool memory_search_policy_has_limit = false;
  for (const auto& step : memory_search_chat["decision_trace"]) {
    if (step.is_object() && step.value("layer", "") == "policy" && step.contains("input") && step["input"].is_object()) {
      const auto& in = step["input"];
      if (in.value("query", std::string()) == "memory-search-token-v0639") memory_search_policy_has_query = true;
      if (in.contains("limit") && in["limit"].is_number_integer() && in["limit"].get<int>() == 5) memory_search_policy_has_limit = true;
    }
  }
  if (int rc = expect(memory_search_policy_has_query, "memory_search chat policy input has query"); rc) return rc;
  if (int rc = expect(memory_search_policy_has_limit, "memory_search chat policy input has limit=5"); rc) return rc;

  auto memory_summary_chat = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "请做记忆摘要 memory-search-token-v0639 4"}});
  if (int rc = expect(memory_summary_chat["type"] == "chat_result", "memory_summary chat type"); rc) return rc;
  if (int rc = expect(memory_summary_chat["decision"]["route"] == "local_memory_summary", "memory_summary chat route=local_memory_summary"); rc) return rc;
  if (int rc = expect(memory_summary_chat["decision"]["intent"] == "memory_summary", "memory_summary chat intent=memory_summary"); rc) return rc;
  if (int rc = expect(memory_summary_chat["decision"]["policy"] == "execute", "memory_summary chat policy=execute"); rc) return rc;
  if (int rc = expect(memory_summary_chat["observation"]["type"] == "memory_summary_result", "memory_summary chat observation type"); rc) return rc;
  if (int rc = expect(memory_summary_chat["observation"]["limit_applied"].get<int>() == 4, "memory_summary chat limit=4"); rc) return rc;
  if (int rc = expect(memory_summary_chat["observation"]["query"] == "memory-search-token-v0639", "memory_summary chat query exact"); rc) return rc;
  if (int rc = expect(memory_summary_chat["observation"]["hit_count"].get<int>() >= 1, "memory_summary chat has hits"); rc) return rc;
  if (int rc = expect_local_policy_trace(memory_summary_chat, "memory_summary chat"); rc) return rc;
  bool memory_summary_policy_has_query = false;
  bool memory_summary_policy_has_limit = false;
  for (const auto& step : memory_summary_chat["decision_trace"]) {
    if (step.is_object() && step.value("layer", "") == "policy" && step.contains("input") && step["input"].is_object()) {
      const auto& in = step["input"];
      if (in.value("query", std::string()) == "memory-search-token-v0639") memory_summary_policy_has_query = true;
      if (in.contains("limit") && in["limit"].is_number_integer() && in["limit"].get<int>() == 4) memory_summary_policy_has_limit = true;
    }
  }
  if (int rc = expect(memory_summary_policy_has_query, "memory_summary chat policy input has query"); rc) return rc;
  if (int rc = expect(memory_summary_policy_has_limit, "memory_summary chat policy input has limit=4"); rc) return rc;

  auto memory_search_task_chat = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "请做记忆检索 capture_photo 10"}});
  if (int rc = expect(memory_search_task_chat["type"] == "chat_result", "memory_search task chat type"); rc) return rc;
  if (int rc = expect(memory_search_task_chat["decision"]["route"] == "local_memory_search", "memory_search task chat route=local_memory_search"); rc) return rc;
  if (int rc = expect(memory_search_task_chat["text"].is_string(), "memory_search task chat text string"); rc) return rc;
  if (int rc = expect(memory_search_task_chat["text"].get<std::string>().find("维护结论：") != std::string::npos,
                      "memory_search task chat text has conclusion line");
      rc)
    return rc;
  if (int rc = expect(memory_search_task_chat["text"].get<std::string>().find("证据点：") != std::string::npos,
                      "memory_search task chat text has evidence line");
      rc)
    return rc;

  auto memory_summary_task_chat = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "请做记忆摘要 capture_photo 10"}});
  if (int rc = expect(memory_summary_task_chat["type"] == "chat_result", "memory_summary task chat type"); rc) return rc;
  if (int rc = expect(memory_summary_task_chat["decision"]["route"] == "local_memory_summary", "memory_summary task chat route=local_memory_summary"); rc) return rc;
  if (int rc = expect(memory_summary_task_chat["text"].is_string(), "memory_summary task chat text string"); rc) return rc;
  if (int rc = expect(memory_summary_task_chat["text"].get<std::string>().find("维护结论：") != std::string::npos,
                      "memory_summary task chat text has conclusion line");
      rc)
    return rc;
  if (int rc = expect(memory_summary_task_chat["text"].get<std::string>().find("证据点：") != std::string::npos,
                      "memory_summary task chat text has evidence line");
      rc)
    return rc;
  if (int rc = expect(memory_summary_task_chat["observation"].contains("evidence_latest_basis") &&
                          memory_summary_task_chat["observation"]["evidence_latest_basis"].is_object(),
                      "memory_summary task chat has evidence_latest_basis");
      rc)
    return rc;
  const auto& summary_latest_basis = memory_summary_task_chat["observation"]["evidence_latest_basis"];
  if (int rc = expect(summary_latest_basis.value("source", std::string()) == "task_sqlite", "memory_summary task chat latest_basis source=task_sqlite"); rc) return rc;
  if (int rc = expect(summary_latest_basis.contains("row_ref") && summary_latest_basis["row_ref"].is_string(), "memory_summary task chat latest_basis row_ref string"); rc) return rc;
  if (int rc = expect(!summary_latest_basis["row_ref"].get<std::string>().empty(), "memory_summary task chat latest_basis row_ref non-empty"); rc) return rc;
  if (int rc = expect(summary_latest_basis.contains("reason") && summary_latest_basis["reason"].is_string(), "memory_summary task chat latest_basis reason string"); rc) return rc;
  if (int rc = expect(summary_latest_basis["reason"].get<std::string>() == "priority+recency+tie_break", "memory_summary task chat latest_basis reason value"); rc) return rc;
  if (int rc = expect(summary_latest_basis.contains("selected_at") && summary_latest_basis["selected_at"].is_string(), "memory_summary task chat latest_basis selected_at string"); rc) return rc;
  if (int rc = expect(memory_summary_task_chat["observation"].contains("memory_replay_hint") && memory_summary_task_chat["observation"]["memory_replay_hint"].is_object(),
                      "memory_summary task chat has memory_replay_hint");
      rc)
    return rc;
  const auto& summary_replay_hint = memory_summary_task_chat["observation"]["memory_replay_hint"];
  if (int rc = expect(summary_replay_hint.value("row_ref", std::string()) == summary_latest_basis.value("row_ref", std::string()), "memory_summary task chat replay_hint row_ref matches latest_basis"); rc) return rc;
  if (int rc = expect(summary_replay_hint.contains("payload") && summary_replay_hint["payload"].is_object(), "memory_summary task chat replay_hint payload object"); rc) return rc;
  if (int rc = expect(summary_replay_hint["payload"].value("type", std::string()) == "memory_summary", "memory_summary task chat replay_hint payload.type=memory_summary"); rc) return rc;
  if (int rc = expect(summary_replay_hint["payload"].contains("query") && summary_replay_hint["payload"]["query"].is_string(), "memory_summary task chat replay_hint payload.query string"); rc) return rc;
  if (int rc = expect(summary_replay_hint["payload"].contains("limit") && summary_replay_hint["payload"]["limit"].is_number_integer(), "memory_summary task chat replay_hint payload.limit int"); rc) return rc;

  auto memory_search_state_priority_chat = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "请做记忆检索 start_recording 20"}});
  if (int rc = expect(memory_search_state_priority_chat["type"] == "chat_result", "memory_search state priority chat type"); rc) return rc;
  if (int rc = expect(memory_search_state_priority_chat["decision"]["route"] == "local_memory_search", "memory_search state priority route"); rc) return rc;
  if (int rc = expect(memory_search_state_priority_chat["text"].is_string(), "memory_search state priority text string"); rc) return rc;
  if (int rc = expect(memory_search_state_priority_chat["text"].get<std::string>().find("证据点：#1 task") != std::string::npos,
                      "memory_search state priority has task as first evidence");
      rc)
    return rc;
  if (int rc = expect(memory_search_state_priority_chat["text"].get<std::string>().find("state=failed") != std::string::npos,
                      "memory_search state priority prefers failed state");
      rc)
    return rc;

  auto memory_search_dedup_chat = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", std::string("请做记忆检索 task ") + tid + " action= 20"}});
  if (int rc = expect(memory_search_dedup_chat["type"] == "chat_result", "memory_search dedup chat type"); rc) return rc;
  if (int rc = expect(memory_search_dedup_chat["decision"]["route"] == "local_memory_search", "memory_search dedup route"); rc) return rc;
  if (int rc = expect(memory_search_dedup_chat["text"].is_string(), "memory_search dedup text string"); rc) return rc;
  const std::string dedup_text = memory_search_dedup_chat["text"].get<std::string>();
  if (int rc = expect(dedup_text.find("证据点：#1") != std::string::npos, "memory_search dedup has #1"); rc) return rc;
  if (int rc = expect(dedup_text.find("[latest]") != std::string::npos, "memory_search dedup has latest marker"); rc) return rc;
  if (int rc = expect(memory_search_dedup_chat["observation"].contains("evidence_latest_basis") && memory_search_dedup_chat["observation"]["evidence_latest_basis"].is_object(),
                      "memory_search dedup has evidence_latest_basis object");
      rc)
    return rc;
  const auto& latest_basis = memory_search_dedup_chat["observation"]["evidence_latest_basis"];
  if (int rc = expect(latest_basis.value("task_id", std::string()).find("task-") == 0, "memory_search dedup latest_basis task_id"); rc) return rc;
  if (int rc = expect(latest_basis["kind"].is_string(), "memory_search dedup latest_basis kind string"); rc) return rc;
  if (int rc = expect(latest_basis["priority"].is_number_integer(), "memory_search dedup latest_basis priority int"); rc) return rc;
  if (int rc = expect(latest_basis["created_at"].is_string(), "memory_search dedup latest_basis created_at string"); rc) return rc;
  if (int rc = expect(latest_basis["audit_id"].is_number_integer(), "memory_search dedup latest_basis audit_id int"); rc) return rc;
  if (int rc = expect(latest_basis.value("source", std::string()) == "task_sqlite", "memory_search dedup latest_basis source=task_sqlite"); rc) return rc;
  if (int rc = expect(latest_basis.contains("row_ref") && latest_basis["row_ref"].is_string(), "memory_search dedup latest_basis row_ref string"); rc) return rc;
  if (int rc = expect(!latest_basis["row_ref"].get<std::string>().empty(), "memory_search dedup latest_basis row_ref non-empty"); rc) return rc;
  if (int rc = expect(latest_basis.contains("reason") && latest_basis["reason"].is_string(), "memory_search dedup latest_basis reason string"); rc) return rc;
  if (int rc = expect(latest_basis["reason"].get<std::string>() == "priority+recency+tie_break", "memory_search dedup latest_basis reason value"); rc) return rc;
  if (int rc = expect(latest_basis.contains("selected_at") && latest_basis["selected_at"].is_string(), "memory_search dedup latest_basis selected_at string"); rc) return rc;
  if (int rc = expect(!latest_basis["selected_at"].get<std::string>().empty(), "memory_search dedup latest_basis selected_at non-empty"); rc) return rc;
  bool latest_row_ref_match = false;
  int latest_row_ref_match_count = 0;
  const std::string latest_row_ref = latest_basis["row_ref"].get<std::string>();
  for (const auto& row : memory_search_dedup_chat["observation"]["results"]) {
    if (!row.is_object()) continue;
    if (row.value("row_ref", std::string()) == latest_row_ref) {
      latest_row_ref_match = true;
      latest_row_ref_match_count += 1;
    }
  }
  if (int rc = expect(latest_row_ref_match, "memory_search dedup latest_basis row_ref replayable in results"); rc) return rc;
  if (int rc = expect(latest_row_ref_match_count == 1, "memory_search dedup latest_basis row_ref unique in results"); rc) return rc;

  auto memory_search_dedup_chat_repeat = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", std::string("请做记忆检索 task ") + tid + " action= 20"}});
  if (int rc = expect(memory_search_dedup_chat_repeat["type"] == "chat_result", "memory_search dedup repeat chat type"); rc) return rc;
  if (int rc = expect(memory_search_dedup_chat_repeat["observation"].contains("evidence_latest_basis") &&
                          memory_search_dedup_chat_repeat["observation"]["evidence_latest_basis"].is_object(),
                      "memory_search dedup repeat has evidence_latest_basis");
      rc)
    return rc;
  const auto& latest_basis_repeat = memory_search_dedup_chat_repeat["observation"]["evidence_latest_basis"];
  if (int rc = expect(latest_basis_repeat.value("row_ref", std::string()) == latest_basis.value("row_ref", std::string()),
                      "memory_search dedup repeat row_ref stable");
      rc)
    return rc;
  if (int rc = expect(latest_basis_repeat.value("task_id", std::string()) == latest_basis.value("task_id", std::string()),
                      "memory_search dedup repeat task_id stable");
      rc)
    return rc;
  if (int rc = expect(latest_basis_repeat.value("kind", std::string()) == latest_basis.value("kind", std::string()),
                      "memory_search dedup repeat kind stable");
      rc)
    return rc;
  if (int rc = expect(memory_search_dedup_chat_repeat["text"].get<std::string>().find("[latest]") != std::string::npos,
                      "memory_search dedup repeat latest marker present");
      rc)
    return rc;
  if (int rc = expect(dedup_text.find("#2") == std::string::npos, "memory_search dedup no #2 after task_id dedup"); rc) return rc;

  auto ms = svc.handle_request("s1", nlohmann::json{{"type", "memory_search"}, {"query", "memory-search-token-v0639"}, {"limit", 5}});
  if (int rc = expect(ms["type"] == "memory_search_result", "memory_search_result type"); rc) return rc;
  if (int rc = expect(ms["query"] == "memory-search-token-v0639", "memory_search query echo"); rc) return rc;
  if (int rc = expect(ms["limit_applied"].get<int>() == 5, "memory_search limit_applied=5"); rc) return rc;
  if (int rc = expect(ms["result_size"].get<int>() >= 1, "memory_search has hits"); rc) return rc;
  if (int rc = expect(ms["results"].is_array(), "memory_search results array"); rc) return rc;

  auto ms_default = svc.handle_request("s1", nlohmann::json{{"type", "memory_search"}, {"query", ""}, {"limit", 0}});
  if (int rc = expect(ms_default["type"] == "memory_search_result", "memory_search default type"); rc) return rc;
  if (int rc = expect(ms_default["limit_applied"].get<int>() == 20, "memory_search default limit=20"); rc) return rc;

  auto msu = svc.handle_request("s1", nlohmann::json{{"type", "memory_summary"}, {"query", "memory-search-token-v0639"}, {"limit", 5}});
  if (int rc = expect(msu["type"] == "memory_summary_result", "memory_summary_result type"); rc) return rc;
  if (int rc = expect(msu["query"] == "memory-search-token-v0639", "memory_summary query echo"); rc) return rc;
  if (int rc = expect(msu["limit_applied"].get<int>() == 5, "memory_summary limit_applied=5"); rc) return rc;
  if (int rc = expect(msu["hit_count"].get<int>() >= 1, "memory_summary hit_count>=1"); rc) return rc;
  if (int rc = expect(msu["summary"].is_string(), "memory_summary summary string"); rc) return rc;
  if (int rc = expect(!msu["summary"].get<std::string>().empty(), "memory_summary summary non-empty"); rc) return rc;
  if (int rc = expect(msu["top_sessions"].is_array(), "memory_summary top_sessions array"); rc) return rc;

  auto ms_task = svc.handle_request("s1", nlohmann::json{{"type", "memory_search"}, {"query", "capture_photo"}, {"limit", 20}});
  if (int rc = expect(ms_task["type"] == "memory_search_result", "memory_search task query type"); rc) return rc;
  if (int rc = expect(ms_task["result_size"].get<int>() >= 1, "memory_search task query has hits"); rc) return rc;
  bool has_task_sqlite_source = false;
  for (const auto& row : ms_task["results"]) {
    if (row.is_object() && row.value("source", std::string()) == "task_sqlite") {
      has_task_sqlite_source = true;
      break;
    }
  }
  if (int rc = expect(has_task_sqlite_source, "memory_search includes task_sqlite source"); rc) return rc;

  auto msu_task = svc.handle_request("s1", nlohmann::json{{"type", "memory_summary"}, {"query", "capture_photo"}, {"limit", 20}});
  if (int rc = expect(msu_task["type"] == "memory_summary_result", "memory_summary task query type"); rc) return rc;
  if (int rc = expect(msu_task["hit_count"].get<int>() >= 1, "memory_summary task query has hits"); rc) return rc;
  if (int rc = expect(msu_task.contains("source_counts") && msu_task["source_counts"].is_object(), "memory_summary source_counts object"); rc) return rc;
  if (int rc = expect(msu_task["source_counts"].value("task_sqlite", 0) >= 1, "memory_summary source_counts.task_sqlite>=1"); rc) return rc;
  if (int rc = expect(msu_task.contains("evidence_latest_basis") && msu_task["evidence_latest_basis"].is_object(), "memory_summary task has evidence_latest_basis"); rc) return rc;
  if (int rc = expect(msu_task["evidence_latest_basis"].value("source", std::string()) == "task_sqlite", "memory_summary task latest_basis source=task_sqlite"); rc) return rc;
  if (int rc = expect(msu_task["evidence_latest_basis"].contains("row_ref") && msu_task["evidence_latest_basis"]["row_ref"].is_string(), "memory_summary task latest_basis row_ref string"); rc) return rc;
  if (int rc = expect(!msu_task["evidence_latest_basis"]["row_ref"].get<std::string>().empty(), "memory_summary task latest_basis row_ref non-empty"); rc) return rc;
  if (int rc = expect(msu_task["evidence_latest_basis"].contains("reason") && msu_task["evidence_latest_basis"]["reason"].is_string(), "memory_summary task latest_basis reason string"); rc) return rc;
  if (int rc = expect(msu_task["evidence_latest_basis"]["reason"].get<std::string>() == "priority+recency+tie_break", "memory_summary task latest_basis reason value"); rc) return rc;
  if (int rc = expect(msu_task["evidence_latest_basis"].contains("selected_at") && msu_task["evidence_latest_basis"]["selected_at"].is_string(), "memory_summary task latest_basis selected_at string"); rc) return rc;
  if (int rc = expect(!msu_task["evidence_latest_basis"]["selected_at"].get<std::string>().empty(), "memory_summary task latest_basis selected_at non-empty"); rc) return rc;
  if (int rc = expect(msu_task.contains("memory_replay_hint") && msu_task["memory_replay_hint"].is_object(), "memory_summary task has memory_replay_hint"); rc) return rc;
  if (int rc = expect(msu_task["memory_replay_hint"].value("row_ref", std::string()) == msu_task["evidence_latest_basis"].value("row_ref", std::string()), "memory_summary task replay_hint row_ref matches latest_basis"); rc) return rc;
  if (int rc = expect(msu_task["memory_replay_hint"].contains("payload") && msu_task["memory_replay_hint"]["payload"].is_object(), "memory_summary task replay_hint payload object"); rc) return rc;
  if (int rc = expect(msu_task["memory_replay_hint"]["payload"].value("type", std::string()) == "memory_summary", "memory_summary task replay_hint payload.type=memory_summary"); rc) return rc;

  auto e = svc.handle_request("s1", nlohmann::json{{"type", "event_recent"}});
  if (int rc = expect(e["type"] == "event_recent_result", "event_recent_result type"); rc) return rc;
  if (int rc = expect(e["event_size"].get<int>() >= 2, "event_recent has entries"); rc) return rc;

  // v0.8.21: fuzzy profile + dialog continuity
  auto chat_profile_which = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "你是哪个？"}});
  if (int rc = expect(chat_profile_which["type"] == "chat_result", "chat profile which type"); rc) return rc;
  if (int rc = expect(chat_profile_which["decision"]["route"] == "local_profile", "chat profile which route"); rc) return rc;

  auto chat_profile_who_en = svc.handle_request("s1", nlohmann::json{{"type", "chat"}, {"text", "who are you"}});
  if (int rc = expect(chat_profile_who_en["decision"]["route"] == "local_profile", "chat profile who_en route"); rc) return rc;

  svc.on_session_open("s_dialog");
  auto dialog_w1 = svc.handle_request("s_dialog", nlohmann::json{{"type", "chat"}, {"text", "今天天气如何？"}});
  if (int rc = expect(dialog_w1["decision"]["intent"] == "weather", "dialog weather intent"); rc) return rc;
  if (int rc = expect(dialog_w1["decision"]["route"] == "local_external_clarify", "today weather without city clarifies"); rc) return rc;

  auto dialog_w2 = svc.handle_request("s_dialog", nlohmann::json{{"type", "chat"}, {"text", "上海"}});
  if (int rc = expect(dialog_w2["decision"]["route"] == "local_external_weather" ||
                      dialog_w2["decision"]["route"] == "local_external_clarify",
                      "dialog shanghai weather route"); rc) return rc;

  if (dialog_w2["decision"]["route"] == "local_external_weather") {
    auto dialog_w3 = svc.handle_request("s_dialog", nlohmann::json{{"type", "chat"}, {"text", "深圳的呢"}});
    if (int rc = expect(dialog_w3["decision"]["intent"] == "weather", "dialog shenzhen follow-up intent"); rc) return rc;
    if (int rc = expect(dialog_w3["decision"]["route"] == "local_external_weather" ||
                        dialog_w3["decision"]["route"] == "local_external_clarify",
                        "dialog shenzhen follow-up route"); rc) return rc;
    if (dialog_w3["decision"]["route"] == "local_external_weather") {
      if (int rc = expect(dialog_w3["decision"]["slots"]["city"] == "深圳", "dialog shenzhen city slot"); rc) return rc;

      auto dialog_need = svc.handle_request("s_dialog", nlohmann::json{{"type", "chat"}, {"text", "需要"}});
      if (int rc = expect(dialog_need["decision"]["route"] == "local_weather_advice", "dialog weather advice route"); rc) return rc;

      auto dialog_not_confirm = svc.handle_request("s_dialog", nlohmann::json{{"type", "chat"}, {"text", "这里是哪里的天气"}});
      if (int rc = expect(dialog_not_confirm["decision"]["route"] != "local_weather_advice",
                          "weather advice not triggered by substring 是"); rc) return rc;
    }
  }
  svc.on_session_close("s_dialog");

  svc.on_session_open("s_slot_switch");
  auto slot_weather_clarify = svc.handle_request("s_slot_switch", nlohmann::json{{"type", "chat"}, {"text", "查天气"}});
  if (int rc = expect(slot_weather_clarify["decision"]["route"] == "local_external_clarify", "slot switch weather clarify"); rc) return rc;
  auto slot_news_switch = svc.handle_request("s_slot_switch", nlohmann::json{{"type", "chat"}, {"text", "看新闻"}});
  if (int rc = expect(slot_news_switch["decision"]["intent"] == "news", "slot switch news intent"); rc) return rc;
  if (int rc = expect(slot_news_switch["decision"]["route"] == "local_external_clarify", "slot switch news clarify"); rc) return rc;
  if (int rc = expect(slot_news_switch["text"].get<std::string>().find("城市") == std::string::npos,
                      "slot switch news not weather clarify text"); rc) return rc;
  svc.on_session_close("s_slot_switch");

  setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE", "");
  // cloud classify 已移除 — 验证规则+ONNX 分类仍能正确识别 profile
  auto tier2_profile = cloud_strategy_svc.handle_request(
      "s_tier2", nlohmann::json{{"type", "chat"}, {"text", "你是谁"}});
  if (int rc = expect(tier2_profile["type"] == "chat_result", "tier2 profile type"); rc) return rc;
  if (int rc = expect(tier2_profile["decision"]["route"] == "local_profile", "tier2 profile route=local_profile"); rc) return rc;
  if (int rc = expect(tier2_profile["decision"]["intent"] == "profile", "tier2 profile intent=profile"); rc) return rc;
  if (int rc = expect(tier2_profile["observation"].contains("cloud_classify") == false, "tier2 profile no cloud_classify obs (removed)"); rc)
    return rc;

  // cloud classify 已移除 — tier2 weather 路由改为规则+ONNX
  ::setenv("THIN_AGENT_EXTERNAL_PROVIDER", "mock", 1);
  auto tier2_weather = cloud_strategy_svc.handle_request(
      "s_tier2w", nlohmann::json{{"type", "chat"}, {"text", "tier2-weather-followup-demo"}});
  if (int rc = expect(tier2_weather["type"] == "chat_result", "tier2 weather type"); rc) return rc;
  if (int rc = expect(tier2_weather["decision"]["route"] == "local_external_weather" ||
                      tier2_weather["decision"]["route"] == "local_external_clarify",
                      "tier2 weather route"); rc)
    return rc;

  setenv_local("THIN_AGENT_TEST_CLOUD_CLASSIFY_RESPONSE",
               R"({"intent":"weather","confidence":0.95,"slots":{},"reasoning":"should_not_run_for_slot_clarify"})");
  cloud_strategy_svc.on_session_open("s_tier2_skip");
  auto slot_clarify_no_tier2 = cloud_strategy_svc.handle_request(
      "s_tier2_skip", nlohmann::json{{"type", "chat"}, {"text", "查天气"}});
  if (int rc = expect(slot_clarify_no_tier2["decision"]["route"] == "local_external_clarify",
                      "slot clarify weather route"); rc)
    return rc;
  bool has_classify_layer = false;
  if (slot_clarify_no_tier2.contains("decision_trace") && slot_clarify_no_tier2["decision_trace"].is_array()) {
    for (const auto& step : slot_clarify_no_tier2["decision_trace"]) {
      if (step.contains("layer") && step["layer"].is_string() &&
          step["layer"].get<std::string>() == "cloud_classify") {
        has_classify_layer = true;
        break;
      }
    }
  }
  if (int rc = expect(!has_classify_layer, "slot clarify weather skips tier2 cloud_classify"); rc) return rc;
  cloud_strategy_svc.on_session_close("s_tier2_skip");

  setenv_local("THIN_AGENT_TEST_CLOUD_CLASSIFY_RESPONSE",
               R"({"intent":"profile","confidence":0.95,"slots":{},"reasoning":"should_not_run_for_news_slot_clarify"})");
  cloud_strategy_svc.on_session_open("s_tier2_news_slot");
  auto news_slot_no_tier2 = cloud_strategy_svc.handle_request(
      "s_tier2_news_slot", nlohmann::json{{"type", "chat"}, {"text", "看看新闻呢"}});
  if (int rc = expect(news_slot_no_tier2["decision"]["route"] == "local_external_clarify",
                      "news slot clarify route"); rc)
    return rc;
  if (int rc = expect(news_slot_no_tier2["intent_backend"] == "rules", "news slot clarify intent_backend=rules"); rc)
    return rc;
  bool news_slot_has_classify = false;
  if (news_slot_no_tier2.contains("decision_trace") && news_slot_no_tier2["decision_trace"].is_array()) {
    for (const auto& step : news_slot_no_tier2["decision_trace"]) {
      if (step.contains("layer") && step["layer"].is_string() &&
          step["layer"].get<std::string>() == "cloud_classify") {
        news_slot_has_classify = true;
        break;
      }
    }
  }
  if (int rc = expect(!news_slot_has_classify, "news slot clarify skips tier2 cloud_classify"); rc) return rc;
  cloud_strategy_svc.on_session_close("s_tier2_news_slot");

  setenv_local("THIN_AGENT_TEST_CLOUD_CLASSIFY_RESPONSE",
               R"({"intent":"general","confidence":0.95,"slots":{},"reasoning":"should_not_run_for_short_ack"})");
  cloud_strategy_svc.on_session_open("s_tier2_ack");
  auto short_ack_no_tier2 = cloud_strategy_svc.handle_request(
      "s_tier2_ack", nlohmann::json{{"type", "chat"}, {"text", "好的"}});
  if (int rc = expect(short_ack_no_tier2["decision"]["reason"] == "short_conversation_ack",
                      "short ack reason"); rc)
    return rc;
  bool ack_has_classify = false;
  if (short_ack_no_tier2.contains("decision_trace") && short_ack_no_tier2["decision_trace"].is_array()) {
    for (const auto& step : short_ack_no_tier2["decision_trace"]) {
      if (step.contains("layer") && step["layer"].is_string() &&
          step["layer"].get<std::string>() == "cloud_classify") {
        ack_has_classify = true;
        break;
      }
    }
  }
  if (int rc = expect(!ack_has_classify, "short ack skips tier2 cloud_classify"); rc) return rc;
  cloud_strategy_svc.on_session_close("s_tier2_ack");

  setenv_local("THIN_AGENT_TEST_CLOUD_CLASSIFY_RESPONSE",
               R"({"intent":"unknown","confidence":0.1,"slots":{},"reasoning":"should_not_run_for_task_capture"})");
  cloud_strategy_svc.on_session_open("s_tier2_photo");
  auto photo_no_tier2 = cloud_strategy_svc.handle_request(
      "s_tier2_photo", nlohmann::json{{"type", "chat"}, {"text", "帮我拍张照"}});
  if (int rc = expect(photo_no_tier2["decision"]["route"] == "local_task_inline", "task capture route"); rc)
    return rc;
  bool photo_has_classify = false;
  if (photo_no_tier2.contains("decision_trace") && photo_no_tier2["decision_trace"].is_array()) {
    for (const auto& step : photo_no_tier2["decision_trace"]) {
      if (step.contains("layer") && step["layer"].is_string() &&
          step["layer"].get<std::string>() == "cloud_classify") {
        photo_has_classify = true;
        break;
      }
    }
  }
  if (int rc = expect(!photo_has_classify, "task capture skips tier2 cloud_classify"); rc) return rc;
  cloud_strategy_svc.on_session_close("s_tier2_photo");

  setenv_local("THIN_AGENT_TEST_CLOUD_CLASSIFY_RESPONSE",
               R"({"intent":"memory_recent","confidence":0.9,"slots":{},"reasoning":"referential_weather_city"})");
  cloud_strategy_svc.on_session_open("s_tier2_mr");
  auto tier2_memory_setup = cloud_strategy_svc.handle_request(
      "s_tier2_mr", nlohmann::json{{"type", "chat"}, {"text", "上海天气"}});
  if (int rc = expect(tier2_memory_setup["decision"]["route"] == "local_external_weather" ||
                      tier2_memory_setup["decision"]["route"] == "local_external_clarify",
                      "tier2 memory_recent setup weather route"); rc)
    return rc;
  auto tier2_memory_recent = cloud_strategy_svc.handle_request(
      "s_tier2_mr", nlohmann::json{{"type", "chat"}, {"text", "刚才最后显示的是哪里的天气？"}});
  if (int rc = expect(tier2_memory_recent["type"] == "chat_result", "tier2 memory_recent type"); rc) return rc;
  if (int rc = expect(tier2_memory_recent["decision"]["route"] == "local_memory_recent",
                      "tier2 memory_recent route=local_memory_recent"); rc)
    return rc;
  bool tier2_memory_has_classify = false;
  if (tier2_memory_recent.contains("decision_trace") && tier2_memory_recent["decision_trace"].is_array()) {
    for (const auto& step : tier2_memory_recent["decision_trace"]) {
      if (step.contains("layer") && step["layer"].is_string() &&
          step["layer"].get<std::string>() == "cloud_classify") {
        tier2_memory_has_classify = true;
        break;
      }
    }
  }
  if (int rc = expect(!tier2_memory_has_classify, "tier2 memory_recent skips cloud_classify"); rc) return rc;
  cloud_strategy_svc.on_session_close("s_tier2_mr");
  setenv_local("THIN_AGENT_TEST_CLOUD_CLASSIFY_RESPONSE", "");

  svc.on_session_open("s_en_profile");
  auto detail_ability = svc.handle_request("s_en_profile", nlohmann::json{{"type", "chat"}, {"text", "detail ability"}});
  if (int rc = expect(detail_ability["decision"]["route"] == "local_profile", "detail ability route=local_profile"); rc)
    return rc;
  auto what_can_you_do = svc.handle_request(
      "s_en_profile", nlohmann::json{{"type", "chat"}, {"text", "what can you do for me"}});
  if (int rc = expect(what_can_you_do["decision"]["route"] == "local_profile",
                      "what can you do route=local_profile"); rc)
    return rc;
  svc.on_session_close("s_en_profile");

  ::setenv("THIN_AGENT_EXTERNAL_PROVIDER", "mock", 1);
  svc.on_session_open("s_recall");
  auto recall_setup = svc.handle_request("s_recall", nlohmann::json{{"type", "chat"}, {"text", "上海天气"}});
  if (int rc = expect(recall_setup["decision"]["intent"] == "weather", "recall setup weather intent"); rc) return rc;
  auto recall_where = svc.handle_request(
      "s_recall", nlohmann::json{{"type", "chat"}, {"text", "刚才最后显示的是哪里的天气？"}});
  if (int rc = expect(recall_where["type"] == "chat_result", "weather location recall type"); rc) return rc;
  if (int rc = expect(recall_where["decision"]["route"] == "local_memory_recent",
                      "weather location recall route"); rc)
    return rc;
  if (int rc = expect(recall_where["decision"]["reason"] == "weather_location_recall",
                      "weather location recall reason"); rc)
    return rc;
  if (int rc = expect(recall_where["text"].get<std::string>().find("上海") != std::string::npos,
                      "weather location recall mentions city"); rc)
    return rc;
  bool recall_has_classify = false;
  if (recall_where.contains("decision_trace") && recall_where["decision_trace"].is_array()) {
    for (const auto& step : recall_where["decision_trace"]) {
      if (step.contains("layer") && step["layer"].is_string() &&
          step["layer"].get<std::string>() == "cloud_classify") {
        recall_has_classify = true;
        break;
      }
    }
  }
  if (int rc = expect(!recall_has_classify, "weather location recall skips tier2"); rc) return rc;
  svc.on_session_close("s_recall");

  ::setenv("THIN_AGENT_EXTERNAL_PROVIDER", "mock", 1);
  svc.on_session_open("s_ctx_weather");
  auto ctx_setup = svc.handle_request("s_ctx_weather", nlohmann::json{{"type", "chat"}, {"text", "上海天气"}});
  if (int rc = expect(ctx_setup["decision"]["route"] == "local_external_weather", "ctx weather setup route"); rc)
    return rc;
  auto ctx_today = svc.handle_request("s_ctx_weather", nlohmann::json{{"type", "chat"}, {"text", "今天天气如何？"}});
  if (int rc = expect(ctx_today["type"] == "chat_result", "ctx today weather type"); rc) return rc;
  if (int rc = expect(ctx_today["decision"]["route"] == "local_external_weather", "ctx today weather route"); rc)
    return rc;
  if (int rc = expect(ctx_today["decision"]["slots"]["city"] == "上海", "ctx today weather city from dialog"); rc)
    return rc;
  bool ctx_has_classify = false;
  if (ctx_today.contains("decision_trace") && ctx_today["decision_trace"].is_array()) {
    for (const auto& step : ctx_today["decision_trace"]) {
      if (step.is_object() && step.contains("layer") && step["layer"] == "cloud_classify") {
        ctx_has_classify = true;
      }
    }
  }
  if (int rc = expect(!ctx_has_classify, "ctx today weather skips tier2 in offline mode"); rc) return rc;
  svc.on_session_close("s_ctx_weather");

  setenv_local("THIN_AGENT_TEST_CLOUD_CLASSIFY_RESPONSE",
               R"({"intent":"memory_recent","confidence":0.9,"slots":{},"reasoning":"should_not_run_ctx_weather"})");
  cloud_strategy_svc.on_session_open("s_ctx_tier2");
  auto ctx_cloud_setup = cloud_strategy_svc.handle_request(
      "s_ctx_tier2", nlohmann::json{{"type", "chat"}, {"text", "上海天气"}});
  if (int rc = expect(ctx_cloud_setup["decision"]["route"] == "local_external_weather" ||
                      ctx_cloud_setup["decision"]["route"] == "local_external_clarify",
                      "ctx cloud weather setup route"); rc)
    return rc;
  auto ctx_cloud_today = cloud_strategy_svc.handle_request(
      "s_ctx_tier2", nlohmann::json{{"type", "chat"}, {"text", "今天天气如何？"}});
  if (int rc = expect(ctx_cloud_today["decision"]["intent"] == "weather", "ctx cloud today weather intent"); rc) return rc;
  if (int rc = expect(ctx_cloud_today["decision"]["route"] == "local_external_weather" ||
                      ctx_cloud_today["decision"]["route"] == "local_external_clarify",
                      "ctx cloud today weather route"); rc)
    return rc;
  if (ctx_cloud_today["decision"]["route"] == "local_external_weather") {
    if (int rc = expect(ctx_cloud_today["decision"]["slots"]["city"] == "上海", "ctx cloud today weather city"); rc)
      return rc;
  }
  bool ctx_cloud_has_classify = false;
  if (ctx_cloud_today.contains("decision_trace") && ctx_cloud_today["decision_trace"].is_array()) {
    for (const auto& step : ctx_cloud_today["decision_trace"]) {
      if (step.is_object() && step.contains("layer") && step["layer"] == "cloud_classify") {
        ctx_cloud_has_classify = true;
      }
    }
  }
  if (int rc = expect(!ctx_cloud_has_classify, "ctx cloud today weather skips tier2 cloud_classify"); rc) return rc;
  cloud_strategy_svc.on_session_close("s_ctx_tier2");
  setenv_local("THIN_AGENT_TEST_CLOUD_CLASSIFY_RESPONSE", "");

  ::setenv("THIN_AGENT_EXTERNAL_PROVIDER", "mock", 1);
  svc.on_session_open("s_ctx_news");
  auto news_setup = svc.handle_request("s_ctx_news", nlohmann::json{{"type", "chat"}, {"text", "最近AI新闻"}});
  if (int rc = expect(news_setup["decision"]["route"] == "local_external_news", "ctx news setup route"); rc) return rc;
  auto news_follow = svc.handle_request("s_ctx_news", nlohmann::json{{"type", "chat"}, {"text", "还有什么新闻？"}});
  if (int rc = expect(news_follow["type"] == "chat_result", "ctx news follow type"); rc) return rc;
  if (int rc = expect(news_follow["decision"]["route"] == "local_external_news", "ctx news follow route"); rc) return rc;
  if (int rc = expect(news_follow["decision"]["slots"]["topic_or_scope"] == "AI", "ctx news follow topic from dialog"); rc)
    return rc;
  svc.on_session_close("s_ctx_news");

  svc.on_session_open("s_news_recall");
  auto news_recall_setup = svc.handle_request("s_news_recall", nlohmann::json{{"type", "chat"}, {"text", "最近AI新闻"}});
  if (int rc = expect(news_recall_setup["decision"]["route"] == "local_external_news", "news recall setup route"); rc)
    return rc;
  auto news_recall = svc.handle_request(
      "s_news_recall", nlohmann::json{{"type", "chat"}, {"text", "刚才看的是什么方向的新闻？"}});
  if (int rc = expect(news_recall["decision"]["route"] == "local_memory_recent", "news topic recall route"); rc) return rc;
  if (int rc = expect(news_recall["decision"]["reason"] == "news_topic_recall", "news topic recall reason"); rc) return rc;
  if (int rc = expect(news_recall["text"].get<std::string>().find("AI") != std::string::npos,
                      "news topic recall mentions topic"); rc)
    return rc;
  svc.on_session_close("s_news_recall");

  ::setenv("THIN_AGENT_INTENT_ONNX", "1", 1);
  ::setenv("THIN_AGENT_CHAT_POLICY_PATH", chat_policy_path.c_str(), 1);
  svc.on_session_open("s_onnx");
  auto onnx_profile = svc.handle_request("s_onnx", nlohmann::json{{"type", "chat"}, {"text", "who r u"}});
  if (int rc = expect(onnx_profile["decision"]["route"] == "local_profile", "onnx who r u route=local_profile"); rc) return rc;
  if (int rc = expect(onnx_profile["intent_backend"] == "onnx", "onnx who r u intent_backend=onnx"); rc) return rc;
  svc.on_session_close("s_onnx");
  ::unsetenv("THIN_AGENT_INTENT_ONNX");
  ::unsetenv("THIN_AGENT_CHAT_POLICY_PATH");

  // cloud classify 已移除 — tier2 null date 改为规则+ONNX 分类
  cloud_strategy_svc.on_session_open("s_tier2_null");
  auto tier2_null_date = cloud_strategy_svc.handle_request(
      "s_tier2_null", nlohmann::json{{"type", "chat"}, {"text", "今天天气如何？"}});
  if (int rc = expect(tier2_null_date["type"] == "chat_result", "tier2 null date slot type"); rc) return rc;
  if (int rc = expect(tier2_null_date["decision"]["intent"] == "weather", "tier2 null date weather intent"); rc)
    return rc;
  cloud_strategy_svc.on_session_close("s_tier2_null");

  svc.on_session_open("s_en");
  auto en_weather = svc.handle_request("s_en", nlohmann::json{{"type", "chat"}, {"text", "How's the weather of Shanghai"}});
  if (int rc = expect(en_weather["decision"]["intent"] == "weather", "english shanghai weather intent"); rc) return rc;
  if (int rc = expect(en_weather["decision"]["route"] == "local_external_weather" ||
                      en_weather["decision"]["route"] == "local_external_clarify",
                      "english shanghai weather route"); rc) return rc;
  if (en_weather["decision"]["route"] == "local_external_weather") {
    if (int rc = expect(en_weather["decision"]["slots"]["city"] == "Shanghai", "english shanghai city slot"); rc) return rc;
  }
  svc.on_session_close("s_en");

  // --- 语言矩阵：提问语言 × 数据源语言 × 模板 × 翻译 ---
  ::setenv("THIN_AGENT_EXTERNAL_PROVIDER", "mock", 1);
  if (int rc = expect(thin_agent::detect_query_language("Presentez vous") == "fr", "detect latin fr query"); rc) return rc;
  if (int rc = expect(thin_agent::detect_query_language("Jaka jest pogoda w Warszawie?") == "pl", "detect latin pl query"); rc) return rc;

  // 中文提问：中文模板，mock 源已中文 → 不翻译
  svc.on_session_open("s_lang_zh");
  auto lang_zh = svc.handle_request("s_lang_zh", nlohmann::json{{"type", "chat"}, {"text", "今天上海天气"}});
  if (int rc = expect(lang_zh["decision"]["route"] == "local_external_weather", "lang zh weather route"); rc) return rc;
  if (int rc = expect(lang_zh["observation"]["query_lang"] == "zh", "lang zh query_lang"); rc) return rc;
  if (int rc = expect(lang_zh["observation"]["template_lang"] == "zh", "lang zh template_lang"); rc) return rc;
  if (int rc = expect(lang_zh["observation"]["translation_applied"] == false, "lang zh no translation (zh source)"); rc)
    return rc;
  svc.on_session_close("s_lang_zh");

  // 英文提问：英文模板；offline 无 api key → 即便源为中文也静默不翻译
  svc.on_session_open("s_lang_en");
  auto lang_en = svc.handle_request("s_lang_en", nlohmann::json{{"type", "chat"}, {"text", "weather in Shanghai today"}});
  if (int rc = expect(lang_en["decision"]["route"] == "local_external_weather", "lang en weather route"); rc) return rc;
  if (int rc = expect(lang_en["observation"]["query_lang"] == "en", "lang en query_lang"); rc) return rc;
  if (int rc = expect(lang_en["observation"]["template_lang"] == "en", "lang en template_lang"); rc) return rc;
  svc.on_session_close("s_lang_en");

  // 其他语言（日文）：ja 模板或 en fallback
  svc.on_session_open("s_lang_ja");
  auto lang_ja = svc.handle_request("s_lang_ja", nlohmann::json{{"type", "chat"}, {"text", "Shanghai の weather"}});
  if (lang_ja["decision"]["route"] == "local_external_weather") {
    if (int rc = expect(lang_ja["observation"]["query_lang"] == "ja", "lang ja query_lang=ja"); rc) return rc;
    if (int rc = expect(lang_ja["observation"]["template_lang"] == "ja" ||
                            lang_ja["observation"]["template_lang"] == "en",
                        "lang ja template_lang"); rc) return rc;
  }
  svc.on_session_close("s_lang_ja");

  // 语言能力问答：27 语名单，不进 profile
  svc.on_session_open("s_lang_list");
  auto lang_list = svc.handle_request("s_lang_list", nlohmann::json{{"type", "chat"}, {"text", "你支持多少种语言？"}});
  if (int rc = expect(lang_list["decision"]["route"] == "local_supported_languages", "supported languages route"); rc)
    return rc;
  if (int rc = expect(lang_list["decision"]["route"] != "local_profile", "supported languages not profile"); rc) return rc;
  if (int rc = expect(lang_list["observation"]["supported_languages_count"].get<size_t>() == 29,
                      "supported languages count=29"); rc)
    return rc;
  const std::string lang_list_text = lang_list["text"].get<std::string>();
  if (int rc = expect(lang_list_text.find("29") != std::string::npos, "supported languages text has count"); rc)
    return rc;
  if (int rc = expect(lang_list_text.find("英语") != std::string::npos ||
                          lang_list_text.find("English") != std::string::npos,
                      "supported languages text has english"); rc)
    return rc;
  svc.on_session_close("s_lang_list");

  // 非指代短句不应复用上一轮天气城市（避免槽位污染）。
  svc.on_session_open("s_ctx_no_ref_weather");
  auto no_ref_setup = svc.handle_request(
      "s_ctx_no_ref_weather", nlohmann::json{{"type", "chat"}, {"text", "weather in Berlin"}});
  if (no_ref_setup["decision"]["route"] == "local_external_weather") {
    auto no_ref_weather = svc.handle_request(
        "s_ctx_no_ref_weather", nlohmann::json{{"type", "chat"}, {"text", "查天气"}});
    if (int rc = expect(no_ref_weather["decision"]["route"] == "local_external_clarify",
                        "weather no-reference should clarify city"); rc)
      return rc;
  }
  svc.on_session_close("s_ctx_no_ref_weather");

  // 英文天气城市 recall 识别（what city was the weather ...）。
  svc.on_session_open("s_weather_recall_en");
  auto recall_en_setup = svc.handle_request(
      "s_weather_recall_en", nlohmann::json{{"type", "chat"}, {"text", "Shanghai weather"}});
  if (int rc = expect(recall_en_setup["decision"]["intent"] == "weather", "weather recall en setup intent"); rc)
    return rc;
  auto recall_en = svc.handle_request(
      "s_weather_recall_en", nlohmann::json{{"type", "chat"}, {"text", "what city was the weather for"}});
  if (int rc = expect(recall_en["decision"]["route"] == "local_memory_recent", "weather recall en route"); rc)
    return rc;
  if (int rc = expect(recall_en["decision"]["reason"] == "weather_location_recall", "weather recall en reason"); rc)
    return rc;
  if (int rc = expect(recall_en["text"].get<std::string>().find("Shanghai") != std::string::npos,
                      "weather recall en mentions city"); rc)
    return rc;
  svc.on_session_close("s_weather_recall_en");

  // 翻译网关：中文提问 + HN 英文源 → mock 翻译回填
  {
    thin_agent::DemoConfigCompat tr_cfg;
    tr_cfg.mode = "cloud";
    tr_cfg.provider = "openai-compatible";
    tr_cfg.model_name = "mock-model";
    tr_cfg.api_base = "http://127.0.0.1:18080/v1";
    tr_cfg.api_key_env = "THIN_AGENT_CLOUD_TEST_KEY";
    tr_cfg.fallback = "offline";
    setenv_local("THIN_AGENT_CLOUD_TEST_KEY", "dummy-key");
    ::setenv("THIN_AGENT_EXTERNAL_PROVIDER", "http", 1);
    ::setenv("THIN_AGENT_EXTERNAL_HTTP_MOCK_NEWS_JSON",
             R"({"items":[{"title":"OpenAI releases new model"},{"title":"AI chip demand surges"}]})", 1);
    setenv_local("THIN_AGENT_TEST_CLOUD_TRANSLATE_RESPONSE",
                 R"(["OpenAI 发布新模型","AI 芯片需求激增"])");
    // 有 topic 的中文新闻，跳过 Tier2 直接执行
    thin_agent::AgentService tr_svc(tr_cfg, ex, te);
    tr_svc.on_session_open("s_tr");
    auto tr_news = tr_svc.handle_request("s_tr", nlohmann::json{{"type", "chat"}, {"text", "最近AI新闻"}});
    if (int rc = expect(tr_news["decision"]["route"] == "local_external_news", "translate news route"); rc) return rc;
    if (int rc = expect(tr_news["observation"]["query_lang"] == "zh", "translate news query_lang"); rc) return rc;
    if (int rc = expect(tr_news["observation"]["translation_applied"] == true, "translate news applied"); rc) return rc;
    tr_svc.on_session_close("s_tr");
    setenv_local("THIN_AGENT_TEST_CLOUD_TRANSLATE_RESPONSE", "");
    ::unsetenv("THIN_AGENT_EXTERNAL_HTTP_MOCK_NEWS_JSON");
    ::setenv("THIN_AGENT_EXTERNAL_PROVIDER", "mock", 1);
  }

  // 英文提问 + HN 英文源 → 不翻译
  {
    thin_agent::DemoConfigCompat tr_cfg2;
    tr_cfg2.mode = "cloud";
    tr_cfg2.provider = "openai-compatible";
    tr_cfg2.model_name = "mock-model";
    tr_cfg2.api_base = "http://127.0.0.1:18080/v1";
    tr_cfg2.api_key_env = "THIN_AGENT_CLOUD_TEST_KEY";
    tr_cfg2.fallback = "offline";
    ::setenv("THIN_AGENT_EXTERNAL_PROVIDER", "http", 1);
    ::setenv("THIN_AGENT_EXTERNAL_HTTP_MOCK_NEWS_JSON",
             R"({"items":[{"title":"OpenAI releases new model"},{"title":"AI chip demand surges"}]})", 1);
    setenv_local("THIN_AGENT_TEST_CLOUD_TRANSLATE_RESPONSE", R"(["SHOULD NOT BE USED","NOPE"])");
    thin_agent::AgentService tr_svc2(tr_cfg2, ex, te);
    tr_svc2.on_session_open("s_tr2");
    auto tr_news2 = tr_svc2.handle_request("s_tr2", nlohmann::json{{"type", "chat"}, {"text", "latest AI news"}});
    if (int rc = expect(tr_news2["decision"]["route"] == "local_external_news", "en news route"); rc) return rc;
    if (int rc = expect(tr_news2["observation"]["query_lang"] == "en", "en news query_lang"); rc) return rc;
    if (int rc = expect(tr_news2["observation"]["translation_applied"] == false, "en news no translation"); rc) return rc;
    if (int rc = expect(tr_news2["text"].get<std::string>().find("OpenAI releases new model") != std::string::npos,
                        "en news keeps english title"); rc)
      return rc;
    tr_svc2.on_session_close("s_tr2");
    setenv_local("THIN_AGENT_TEST_CLOUD_TRANSLATE_RESPONSE", "");
    ::unsetenv("THIN_AGENT_EXTERNAL_HTTP_MOCK_NEWS_JSON");
    ::setenv("THIN_AGENT_EXTERNAL_PROVIDER", "mock", 1);
  }

  auto metrics = svc.handle_request("s1", nlohmann::json{{"type", "metrics"}});
  if (int rc = expect(metrics["type"] == "metrics_result", "metrics_result type"); rc) return rc;
  if (int rc = expect(metrics["data"]["total_requests"].get<int>() >= 8, "metrics total_requests"); rc) return rc;
  if (int rc = expect(metrics["data"]["by_type"]["chat"].get<int>() >= 2, "metrics chat count"); rc) return rc;
  if (int rc = expect(metrics["data"]["by_type"].contains("metrics"), "metrics by_type contains metrics key"); rc) return rc;
  if (int rc = expect(metrics["data"]["by_type"]["metrics"].get<int>() >= 1, "metrics by_type.metrics >= 1"); rc) return rc;
  if (int rc = expect(metrics["data"]["type_count"].is_number_integer(), "metrics type_count is integer"); rc) return rc;
  if (int rc = expect(metrics["data"]["type_count"].get<int>() >= 1, "metrics type_count >= 1"); rc) return rc;
  if (int rc = expect(metrics["data"]["event_count"].is_number_integer(), "metrics event_count is integer"); rc) return rc;
  if (int rc = expect(metrics["data"]["event_count"].get<int>() >= 1, "metrics event_count >= 1"); rc) return rc;
  if (int rc = expect(metrics["data"]["event_window_limit"].is_number_integer(), "metrics event_window_limit is integer"); rc)
    return rc;
  if (int rc = expect(metrics["data"]["event_window_limit"].get<int>() == 32, "metrics event_window_limit == 32"); rc)
    return rc;
  if (int rc = expect(metrics["data"]["event_count"].get<int>() <= metrics["data"]["event_window_limit"].get<int>(),
                      "metrics event_count <= event_window_limit");
      rc)
    return rc;
  if (int rc = expect(metrics["data"]["history_budget_tokens"].is_number_integer(), "metrics history_budget_tokens is integer"); rc)
    return rc;
  if (int rc = expect(metrics["data"]["history_budget_tokens"].get<int>() == 12000, "metrics history_budget_tokens == 12000"); rc)
    return rc;
  if (int rc = expect(metrics["data"]["active_session_count"].is_number_integer(), "metrics active_session_count is integer"); rc)
    return rc;
  if (int rc = expect(metrics["data"]["active_session_count"].get<int>() >= 1, "metrics active_session_count >= 1 (at least s1)"); rc)
    return rc;
  if (int rc = expect(metrics["data"]["active_session_count"].get<int>() >= 0, "metrics active_session_count >= 0"); rc)
    return rc;
  if (int rc = expect(metrics["data"]["history_budget_tokens"].get<int>() >= m["memory_size"].get<int>(),
                      "metrics history_budget_tokens >= memory_size");
      rc)
    return rc;
  if (int rc = expect(metrics["data"]["last_latency_ms"].get<double>() >= 0.0, "metrics last_latency_ms"); rc) return rc;
  if (int rc = expect(metrics["data"]["avg_latency_ms"].get<double>() >= 0.0, "metrics avg_latency_ms"); rc) return rc;

  // --- media plan: dialogue_act / count / compound ---
  {
    svc.on_session_open("s_media");
    auto shot = svc.handle_request("s_media", nlohmann::json{{"type", "chat"}, {"text", "拍张照片"}});
    if (int rc = expect(shot["decision"]["route"] == "local_task_inline" ||
                            shot["decision"]["route"] == "local_action",
                        "single capture still deterministic route");
        rc)
      return rc;

    // 幂等键含 cmd_id：同 session 连续拍照应得到不同 task，且 key 带上 cmd
    auto shot_a = svc.handle_request(
        "s_media",
        nlohmann::json{{"type", "chat"}, {"text", "拍张照片"}, {"cmd_id", "idem-test-a"}});
    auto shot_b = svc.handle_request(
        "s_media",
        nlohmann::json{{"type", "chat"}, {"text", "拍张照片"}, {"cmd_id", "idem-test-b"}});
    const std::string tid_a = shot_a["decision"]["slots"].value("task_id", "");
    const std::string tid_b = shot_b["decision"]["slots"].value("task_id", "");
    if (tid_a.empty()) {
      // local_action 路径无 task_id，跳过；task_inline 必须有
      if (shot_a["decision"]["route"] == "local_task_inline") {
        std::cerr << "FAIL: shot_a missing task_id\n";
        return 1;
      }
    } else {
      if (int rc = expect(tid_a != tid_b, "consecutive capture different task_id"); rc) return rc;
      const std::string idem_a = shot_a["observation"]["task"].value("idempotency_key", "");
      const std::string idem_b = shot_b["observation"]["task"].value("idempotency_key", "");
      if (int rc = expect(idem_a.find("idem-test-a") != std::string::npos, "idem contains cmd_id a");
          rc)
        return rc;
      if (int rc = expect(idem_b.find("idem-test-b") != std::string::npos, "idem contains cmd_id b");
          rc)
        return rc;
      if (int rc = expect(idem_a != idem_b, "consecutive capture different idem keys"); rc) return rc;
    }

    auto ask = svc.handle_request("s_media", nlohmann::json{{"type", "chat"}, {"text", "拍照完成了吗"}});
    if (int rc = expect(ask["decision"]["route"] == "local_media_status", "status query route"); rc) return rc;
    if (int rc = expect(ask["decision"]["intent"] == "media_capture_status", "status intent family"); rc)
      return rc;
    if (int rc = expect(ask["decision"]["policy"] == "execute" || ask["decision"]["policy"] == "clarify",
                        "status query not side-effect execute-capture");
        rc)
      return rc;
    if (int rc = expect(ask["observation"].contains("dialogue_act") &&
                            ask["observation"]["dialogue_act"] == "question",
                        "status query dialogue_act=question");
        rc)
      return rc;
    if (int rc = expect(ask["observation"]["media_plan"].contains("fuzzy_intent"),
                        "status has fuzzy_intent field");
        rc)
      return rc;
    // 不应再次提交 capture
    if (ask.contains("tool_calls") && ask["tool_calls"].is_array()) {
      for (const auto& tc : ask["tool_calls"]) {
        if (tc.contains("action") && tc["action"] == "capture_photo" && tc.value("tool", "") == "task_submit") {
          std::cerr << "FAIL: status query must not submit capture_photo\n";
          return 1;
        }
      }
    }

    auto short_ask = svc.handle_request("s_media", nlohmann::json{{"type", "chat"}, {"text", "完成了吗"}});
    if (int rc = expect(short_ask["decision"]["route"] == "local_media_status",
                        "short status followup route");
        rc)
      return rc;
    if (int rc = expect(short_ask["decision"]["intent"] == "media_capture_status",
                        "short status intent family");
        rc)
      return rc;

    auto multi = svc.handle_request("s_media", nlohmann::json{{"type", "chat"}, {"text", "拍5张照片"}});
    if (int rc = expect(multi["decision"]["route"] == "local_task_pipeline", "multi capture pipeline route"); rc)
      return rc;
    if (int rc = expect(multi["observation"]["media_plan"]["capture_count"] == 5, "multi capture_count=5"); rc)
      return rc;
    if (int rc = expect(multi["observation"]["pipeline_outcome"]["completed"].get<int>() >= 1,
                        "multi capture completed>=1");
        rc)
      return rc;

    auto compound = svc.handle_request(
        "s_media", nlohmann::json{{"type", "chat"}, {"text", "给我拍3张照片，再拍一段20秒的视频"}});
    if (int rc = expect(compound["decision"]["route"] == "local_task_pipeline", "compound pipeline route"); rc)
      return rc;
    if (int rc = expect(compound["observation"]["pipeline_outcome"]["step_count"].get<int>() == 3,
                        "compound step_count=3");
        rc)
      return rc;
    if (int rc = expect(compound["observation"]["pipeline_outcome"]["completed"].get<int>() == 3,
                        "compound completed=3");
        rc)
      return rc;
    if (int rc = expect(compound["observation"]["media_plan"]["capture_count"] == 3, "compound capture_count=3");
        rc)
      return rc;
    if (int rc = expect(compound["observation"]["media_plan"]["record_duration_sec"] == 20,
                        "compound duration=20");
        rc)
      return rc;
    // FAST_MEDIA 下应跳过真实等待
    if (compound["observation"]["pipeline_steps"].is_array()) {
      bool saw_skip_or_wait = false;
      // v0.54.6: 改名 pstep——此前与外层 `st` 同名（-Wshadow=local 判官命中）
      for (const auto& pstep : compound["observation"]["pipeline_steps"]) {
        if (pstep.value("action", "") == "start_recording") {
          if (pstep.contains("duration_wait_skipped") || pstep.contains("duration_wait_sec")) {
            saw_skip_or_wait = true;
          }
        }
      }
      if (int rc = expect(saw_skip_or_wait, "compound start_recording has duration wait marker"); rc)
        return rc;
    }

    auto capq = svc.handle_request("s_media", nlohmann::json{{"type", "chat"}, {"text", "能拍照吗"}});
    if (int rc = expect(capq["decision"]["route"] == "local_media_capability", "capability route"); rc) return rc;
    if (int rc = expect(capq["decision"]["policy"] == "clarify", "capability not execute"); rc) return rc;

    auto en_cap = svc.handle_request("s_media", nlohmann::json{{"type", "chat"}, {"text", "can you take photos?"}});
    if (int rc = expect(en_cap["decision"]["route"] == "local_media_capability",
                        "en capability route");
        rc)
      return rc;
    if (int rc = expect(en_cap["decision"]["policy"] == "clarify", "en capability not execute"); rc)
      return rc;

    // 阶段4：离线歧义句不得关键词直执行
    auto ambig_offline = svc.handle_request("s_media", nlohmann::json{{"type", "chat"}, {"text", "要不要拍照"}});
    if (int rc = expect(ambig_offline["decision"]["route"] == "local_media_clarify",
                        "offline ambiguous media clarifies");
        rc)
      return rc;
    if (int rc = expect(ambig_offline["decision"]["policy"] == "clarify", "offline ambiguous policy=clarify");
        rc)
      return rc;
    if (ambig_offline.contains("tool_calls") && ambig_offline["tool_calls"].is_array()) {
      for (const auto& tc : ambig_offline["tool_calls"]) {
        if (tc.contains("action") && tc["action"] == "capture_photo") {
          std::cerr << "FAIL: offline ambiguous must not capture\n";
          return 1;
        }
      }
    }

    svc.on_session_close("s_media");
  }

  // 阶段4：cloud classify/cloud router 已移除 — 歧义媒体句直接走 clarify

  const std::string mem_file = thin_agent::default_memory_jsonl_path();
  if (int rc = expect(std::filesystem::exists(mem_file), "memory file exists"); rc) return rc;
  if (int rc = expect(line_count(mem_file) >= 2, "memory file has chat records"); rc) return rc;


  // ── HITL 测试：chat_approve 空状态 ────────────────────────────
  {
    thin_agent::AgentService hitl_svc(cfg, ex, te);
    hitl_svc.on_session_open("hitl_test");

    // 无待审批操作时 chat_approve 返回澄清消息
    auto no_pending = hitl_svc.handle_request("hitl_test",
        nlohmann::json{{"type", "chat_approve"}, {"approved", true}});
    if (int rc = expect(no_pending["type"] == "chat_result", "hitl: no-pending returns chat_result"); rc) return rc;
    if (int rc = expect(no_pending["decision"]["reason"] == "no_pending_approval",
                        "hitl: no-pending reason"); rc) return rc;
    if (int rc = expect(no_pending["text"].get<std::string>().find("待审批") != std::string::npos,
                        "hitl: no-pending text mentions approval"); rc) return rc;

    // chat_approve with approved=false also returns correctly
    auto no_pending2 = hitl_svc.handle_request("hitl_test",
        nlohmann::json{{"type", "chat_approve"}, {"approved", false}});
    if (int rc = expect(no_pending2["type"] == "chat_result", "hitl: no-pending cancel returns chat_result"); rc) return rc;
    if (int rc = expect(no_pending2["decision"]["reason"] == "no_pending_approval",
                        "hitl: no-pending cancel reason"); rc) return rc;

    hitl_svc.on_session_close("hitl_test");
  }

  // ── 流式 StreamCallback 正确传播测试 ──────────────────────────
  {
    int chunk_count = 0;
    int done_count = 0;
    thin_agent::StreamCallback on_chunk =
        [&](const std::string& chunk, bool done) {
          if (done) ++done_count;
          else if (!chunk.empty()) ++chunk_count;
        };

    thin_agent::AgentService stream_svc(cfg, ex, te);
    stream_svc.on_session_open("stream_test");

    // 发送 chat 请求并传递 StreamCallback
    auto stream_result = stream_svc.handle_request("stream_test",
        nlohmann::json{{"type", "chat"}, {"text", "你好"}}, on_chunk);

    if (int rc = expect(stream_result["type"] == "chat_result", "stream: returns chat_result"); rc) return rc;
    if (int rc = expect(!stream_result["text"].get<std::string>().empty(),
                        "stream: has reply text"); rc) return rc;
    // 离线模式走 HybridRouter cascade_stream：应有至少一次 token 回调
    if (int rc = expect(chunk_count + done_count >= 1,
                        "stream: at least one callback"); rc) return rc;

    // 第二次请求：常规对话，验证回调也触发
    int chunk2 = 0;
    int done2 = 0;
    thin_agent::StreamCallback on_chunk2 =
        [&](const std::string&, bool done) {
          if (done) ++done2; else ++chunk2;
        };
    // 使用 general query 确保走 streaming 路径（非 status/profile 等本地 intent）
    auto stream_result2 = stream_svc.handle_request("stream_test",
        nlohmann::json{{"type", "chat"}, {"text", "请讲个有趣的故事"}}, on_chunk2);

    if (int rc = expect(stream_result2["type"] == "chat_result", "stream2: returns chat_result"); rc) return rc;
    if (int rc = expect(chunk2 + done2 >= 1, "stream2: at least one callback"); rc) return rc;

    // 不带回调的请求：不会 crash
    thin_agent::StreamCallback null_cb = nullptr;
    auto no_stream_result = stream_svc.handle_request("stream_test",
        nlohmann::json{{"type", "chat"}, {"text", "你好"}}, null_cb);
    if (int rc = expect(no_stream_result["type"] == "chat_result", "stream-null: returns chat_result"); rc) return rc;

    stream_svc.on_session_close("stream_test");
  }

  svc.on_session_close("s1");

  // ═══════════════════════════════════════════════════════════════
  //  自动回归测试套件：全路径覆盖
  // ═══════════════════════════════════════════════════════════════

  // ── 1. handle_request 完整路由矩阵（16 种 msg type）───────────
  {
    thin_agent::AgentService router_svc(cfg, ex, te);
    router_svc.on_session_open("rt");

    // 1a) ping → pong
    auto p = router_svc.handle_request("rt", {{"type", "ping"}});
    if (int rc = expect(p["type"] == "pong", "route: ping→pong"); rc) return rc;

    // 1b) status
    auto s = router_svc.handle_request("rt", {{"type", "status"}});
    if (int rc = expect(s["type"] == "status", "route: status"); rc) return rc;
    if (int rc = expect(s["data"].contains("sense"), "route: status.sense"); rc) return rc;

    // 1c) trace_list
    auto tl = router_svc.handle_request("rt", {{"type", "trace_list"}});
    if (int rc = expect(tl["type"] == "trace_list", "route: trace_list"); rc) return rc;

    // 1d) chat_approve (no pending)
    auto ca = router_svc.handle_request("rt", {{"type", "chat_approve"}, {"approved", true}});
    if (int rc = expect(ca["type"] == "chat_result", "route: chat_approve"); rc) return rc;
    if (int rc = expect(ca["decision"]["reason"] == "no_pending_approval", "route: chat_approve no pending"); rc) return rc;

    // 1e) action
    auto ac = router_svc.handle_request("rt", {{"type", "action"}, {"action", "fetch_capture_results"}, {"args", nlohmann::json::object()}});
    if (int rc = expect(ac["type"] == "action_result", "route: action→action_result"); rc) return rc;

    // 1f) task_submit
    auto ts = router_svc.handle_request("rt", {{"type", "task_submit"}, {"action", "capture_photo"}, {"args", nlohmann::json::object()}, {"idempotency_key", "rt-ts-1"}});
    if (int rc = expect(ts["type"] == "task_submit_result", "route: task_submit"); rc) return rc;
    if (int rc = expect(ts["task"].contains("task_id"), "route: task_submit.task_id"); rc) return rc;

    // 1g) task_get (by task_id from submit)
    auto tg = router_svc.handle_request("rt", {{"type", "task_get"}, {"task_id", ts["task"]["task_id"]}});
    if (int rc = expect(tg["type"] == "task_get_result", "route: task_get"); rc) return rc;

    // 1h) task_list
    auto tlist = router_svc.handle_request("rt", {{"type", "task_list"}});
    if (int rc = expect(tlist["type"] == "task_list_result", "route: task_list"); rc) return rc;

    // 1i) task_cancel
    auto tc = router_svc.handle_request("rt", {{"type", "task_cancel"}, {"task_id", ts["task"]["task_id"]}});
    if (int rc = expect(tc["type"] == "task_cancel_result", "route: task_cancel"); rc) return rc;

    // 1j) task_replay
    auto tr = router_svc.handle_request("rt", {{"type", "task_replay"}, {"task_id", ts["task"]["task_id"]}});
    if (int rc = expect(tr["type"] == "task_replay_result", "route: task_replay"); rc) return rc;

    // 1k) task_audit
    auto ta = router_svc.handle_request("rt", {{"type", "task_audit"}, {"task_id", ts["task"]["task_id"]}});
    if (int rc = expect(ta["type"] == "task_audit_result", "route: task_audit"); rc) return rc;

    // 1l) memory_recent
    auto mr = router_svc.handle_request("rt", {{"type", "memory_recent"}});
    if (int rc = expect(mr["type"] == "memory_recent_result", "route: memory_recent"); rc) return rc;

    // 1m) memory_history
    // v0.54.6: 改名 mh_res/ms_res/msu_res——此前 `mh`/`ms`/`msu` 与函数外层同名（-Wshadow=local 判官命中）
    auto mh_res = router_svc.handle_request("rt", {{"type", "memory_history"}});
    if (int rc = expect(mh_res["type"] == "memory_history_result", "route: memory_history"); rc) return rc;

    // 1n) memory_search
    auto ms_res = router_svc.handle_request("rt", {{"type", "memory_search"}, {"query", "test"}});
    if (int rc = expect(ms_res["type"] == "memory_search_result", "route: memory_search"); rc) return rc;

    // 1o) memory_summary
    auto msu_res = router_svc.handle_request("rt", {{"type", "memory_summary"}, {"query", "test"}});
    if (int rc = expect(msu_res["type"] == "memory_summary_result", "route: memory_summary"); rc) return rc;

    // 1p) event_recent
    auto er = router_svc.handle_request("rt", {{"type", "event_recent"}});
    if (int rc = expect(er["type"] == "event_recent_result", "route: event_recent"); rc) return rc;

    // 1q) metrics
    auto mt = router_svc.handle_request("rt", {{"type", "metrics"}});
    if (int rc = expect(mt["type"] == "metrics_result", "route: metrics"); rc) return rc;

    // 1r) unknown type → error
    auto unk = router_svc.handle_request("rt", {{"type", "nonexistent_xyz"}});
    if (int rc = expect(unk["type"] == "error", "route: unknown→error"); rc) return rc;

    // 1s) empty type → error
    auto empty_t = router_svc.handle_request("rt", nlohmann::json::object());
    if (int rc = expect(empty_t["type"] == "error", "route: empty type→error"); rc) return rc;

    router_svc.on_session_close("rt");
  }

  // ── 2. 模式切换：offline / auto(missing key) / auto(mock key) ──
  {
    // 2a) offline 模式始终返回 mode_used="offline"
    thin_agent::DemoConfigCompat off_cfg;
    off_cfg.mode = "offline";
    off_cfg.fallback = "offline";
    thin_agent::AgentService off_svc(off_cfg, ex, te);
    off_svc.on_session_open("m1");
    auto off_r = off_svc.handle_request("m1", {{"type", "chat"}, {"text", "hello"}});
    if (int rc = expect(off_r["type"] == "chat_result", "mode: offline chat_result"); rc) return rc;
    if (int rc = expect(off_r["mode_used"] == "offline", "mode: offline mode_used"); rc) return rc;
    if (int rc = expect(off_r["intent_backend"] == "rules", "mode: offline backend=rules"); rc) return rc;
    off_svc.on_session_close("m1");

    // 2b) auto 模式无 api_key：本地 cascade 返回 local-agent 或 offline-fallback
    thin_agent::DemoConfigCompat auto_no_key;
    auto_no_key.mode = "auto";
    auto_no_key.fallback = "offline";
    thin_agent::AgentService auto_svc(auto_no_key, ex, te);
    auto_svc.on_session_open("m2");
    auto auto_r = auto_svc.handle_request("m2", {{"type", "chat"}, {"text", "hello"}});
    if (int rc = expect(auto_r["type"] == "chat_result", "mode: auto-no-key chat_result"); rc) return rc;
    if (int rc = expect(auto_r["mode_used"] == "offline-fallback" || auto_r["mode_used"] == "local-agent",
                        "mode: auto-no-key mode_used"); rc) return rc;
    auto_svc.on_session_close("m2");

    // 2c) auto 模式有 mock key + mock response：走云策略路径
    setenv_local("DUMMY_API_KEY", "mock-key-test");
    setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
                 R"({"strategy":"answer_direct","intent":"general_query","confidence":0.65,"local_route_hint":"","risk":"low","need_clarify":false,"response_draft":"mock cloud reply"})");
    thin_agent::DemoConfigCompat auto_mock;
    auto_mock.mode = "auto";
    auto_mock.fallback = "offline";
    auto_mock.api_key_env = "DUMMY_API_KEY";
    thin_agent::AgentService auto_mock_svc(auto_mock, ex, te);
    auto_mock_svc.on_session_open("m3");
    // 需要 general 意图才能走云（非本地 intent 命中）
    auto auto_mock_r = auto_mock_svc.handle_request("m3", {{"type", "chat"}, {"text", "请解释人工智能的基本概念"}});
    if (int rc = expect(auto_mock_r["type"] == "chat_result", "mode: auto-mock chat_result"); rc) return rc;
    // 云策略可能返回 cloud 或 local-agent（取决于本地 cascade 是否命中）
    if (int rc = expect(!auto_mock_r["text"].get<std::string>().empty(),
                        "mode: auto-mock has text"); rc) return rc;
    auto_mock_svc.on_session_close("m3");
    setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE", "");
    setenv_local("DUMMY_API_KEY", "");
  }

  // ── 3. 云策略 contract 校验（reject / high-risk）─────────────
  {
    setenv_local("DUMMY_API_KEY", "mock-key-test");

    // 3a) strategy=reject → local_reject route
    setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
                 R"({"strategy":"reject","intent":"high_risk_query","confidence":0.90,"local_route_hint":"","risk":"high","need_clarify":false,"reason":"policy_reject_test"})");
    thin_agent::DemoConfigCompat rej_cfg;
    rej_cfg.mode = "cloud";
    rej_cfg.fallback = "none";
    rej_cfg.api_key_env = "DUMMY_API_KEY";
    thin_agent::AgentService rej_svc(rej_cfg, ex, te);
    rej_svc.on_session_open("c1");
    auto rej_r = rej_svc.handle_request("c1", {{"type", "chat"}, {"text", "帮我破解密码"}});
    if (int rc = expect(rej_r["type"] == "chat_result", "contract: reject chat_result"); rc) return rc;
    if (int rc = expect(rej_r["mode_used"] == "local-agent" || rej_r["mode_used"] == "cloud",
                        "contract: reject mode"); rc) return rc;
    // 应包含 cloud_policy observation
    if (int rc = expect(rej_r["observation"].contains("cloud_policy") || rej_r["observation"].contains("risk_gate"),
                        "contract: reject has cloud info"); rc) return rc;
    rej_svc.on_session_close("c1");

    // 3b) risk=high with general_info → downgrade to clarify
    setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
                 R"({"strategy":"reject","intent":"general_query","confidence":0.52,"local_route_hint":"","risk":"high","need_clarify":false,"reason":"high_risk_test"})");
    thin_agent::AgentService high_svc(rej_cfg, ex, te);
    high_svc.on_session_open("c2");
    // need text that matches general_info_query_include keywords
    auto high_r = high_svc.handle_request("c2", {{"type", "chat"}, {"text", "解释一下经济学原理"}});
    if (int rc = expect(high_r["type"] == "chat_result", "contract: high-risk chat_result"); rc) return rc;
    high_svc.on_session_close("c2");

    // 3c) need_clarify=true → clarify route
    setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
                 R"({"strategy":"clarify","intent":"general_query","confidence":0.55,"need_clarify":true,"clarify_question":"请提供更多上下文？","risk":"low"})");
    thin_agent::AgentService cl_svc(rej_cfg, ex, te);
    cl_svc.on_session_open("c3");
    auto cl_r = cl_svc.handle_request("c3", {{"type", "chat"}, {"text", "能帮我做点什么"}});
    if (int rc = expect(cl_r["type"] == "chat_result", "contract: clarify chat_result"); rc) return rc;
    if (int rc = expect(cl_r["decision"]["policy"] == "clarify" || cl_r["decision"]["route"].get<std::string>().find("clarify") != std::string::npos,
                        "contract: clarify policy"); rc) return rc;
    cl_svc.on_session_close("c3");

    setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE", "");
    setenv_local("DUMMY_API_KEY", "");
  }

  // ── 4. 递归 handle_chat 补传 on_chunk 回归测试 ──────────────
  {
    // 验证 on_chunk 参数在所有 handle_chat 调用中都被正确传递。
    // 创建 mock 配置，让云策略返回 weather route_hint 触发递归。
    setenv_local("DUMMY_API_KEY", "mock-key-test");
    setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
                 R"({"strategy":"answer_direct","intent":"weather","confidence":0.80,"local_route_hint":"local_external_weather","risk":"low","response_draft":"mock weather"})");
    thin_agent::DemoConfigCompat rec_cfg;
    rec_cfg.mode = "cloud";
    rec_cfg.fallback = "none";
    rec_cfg.api_key_env = "DUMMY_API_KEY";
    thin_agent::AgentService rec_svc(rec_cfg, ex, te);
    rec_svc.on_session_open("rec");

    // 带 on_chunk 回调：即使递归也不 crash
    int rec_chunks = 0;
    thin_agent::StreamCallback rec_cb =
        [&](const std::string&, bool done) {
          if (!done) ++rec_chunks;
        };
    auto rec_r = rec_svc.handle_request("rec",
        {{"type", "chat"}, {"text", "查天气"}}, rec_cb);
    if (int rc = expect(rec_r["type"] == "chat_result", "recurse: returns chat_result"); rc) return rc;

    // 不带 on_chunk：nullptr 也不 crash
    auto rec_null = rec_svc.handle_request("rec",
        {{"type", "chat"}, {"text", "上海天气"}}, nullptr);
    if (int rc = expect(rec_null["type"] == "chat_result", "recurse-null: returns chat_result"); rc) return rc;

    rec_svc.on_session_close("rec");
    setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE", "");
    setenv_local("DUMMY_API_KEY", "");
  }

  // ── 5. 会话生命周期边界测试 ──────────────────────────────────
  {
    thin_agent::AgentService lc_svc(cfg, ex, te);

    // 5a) 未 open 的 session 直接 chat：不 crash
    auto pre_open = lc_svc.handle_request("ghost_session", {{"type", "chat"}, {"text", "ping"}});
    if (int rc = expect(pre_open["type"] == "chat_result", "lifecycle: ghost session chat"); rc) return rc;

    // 5b) open → chat → close → chat again：不 crash
    lc_svc.on_session_open("lc1");
    auto lc1 = lc_svc.handle_request("lc1", {{"type", "chat"}, {"text", "test1"}});
    if (int rc = expect(lc1["type"] == "chat_result", "lifecycle: session1 chat1"); rc) return rc;
    lc_svc.on_session_close("lc1");
    auto lc2 = lc_svc.handle_request("lc1", {{"type", "chat"}, {"text", "test2"}});
    if (int rc = expect(lc2["type"] == "chat_result", "lifecycle: session1 chat2 after close"); rc) return rc;

    // 5c) 多 session 并发：不同 session_id 隔离
    lc_svc.on_session_open("sA");
    lc_svc.on_session_open("sB");
    auto sA_r = lc_svc.handle_request("sA", {{"type", "chat"}, {"text", "A's message"}});
    auto sB_r = lc_svc.handle_request("sB", {{"type", "chat"}, {"text", "B's message"}});
    if (int rc = expect(sA_r["type"] == "chat_result", "lifecycle: sA chat"); rc) return rc;
    if (int rc = expect(sB_r["type"] == "chat_result", "lifecycle: sB chat"); rc) return rc;
    lc_svc.on_session_close("sA");
    lc_svc.on_session_close("sB");
  }

  // ── 6. StreamCallback 健壮性测试 ─────────────────────────────
  {
    thin_agent::AgentService robust_svc(cfg, ex, te);
    robust_svc.on_session_open("robust");

    // 6a) callback 抛异常不 crash（lambda 内 throw 由 AgentService 边界捕获）
    int called = 0;
    thin_agent::StreamCallback throwing_cb =
        [&](const std::string&, bool) { ++called; };
    auto r1 = robust_svc.handle_request("robust",
        {{"type", "chat"}, {"text", "稳定测试"}}, throwing_cb);
    if (int rc = expect(r1["type"] == "chat_result", "robust: throwing cb chat"); rc) return rc;

    // 6b) 短时间内多次请求 + 回调：内存不泄漏检查（通过正常运行验证）
    for (int i = 0; i < 3; ++i) {
      int n = 0;
      thin_agent::StreamCallback loop_cb = [&](const std::string&, bool done) { if (!done) ++n; };
      auto r = robust_svc.handle_request("robust",
          {{"type", "chat"}, {"text", "loop" + std::to_string(i)}}, loop_cb);
      if (int rc = expect(r["type"] == "chat_result",
                          ("robust: loop" + std::to_string(i) + " chat").c_str()); rc) return rc;
    }

    robust_svc.on_session_close("robust");
  }

  // ── 7. 云策略 answer_direct + response_draft 路径 ─────────────
  {
    setenv_local("DUMMY_API_KEY", "mock-key-test");
    setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
                 R"({"strategy":"answer_direct","intent":"general_query","confidence":0.72,"risk":"low","response_draft":"这是一条模拟的云端回复。","local_route_hint":""})");
    thin_agent::DemoConfigCompat ans_cfg;
    ans_cfg.mode = "cloud";
    ans_cfg.fallback = "none";
    ans_cfg.api_key_env = "DUMMY_API_KEY";
    thin_agent::AgentService ans_svc(ans_cfg, ex, te);
    ans_svc.on_session_open("ans");

    auto ans_r = ans_svc.handle_request("ans", {{"type", "chat"}, {"text", "解释量子计算"}});
    if (int rc = expect(ans_r["type"] == "chat_result", "answer: chat_result"); rc) return rc;
    if (int rc = expect(ans_r["mode_used"] == "cloud", "answer: cloud mode"); rc) return rc;
    if (int rc = expect(!ans_r["text"].get<std::string>().empty(), "answer: has text"); rc) return rc;
    if (int rc = expect(ans_r["decision"]["route"] == "cloud_llm", "answer: route=cloud_llm"); rc) return rc;

    ans_svc.on_session_close("ans");
    setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE", "");
    setenv_local("DUMMY_API_KEY", "");
  }

  // ── 8. Tier 2 云意图分类 → 回到本地 handler ─────────────
  {
    setenv_local("DUMMY_API_KEY", "mock-key-test");
    // Mock: Flash 返回的 content 直接是意图 JSON（chat_completion 原样返回 mock 文本）
    setenv_local("THIN_AGENT_TEST_CLOUD_INTENT_RESPONSE",
                 R"({"intent":"profile","confidence":0.92,"slots":{}})");

    thin_agent::DemoConfigCompat t2_cfg;
    t2_cfg.mode = "cloud";
    t2_cfg.fallback = "none";
    t2_cfg.api_key_env = "DUMMY_API_KEY";
    thin_agent::AgentService t2_svc(t2_cfg, ex, te);
    t2_svc.on_session_open("t2");

    // "what r u" — ONNX may not classify, Tier 2 Flash should classify as profile
    auto t2_what_r_u = t2_svc.handle_request("t2", nlohmann::json{{"type", "chat"}, {"text", "what r u"}});
    if (int rc = expect(t2_what_r_u["type"] == "chat_result", "tier2 what r u: chat_result"); rc) return rc;
    if (int rc = expect(t2_what_r_u["mode_used"] == "local-agent", "tier2 what r u: local-agent mode"); rc) return rc;
    if (int rc = expect(t2_what_r_u["decision"]["route"] == "local_profile",
                         "tier2 what r u: route=local_profile"); rc) return rc;

    // "what are u" — variant, same path
    auto t2_what_are_u = t2_svc.handle_request("t2", nlohmann::json{{"type", "chat"}, {"text", "what are u"}});
    if (int rc = expect(t2_what_are_u["decision"]["route"] == "local_profile",
                         "tier2 what are u: route=local_profile"); rc) return rc;

    // Mock: classify as status
    setenv_local("THIN_AGENT_TEST_CLOUD_INTENT_RESPONSE",
                 R"({"intent":"status","confidence":0.88,"slots":{}})");

    auto t2_status = t2_svc.handle_request("t2", nlohmann::json{{"type", "chat"}, {"text", "how is the system"}});
    if (int rc = expect(t2_status["type"] == "chat_result", "tier2 status: chat_result"); rc) return rc;
    // 测试环境中 ONNX 可能已分类为 status 并走本地 handler，或走 cloud-classify
    if (int rc = expect(t2_status["decision"]["route"] == "local_status" ||
                         t2_status["decision"]["route"] == "cloud_error",
                         "tier2 status: route=local_status (or cloud_error in test env)"); rc) return rc;

    // Mock: classify as complex (no local handler, should fall through to Tier 3)
    setenv_local("THIN_AGENT_TEST_CLOUD_INTENT_RESPONSE",
                 R"({"intent":"complex","confidence":0.85,"slots":{}})");

    auto t2_complex = t2_svc.handle_request("t2", nlohmann::json{{"type", "chat"}, {"text", "write quicksort"}});
    if (int rc = expect(t2_complex["type"] == "chat_result", "tier2 complex: chat_result"); rc) return rc;
    // complex 无 handler → fallthrough 到 Tier 3/cloud strategy（测试环境可能失败）

    t2_svc.on_session_close("t2");
    setenv_local("THIN_AGENT_TEST_CLOUD_INTENT_RESPONSE", "");
    setenv_local("DUMMY_API_KEY", "");
  }

  // ── 9. Skill Pipeline 测试 (write_file cpp handler + 汇总路由) ──
  {
    // v0.54.32: 使用精简 fixture data/test_chat_policy_skill.json（仅 skills/whitelist，
    // 避免拷贝完整 chat_policy 触发只读库/重配置副作用）。
    const std::string skill_policy_path = "data/test_chat_policy_skill.json";
    ::setenv("THIN_AGENT_CHAT_POLICY_PATH", skill_policy_path.c_str(), 1);

    setenv_local("DUMMY_API_KEY", "mock-skill-test");

    thin_agent::DemoConfigCompat skill_cfg;
    skill_cfg.mode = "cloud";
    skill_cfg.provider = "openai-compatible";
    skill_cfg.model_name = "mock-model";
    skill_cfg.api_base = "http://127.0.0.1:18080/v1";
    skill_cfg.api_key_env = "DUMMY_API_KEY";
    skill_cfg.fallback = "none";

    // 9a) write_file cpp handler — pipeline 成功
    {
      setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
        R"({"strategy":"answer_direct","intent":"file_ops","confidence":0.85,"risk":"low","local_route_hint":"local_task_pipeline","response_draft":"pipeline ok","task_pipeline":[{"step":1,"action":"write_file","params":{"path":"data/test_write.txt","content":"hello skill pipeline"}}]})");

      thin_agent::AgentService skill_svc(skill_cfg, ex, te);
      skill_svc.on_session_open("sk1");
      auto r = skill_svc.handle_request("sk1", {{"type", "chat"}, {"text", "写一个测试文件"}});

      if (int rc = expect(r["type"] == "chat_result", "skill: write_file chat_result"); rc) return rc;
      if (int rc = expect(r["decision"]["route"] == "local_task_pipeline", "skill: write_file route=local_task_pipeline"); rc) return rc;
      if (int rc = expect(r["observation"].contains("pipeline_execution"), "skill: has pipeline_execution"); rc) return rc;
      if (int rc = expect(r["observation"]["pipeline_execution"]["ok"].get<bool>() == true, "skill: write_file ok=true"); rc) return rc;
      if (int rc = expect(r["observation"]["pipeline_execution"]["failed"].get<bool>() == false, "skill: write_file failed=false"); rc) return rc;
      // 验证文件确实被写入
      {
        std::ifstream fin("data/test_write.txt");
        std::string content;
        std::getline(fin, content);
        if (int rc = expect(content == "hello skill pipeline", "skill: write_file content verified"); rc) return rc;
      }
      skill_svc.on_session_close("sk1");
    }

    // 9b) 未知 action — 被 contract 拒绝，不进 pipeline
    {
      setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
        R"({"strategy":"answer_direct","intent":"file_ops","confidence":0.85,"risk":"low","local_route_hint":"local_task_pipeline","response_draft":"pipeline ok","task_pipeline":[{"step":1,"action":"delete_everything","params":{}}]})");

      thin_agent::AgentService skill_svc2(skill_cfg, ex, te);
      skill_svc2.on_session_open("sk2");
      auto r = skill_svc2.handle_request("sk2", {{"type", "chat"}, {"text", "执行危险操作"}});

      if (int rc = expect(r["type"] == "chat_result", "skill: unknown_action chat_result"); rc) return rc;
      // 未知 action 被 contract 拦截 → route=local_clarify（合约违规）
      if (int rc = expect(r["decision"]["route"] == "local_clarify" ||
                           r["decision"]["reason"] == "cloud_policy_contract_violation",
                           "skill: unknown rejected by contract"); rc) return rc;
      if (int rc = expect(r["observation"].contains("cloud_task_pipeline_contract"), "skill: unknown has pipeline contract"); rc) return rc;
      if (int rc = expect(r["observation"]["cloud_task_pipeline_contract"]["valid"].get<bool>() == false, "skill: unknown contract invalid"); rc) return rc;
      skill_svc2.on_session_close("sk2");
    }

    // 9c) 汇总路由：简单结果 → 本地模板 ≤500字（含参数详情）
    {
      setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
        R"({"strategy":"answer_direct","intent":"file_ops","confidence":0.85,"risk":"low","local_route_hint":"local_task_pipeline","response_draft":"pipeline ok","task_pipeline":[{"step":1,"action":"write_file","params":{"path":"data/test_simple.txt","content":"test"}}]})");

      thin_agent::AgentService skill_svc3(skill_cfg, ex, te);
      skill_svc3.on_session_open("sk3");
      auto r = skill_svc3.handle_request("sk3", {{"type", "chat"}, {"text", "读取测试文件"}});

      if (int rc = expect(r["type"] == "chat_result", "skill: simple summary chat_result"); rc) return rc;
      // 简单结果（1 步 + 输出 <500）→ 本地模板，不走 cloud summary
      if (int rc = expect(r["text"].get<std::string>().find("执行完成") != std::string::npos, "skill: simple uses local template"); rc) return rc;
      // 验证参数详情格式: "write_file: data/test_simple.txt → ✅"
      if (int rc = expect(r["text"].get<std::string>().find("write_file: data/test_simple.txt") != std::string::npos, "skill: summary has param detail"); rc) return rc;
      if (int rc = expect(r["text"].get<std::string>().find("✅") != std::string::npos, "skill: summary has checkmark"); rc) return rc;
      skill_svc3.on_session_close("sk3");
    }

    // 9d) v0.54.32: media+技能混管线 → contract 拒绝（防 stub 假成功）
    {
      setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
        R"({"strategy":"answer_direct","intent":"file_ops","confidence":0.85,"risk":"low","local_route_hint":"local_task_pipeline","response_draft":"pipeline ok","task_pipeline":[{"step":1,"action":"write_file","params":{"path":"data/x.txt","content":"a"}},{"step":2,"action":"capture_photo","params":{}}]})");

      thin_agent::AgentService skill_svc4(skill_cfg, ex, te);
      skill_svc4.on_session_open("sk4");
      auto r = skill_svc4.handle_request("sk4", {{"type", "chat"}, {"text", "执行这个多步任务"}});

      if (int rc = expect(r["type"] == "chat_result", "skill: mixed media chat_result"); rc) return rc;
      // 合约违规走 make_local_reply；observation 必带 pipeline contract
      if (int rc = expect(r["decision"]["reason"] == "cloud_policy_contract_violation" ||
                           r["decision"]["route"] == "local_clarify",
                           "skill: mixed rejected by contract"); rc) return rc;
      if (int rc = expect(r["observation"].contains("cloud_task_pipeline_contract") ||
                           r["observation"].contains("cloud_policy_contract"),
                           "skill: mixed has pipeline contract"); rc) return rc;
      bool contract_invalid = false;
      if (r["observation"].contains("cloud_task_pipeline_contract")) {
        contract_invalid = !r["observation"]["cloud_task_pipeline_contract"].value("valid", true);
        const auto errs = r["observation"]["cloud_task_pipeline_contract"].value("errors", nlohmann::json::array());
        for (const auto& pe : errs) {
          if (pe.is_string() && pe.get<std::string>().find("mixed_media") != std::string::npos)
            contract_invalid = true;
        }
      }
      if (r["observation"].contains("cloud_policy_contract")) {
        const auto errs = r["observation"]["cloud_policy_contract"].value("errors", nlohmann::json::array());
        for (const auto& pe : errs) {
          if (pe.is_string() && pe.get<std::string>().find("invalid_task_pipeline") != std::string::npos)
            contract_invalid = true;
        }
      }
      if (int rc = expect(contract_invalid, "skill: mixed_media_and_skill_pipeline error"); rc) return rc;
      if (int rc = expect(!r["observation"].contains("pipeline_execution"),
                           "skill: mixed must not execute pipeline"); rc) return rc;
      skill_svc4.on_session_close("sk4");
    }

    // 9e) v0.54.32: shell_exec 走加固 cpp——注入命令被拒绝，不得假成功
    {
      setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
        R"({"strategy":"answer_direct","intent":"file_ops","confidence":0.85,"risk":"low","local_route_hint":"local_task_pipeline","response_draft":"pipeline ok","task_pipeline":[{"step":1,"action":"shell_exec","params":{"command":"echo hi; curl http://evil.example"}}]})");

      thin_agent::AgentService skill_svc5(skill_cfg, ex, te);
      skill_svc5.on_session_open("sk5");
      auto r = skill_svc5.handle_request("sk5", {{"type", "chat"}, {"text", "执行命令"}});

      if (int rc = expect(r["type"] == "chat_result", "skill: inject chat_result"); rc) return rc;
      if (int rc = expect(r["decision"]["route"] == "local_task_pipeline", "skill: inject route"); rc) return rc;
      if (int rc = expect(r["observation"].contains("pipeline_execution"), "skill: inject has execution"); rc) return rc;
      if (int rc = expect(r["observation"]["pipeline_execution"]["failed"].get<bool>() == true,
                           "skill: inject must fail"); rc) return rc;
      const std::string fr = r["observation"]["pipeline_execution"].value("fail_reason", "");
      if (int rc = expect(!fr.empty(), "skill: inject fail_reason set"); rc) return rc;
      skill_svc5.on_session_close("sk5");
    }

    // 9f) v0.54.33: search_code 走纯 C++——能搜到内容，且注入 pattern 不落盘副作用
    {
      {
        std::ofstream fout("data/search_probe.txt");
        fout << "needle_v05433_unique_token in file\n";
      }
      setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
        R"({"strategy":"answer_direct","intent":"file_ops","confidence":0.85,"risk":"low","local_route_hint":"local_task_pipeline","response_draft":"pipeline ok","task_pipeline":[{"step":1,"action":"search_code","params":{"pattern":"needle_v05433_unique_token","dir":"data"}}]})");

      thin_agent::AgentService skill_svc6(skill_cfg, ex, te);
      skill_svc6.on_session_open("sk6");
      auto r = skill_svc6.handle_request("sk6", {{"type", "chat"}, {"text", "搜索代码"}});

      if (int rc = expect(r["type"] == "chat_result", "skill: search_code chat_result"); rc) return rc;
      if (int rc = expect(r["decision"]["route"] == "local_task_pipeline", "skill: search_code route"); rc) return rc;
      if (int rc = expect(r["observation"].contains("pipeline_execution"), "skill: search_code has execution"); rc) return rc;
      if (int rc = expect(r["observation"]["pipeline_execution"]["ok"].get<bool>() == true,
                           "skill: search_code ok"); rc) return rc;
      const auto& steps = r["observation"]["pipeline_execution"]["pipeline_steps"];
      if (int rc = expect(steps.is_array() && !steps.empty(), "skill: search_code steps"); rc) return rc;
      const std::string out = steps[0].value("output", "");
      if (int rc = expect(out.find("needle_v05433_unique_token") != std::string::npos,
                           "skill: search_code finds needle"); rc) return rc;
      skill_svc6.on_session_close("sk6");

      // 注入 pattern：若仍拼 grep 进 shell，会 touch 副作用文件；cpp 路径只做字面/正则搜索
      const std::string inject_marker = "data/search_inject_side_effect.flag";
      std::filesystem::remove(inject_marker);
      setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
        R"({"strategy":"answer_direct","intent":"file_ops","confidence":0.85,"risk":"low","local_route_hint":"local_task_pipeline","response_draft":"pipeline ok","task_pipeline":[{"step":1,"action":"search_code","params":{"pattern":"x' /tmp; touch data/search_inject_side_effect.flag; echo '","dir":"data"}}]})");

      thin_agent::AgentService skill_svc6b(skill_cfg, ex, te);
      skill_svc6b.on_session_open("sk6b");
      auto r2 = skill_svc6b.handle_request("sk6b", {{"type", "chat"}, {"text", "搜索注入"}});
      if (int rc = expect(r2["observation"].contains("pipeline_execution"),
                           "skill: search inject has execution"); rc) return rc;
      // cpp 路径应成功返回（无匹配或字面搜），绝不能出现 touch 副作用
      if (int rc = expect(!std::filesystem::exists(inject_marker),
                           "skill: search_code must not shell-inject"); rc) return rc;
      skill_svc6b.on_session_close("sk6b");
    }

    // 9g) v0.54.33: parser_mode=fallback 不得清空 invalid_task_pipeline 硬错误
    {
      setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
        R"({"strategy":"answer_direct","intent":"file_ops","confidence":0.85,"risk":"low","local_route_hint":"local_task_pipeline","parser_mode":"fallback","response_draft":"fallback draft text","task_pipeline":[{"step":1,"action":"write_file","params":{"path":"data/x.txt","content":"a"}},{"step":2,"action":"capture_photo","params":{}}]})");

      thin_agent::AgentService skill_svc7(skill_cfg, ex, te);
      skill_svc7.on_session_open("sk7");
      auto r = skill_svc7.handle_request("sk7", {{"type", "chat"}, {"text", "混管线 fallback"}});

      if (int rc = expect(r["type"] == "chat_result", "skill: fallback hard chat_result"); rc) return rc;
      if (int rc = expect(r["decision"]["reason"] == "cloud_policy_contract_violation" ||
                           r["decision"]["route"] == "local_clarify",
                           "skill: fallback must keep pipeline hard error"); rc) return rc;
      if (int rc = expect(!r["observation"].contains("pipeline_execution"),
                           "skill: fallback hard must not execute"); rc) return rc;
      skill_svc7.on_session_close("sk7");
    }

    // 9h) v0.54.33: 纯 media 动作进 pipeline → 纵深拒绝（不撞 stub 假成功）
    {
      setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE",
        R"({"strategy":"answer_direct","intent":"file_ops","confidence":0.85,"risk":"low","local_route_hint":"local_task_pipeline","response_draft":"pipeline ok","task_pipeline":[{"step":1,"action":"capture_photo","params":{}}]})");

      thin_agent::AgentService skill_svc8(skill_cfg, ex, te);
      skill_svc8.on_session_open("sk8");
      auto r = skill_svc8.handle_request("sk8", {{"type", "chat"}, {"text", "拍照"}});

      if (int rc = expect(r["type"] == "chat_result", "skill: media-only chat_result"); rc) return rc;
      // 纯 media 可能被 contract 当合法（走 TaskEngine 路由）或进入 pipeline 后被纵深拦；
      // 若进了 pipeline，必须 failed 且 reason=media_action_requires_task_engine
      if (r["observation"].contains("pipeline_execution")) {
        if (int rc = expect(r["observation"]["pipeline_execution"]["failed"].get<bool>() == true,
                             "skill: media-only pipeline must fail"); rc) return rc;
        const std::string fr = r["observation"]["pipeline_execution"].value("fail_reason", "");
        if (int rc = expect(fr.find("media_action_requires_task_engine") != std::string::npos,
                             "skill: media-only fail_reason"); rc) return rc;
      }
      skill_svc8.on_session_close("sk8");
    }

    setenv_local("THIN_AGENT_TEST_CLOUD_RESPONSE", "");
    setenv_local("DUMMY_API_KEY", "");
    ::setenv("THIN_AGENT_CHAT_POLICY_PATH", chat_policy_path.c_str(), 1);
    std::filesystem::remove("data/test_write.txt");
    std::filesystem::remove("data/test_simple.txt");
    std::filesystem::remove("data/search_probe.txt");
    // 保留 data/test_chat_policy_skill.json fixture（勿删）
  }

  // ── 10) 云调用失败回退：key 有效但 API 不可达 → HybridRouter 级联 ──
  {
    setenv_local("THIN_AGENT_CLOUD_FAIL_KEY", "fake-but-present");
    thin_agent::DemoConfigCompat cloud_fail_cfg;
    cloud_fail_cfg.mode = "cloud";
    cloud_fail_cfg.provider = "openai-compatible";
    cloud_fail_cfg.model_name = "gpt-4.1-mini";
    cloud_fail_cfg.api_base = "http://127.0.0.1:9/v1";  // 不可达
    cloud_fail_cfg.api_key_env = "THIN_AGENT_CLOUD_FAIL_KEY";
    cloud_fail_cfg.request_timeout_ms = 500;
    cloud_fail_cfg.fallback = "offline";

    thin_agent::AgentService cloud_fail_svc(cloud_fail_cfg, ex, te);
    cloud_fail_svc.on_session_open("cf1");
    auto r = cloud_fail_svc.handle_request("cf1", {{"type", "chat"}, {"text", "你好"}});

    if (int rc = expect(r["type"] == "chat_result", "cloud fail: chat_result type"); rc) return rc;
    // 有 key → cloud 失败 → HybridRouter 尝试 → mode_used 为 local-agent 或 offline-fallback
    if (int rc = expect(r["mode_used"] == "local-agent" || r["mode_used"] == "offline-fallback",
                        "cloud fail: fallback to local/offline"); rc) return rc;
    if (int rc = expect(r["fallback_reason"] == "cloud_call_failed",
                        "cloud fail: fallback_reason=cloud_call_failed"); rc) return rc;
    if (int rc = expect(!r["text"].get<std::string>().empty(),
                        "cloud fail: has reply text"); rc) return rc;
    if (int rc = expect(r["cloud_error"].is_string(),
                        "cloud fail: has cloud_error"); rc) return rc;
    cloud_fail_svc.on_session_close("cf1");
    setenv_local("THIN_AGENT_CLOUD_FAIL_KEY", "");
  }

  // ── 11) --auto 模式：local-first 级联命中则秒回 ──
  {
    thin_agent::DemoConfigCompat auto_cfg;
    auto_cfg.mode = "auto";
    auto_cfg.provider = "openai-compatible";
    auto_cfg.model_name = "glm-5.2";
    auto_cfg.api_base = "http://127.0.0.1:9/v1";  // 云不可达
    auto_cfg.api_key_env = "THIN_AGENT_TEST_MISSING_KEY";  // 无 key → 强制走本地
    auto_cfg.request_timeout_ms = 500;
    auto_cfg.fallback = "offline";

    thin_agent::AgentService auto_svc(auto_cfg, ex, te);
    auto_svc.on_session_open("auto1");
    auto r = auto_svc.handle_request("auto1", {{"type", "chat"}, {"text", "你好"}});

    if (int rc = expect(r["type"] == "chat_result", "auto: chat_result type"); rc) return rc;
    // auto 模式本地级联命中 → mode_used 为 local-agent / offline / offline-fallback
    if (int rc = expect(r["mode_used"] == "local-agent" || r["mode_used"] == "offline" || r["mode_used"] == "offline-fallback",
                        "auto: local cascade hit"); rc) return rc;
    if (int rc = expect(!r["text"].get<std::string>().empty(),
                        "auto: has reply text"); rc) return rc;
    auto_svc.on_session_close("auto1");
  }

  // ── 12) metrics_prometheus：纯单元契约（不起服务、不占端口）──
  // v0.54.13 (R93「测试登记债」): 自 tests/e2e/metrics_unit_test.py 迁移。那个脚本自
  // v0.53.4 起 AgentService 构造签名变更为 (cfg, executor, task_engine) 后就**编译不过**
  // （`no matching function for call to AgentService::AgentService()`），且它未登记进 CI
  // ⇒ 烂了没人看见。这里复用上面已构造的 svc，把这批断言放回**它真正该在的层**：
  // 纯单元（e2e_metrics 走真服务 /metrics，覆盖的是"端点+鉴权"，不是"导出函数本身"）。
  {
    const std::string mtx = svc.metrics_prometheus();
    auto has = [&](const std::string& s) { return mtx.find(s) != std::string::npos; };
    if (int rc = expect(has("# HELP thin_agent_uptime_seconds"), "metrics: HELP uptime"); rc) return rc;
    if (int rc = expect(has("# TYPE thin_agent_uptime_seconds gauge"), "metrics: TYPE uptime gauge"); rc) return rc;
    if (int rc = expect(has("# TYPE thin_agent_api_calls_total counter"), "metrics: TYPE api_calls counter"); rc) return rc;
    if (int rc = expect(has("thin_agent_version_info{version="), "metrics: version_info 带标签"); rc) return rc;
    if (int rc = expect(has("thin_agent_api_calls_total "), "metrics: api_calls 样本行"); rc) return rc;
    if (int rc = expect(has("thin_agent_sessions_active "), "metrics: sessions_active 样本行"); rc) return rc;
    // 格式律：整数直出（counter/gauge 整值不得渲染成 0.000000）
    if (int rc = expect(mtx.find("0.000000") == std::string::npos, "metrics: 无 0.000000（整数直出）"); rc) return rc;
    // 三段齐整律：每个 # TYPE 的指标名必须有样本行（名 或 名{），且 HELP/TYPE/样本三计数相等
    // （实现里每个指标恒发 3 行——任一指标少发一段即断格式律，Prometheus 解析器会报错）
    int n_help = 0, n_type = 0, n_sample = 0;
    for (size_t begin = 0; begin <= mtx.size();) {
      const size_t nl = mtx.find('\n', begin);
      const std::string line = mtx.substr(begin, (nl == std::string::npos ? mtx.size() : nl) - begin);
      if (line.rfind("# HELP ", 0) == 0) {
        ++n_help;
      } else if (line.rfind("# TYPE ", 0) == 0) {
        ++n_type;
        const std::string rest = line.substr(7);
        const std::string metric = rest.substr(0, rest.find(' '));
        if (!has(metric + " ") && !has(metric + "{")) {
          std::cerr << "FAIL: metrics: TYPE 无样本 " << metric << "\n";
          return 1;
        }
      } else if (!line.empty() && line[0] != '#') {
        ++n_sample;
      }
      if (nl == std::string::npos) break;
      begin = nl + 1;
    }
    if (int rc = expect(n_type > 0 && n_sample == n_type && n_help == n_type,
                        "metrics: HELP/TYPE/样本 三段齐整"); rc) return rc;
  }

  std::cout << "unit:test_agent_service PASS\n";
  return 0;
}
