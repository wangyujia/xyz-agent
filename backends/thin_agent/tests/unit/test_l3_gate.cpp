// unit_l3_gate：v0.52.4 L3 验证 gate 回归（mock LLM，无真网）
//
// 三态：
//   1) 无项目上下文 → gate skipped，正常合成（通用性：非编程任务不受阻）
//   2) 项目带失败测试 → 合成被拒，响应 error=project tests failed，
//      gate.commands[0].ok=false 带 pytest 输出；父目标 failed
//   3) 修好测试 → resume 重验过闸 → ok 合成
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/TaskEngine.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"

#include "test_macros.h"

using namespace thin_agent;

int main() {
  std::filesystem::remove_all("/tmp/l3_home");
  std::filesystem::create_directories("/tmp/l3_home/data");
  setenv("THIN_AGENT_HOME", "/tmp/l3_home", 1);

  // 项目：pytest 带一个失败测试
  std::filesystem::create_directories("/tmp/l3_proj");
  {
    std::ofstream f("/tmp/l3_proj/test_calc.py");
    f << "def test_broken():\n    assert 1 + 1 == 3  # 故意红\n";
  }
  {
    std::ofstream f("/tmp/l3_proj/pytest.ini");
    f << "[pytest]\n";
  }

  const char* mock = "```json\n"
      "{\"tasks\":["
      "{\"task_id\":\"t1\",\"name\":\"实现\",\"prompt\":\"impl\",\"role\":\"coder\",\"depends_on\":[]}"
      "]}\n"
      "```\n尾随。";
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

    // ── 1) 无项目上下文 → skipped ──
    nlohmann::json req;
    req["type"] = "agent_decompose_and_run";
    req["goal"] = "无项目任务";
    req["use_goal_tree"] = true;
    auto r0 = svc.handle_request("sess_l3_a", req);
    ASSERT_TRUE("无项目 gate 跳过仍 ok", r0.value("ok", false));

    // ── 2) 项目红 → 拒合成 ──
    nlohmann::json preq;
    preq["type"] = "set_project";
    preq["path"] = "/tmp/l3_proj";
    preq["mode"] = "rw";
    svc.handle_request("sess_l3_b", preq);

    nlohmann::json req2;
    req2["type"] = "agent_decompose_and_run";
    req2["goal"] = "修复 calc";
    req2["use_goal_tree"] = true;
    auto r1 = svc.handle_request("sess_l3_b", req2);
    ASSERT_TRUE("测试红 → ok=false", !r1.value("ok", false));
    ASSERT_TRUE("error 指明 L3 gate",
                r1.value("error", "").find("L3 gate") != std::string::npos);
    auto gate = r1.value("gate", nlohmann::json::object());
    ASSERT_TRUE("gate.skipped=false", !gate.value("skipped", true));
    ASSERT_TRUE("gate.commands 非空",
                gate.value("commands", nlohmann::json::array()).size() > 0);
    ASSERT_TRUE("失败命令是 pytest",
                gate["commands"][0].value("command", "").find("pytest") != std::string::npos);
    ASSERT_TRUE("输出尾含断言失败信息",
                gate["commands"][0].value("output_tail", "").find("assert") != std::string::npos);

    // ── 3) 修好测试 → resume 过闸 ──
    {
      std::ofstream f("/tmp/l3_proj/test_calc.py");
      f << "def test_ok():\n    assert 1 + 1 == 2\n";
    }
    int gid = r1.value("goal_tree_id", 0);
    ASSERT_TRUE("goal_tree_id 有效", gid > 0);
    nlohmann::json rreq;
    rreq["type"] = "agent_decompose_resume";
    rreq["goal_id"] = gid;
    auto r2 = svc.handle_request("sess_l3_b", rreq);
    ASSERT_TRUE("修好后 resume 过闸 ok", r2.value("ok", false));
  }

  unsetenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE");
  std::filesystem::remove_all("/tmp/l3_home");
  std::filesystem::remove_all("/tmp/l3_proj");
  if (rc) return rc;
  return TEST_REPORT();
}
