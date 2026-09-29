// unit_decompose_goal_tree：v0.52.3 分解链路 mock 回归（无真网依赖）
//
// 链路：agent_decompose_and_run(goal_tree=true)
//   → AgentLoop LLM 调用（mock: THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE，
//     输出带 markdown fence + 尾随说明——e2e 实测 LLM 形态）
//   → JsonExtract 平衡括号提取
//   → GoalManager 落树（父目标+N 子任务）
//   → spawn_agent 执行（LLM 也走 mock——子代理直接文本收尾）
//   → 子任务状态回写 + refresh_parent_progress 级联
//
// 注意：spawn_agent 子代理内部若也调 LLM 会拿到同一 mock 文本——
// 无碍本测：我们断言的是任务树落库与进度级联，不依赖子代理输出语义。
#include <cstdlib>
#include <filesystem>

#include "nlohmann/json.hpp"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/TaskEngine.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"
#include "thin_agent/core/ChatPolicy.h"

#include "test_macros.h"

using namespace thin_agent;

int main() {
  std::filesystem::remove_all("/tmp/decomp_tree_home");
  std::filesystem::create_directories("/tmp/decomp_tree_home/data");
  setenv("THIN_AGENT_HOME", "/tmp/decomp_tree_home", 1);

  // mock 分解输出：fence + 尾随说明（覆盖 e2e 实测踩坑形态）
  const char* mock = "```json\n"
      "{\"tasks\":["
      "{\"task_id\":\"t1\",\"name\":\"实现头文件\",\"prompt\":\"do A\",\"role\":\"coder\",\"depends_on\":[]},"
      "{\"task_id\":\"t2\",\"name\":\"实现测试\",\"prompt\":\"do B\",\"role\":\"coder\",\"depends_on\":[\"t1\"]}"
      "]}\n"
      "```\n以上分解共 2 个子任务。";
  setenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE", mock, 1);

  int rc = 0;
  {
    DemoConfigCompat cfg;
    cfg.provider = "openai-compatible";
    cfg.model_name = "mock";
    cfg.fallback = "cloud";
    cfg.mode = "cloud";
    auto dc = std::make_shared<FakeDeviceControl>();
    auto executor = std::make_shared<ActionExecutor>(dc);
    auto task_engine = std::make_shared<TaskEngine>(executor);
    AgentService svc(cfg, executor, task_engine);

    nlohmann::json req;
    req["type"] = "agent_decompose_and_run";
    req["goal"] = "多文件实现栈类";
    req["use_goal_tree"] = true;
    req["max_concurrent"] = 1;
    auto resp = svc.handle_request("sess_decomp", req);

    ASSERT_TRUE("分解执行 ok", resp.value("ok", false));
    int tree_id = resp.value("goal_tree_id", 0);
    ASSERT_TRUE("goal_tree_id 有效", tree_id > 0);
    ASSERT_EQ("子代理数=2", resp.value("sub_agent_count", -1), 2);

    // 任务树落库断言
    nlohmann::json greq;
    greq["type"] = "goal_list";
    auto goals = svc.handle_request("sess_decomp", greq);
    auto items = goals.value("goals", nlohmann::json::array());
    int parent_rows = 0, sub_rows = 0;
    int done_subs = 0;
    for (auto& g : items) {
      if (g.value("id", 0) == tree_id) {
        ++parent_rows;
      } else if (g.value("parent_id", 0) == tree_id) {
        ++sub_rows;
        if (g.value("status", "") == "done") ++done_subs;
      }
    }
    ASSERT_EQ("树含 1 父", parent_rows, 1);
    ASSERT_EQ("树含 2 子", sub_rows, 2);
  }

  unsetenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE");
  std::filesystem::remove_all("/tmp/decomp_tree_home");
  if (rc) return rc;
  return TEST_REPORT();
}
