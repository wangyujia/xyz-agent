/// unit_workflow：工作流管理插件测试
///
/// 测试 thin_agent::workflow 的 6 个 handler
/// 专注于参数校验（WorkflowManager 单例状态不可控）

#include <cstdlib>
#include <iostream>
#include <string>

#include "thin_agent/plugin/PluginInterface.h"
#include "thin_agent/core/WorkflowManager.h"
#include "../../src/plugin/skills/workflow.cpp"

#define ASSERT(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << std::endl; return 1; } \
} while (0)

#define TEST(name) int test_##name()

using namespace nlohmann;

// ── workflow_list ──

TEST(list_basic) {
  auto r = thin_agent::workflow::handle_workflow_list(json::object());
  ASSERT(r["success"].get<bool>(), "should succeed");
  ASSERT(r["workflows"].is_array(), "workflows is array");
  std::cout << "pass: list_basic" << std::endl;
  return 0;
}

// ── workflow_view ──

TEST(view_empty_name) {
  json params = {{"name", ""}};
  auto r = thin_agent::workflow::handle_workflow_view(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty name");
  std::cout << "pass: view_empty_name" << std::endl;
  return 0;
}

TEST(view_not_found) {
  json params = {{"name", "nonexistent_workflow_xyz"}};
  auto r = thin_agent::workflow::handle_workflow_view(params);
  ASSERT(!r["success"].get<bool>(), "should fail on nonexistent");
  std::cout << "pass: view_not_found" << std::endl;
  return 0;
}

// ── workflow_create ──

TEST(create_empty_name) {
  json params = {{"name", ""}};
  auto r = thin_agent::workflow::handle_workflow_create(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty name");
  std::cout << "pass: create_empty_name" << std::endl;
  return 0;
}

TEST(create_no_steps) {
  json params = {{"name", "test_wf"}};
  auto r = thin_agent::workflow::handle_workflow_create(params);
  ASSERT(!r["success"].get<bool>(), "should fail without steps");
  std::cout << "pass: create_no_steps" << std::endl;
  return 0;
}

// ── workflow_set ──

TEST(set_empty_name) {
  json params = {{"name", ""}};
  auto r = thin_agent::workflow::handle_workflow_set(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty name");
  std::cout << "pass: set_empty_name" << std::endl;
  return 0;
}

TEST(set_not_found) {
  json params = {{"name", "nonexistent_wf"}};
  auto r = thin_agent::workflow::handle_workflow_set(params);
  ASSERT(!r["success"].get<bool>(), "should fail on nonexistent");
  std::cout << "pass: set_not_found" << std::endl;
  return 0;
}

// ── workflow_delete ──

TEST(delete_empty_name) {
  json params = {{"name", ""}};
  auto r = thin_agent::workflow::handle_workflow_delete(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty name");
  std::cout << "pass: delete_empty_name" << std::endl;
  return 0;
}

TEST(delete_not_found) {
  json params = {{"name", "nonexistent_wf"}};
  auto r = thin_agent::workflow::handle_workflow_delete(params);
  ASSERT(!r["success"].get<bool>(), "should fail on nonexistent");
  std::cout << "pass: delete_not_found" << std::endl;
  return 0;
}

// ── workflow_run ──

TEST(run_empty_name) {
  json params = {{"name", ""}};
  auto r = thin_agent::workflow::handle_workflow_run(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty name");
  std::cout << "pass: run_empty_name" << std::endl;
  return 0;
}

int main() {
  int fails = 0;
  #define RUN(name) do { \
    std::cout << "test: " #name "..." << std::endl; \
    if (test_##name() != 0) ++fails; \
  } while (0)

  RUN(list_basic);
  RUN(view_empty_name);
  RUN(view_not_found);
  RUN(create_empty_name);
  RUN(create_no_steps);
  RUN(set_empty_name);
  RUN(set_not_found);
  RUN(delete_empty_name);
  RUN(delete_not_found);
  RUN(run_empty_name);

  std::cout << (fails == 0 ? "ALL PASS" : "SOME FAILED") << " (" << fails << " failures)" << std::endl;
  return fails;
}
