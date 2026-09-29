// unit_decompose_resume：v0.52.4 L2 断点续跑回归（mock LLM，无真网）
//
// 链路：decompose_and_run(goal_tree) → meta 落 payload → 中断模拟
//（只完成部分子任务）→ agent_decompose_resume 续跑 → 已完成子任务
// 不重跑 → scan 报告。
//
// mock 行为：THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE 恒定返回分解 JSON；
// spawn_agent 子代理的 LLM 调用同 mock。为制造“部分完成”断点，
// 直接调用 AgentService 内部路径不可行（wave 函数私有）——改用两段式：
// 第一轮正常跑（全完成）验证 meta 结构；再用 SQL 改 meta 模拟
// 中断（清掉 t2 结果+改状态），resume 后断言 t1 未重跑。
#include <cstdlib>
#include <filesystem>
#include <sqlite3.h>

#include "nlohmann/json.hpp"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/TaskEngine.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"

#include "test_macros.h"

using namespace thin_agent;

int main() {
  std::filesystem::remove_all("/tmp/decomp_resume_home");
  std::filesystem::create_directories("/tmp/decomp_resume_home/data");
  setenv("THIN_AGENT_HOME", "/tmp/decomp_resume_home", 1);

  const char* mock = "```json\n"
      "{\"tasks\":["
      "{\"task_id\":\"t1\",\"name\":\"A\",\"prompt\":\"do A\",\"role\":\"coder\",\"depends_on\":[]},"
      "{\"task_id\":\"t2\",\"name\":\"B\",\"prompt\":\"do B\",\"role\":\"coder\",\"depends_on\":[\"t1\"]}"
      "]}\n"
      "```\n尾随说明文字。";
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

    // ── 1) 完整跑 → meta 落 payload ──
    nlohmann::json req;
    req["type"] = "agent_decompose_and_run";
    req["goal"] = "断点测试任务";
    req["use_goal_tree"] = true;
    req["max_concurrent"] = 1;
    auto resp = svc.handle_request("sess_r", req);
    ASSERT_TRUE("第一轮 ok", resp.value("ok", false));
    int goal_id = resp.value("goal_tree_id", 0);
    ASSERT_TRUE("goal_tree_id 有效", goal_id > 0);

    nlohmann::json greq;
    greq["type"] = "goal_get";
    greq["id"] = goal_id;
    // 用 goal_list 验证 meta（goal_get 不存在——list 已带 parent_id）
    nlohmann::json lreq;
    lreq["type"] = "goal_list";
    auto goals = svc.handle_request("sess_r", lreq);

    // meta 直查 DB（get_meta 不在 WS API——经 sqlite 验证持久化）
    sqlite3* db = nullptr;
    std::string dbpath = "/tmp/decomp_resume_home/data/goals.db";
    ASSERT_TRUE("goals.db 存在", sqlite3_open(dbpath.c_str(), &db) == SQLITE_OK);
    char* err = nullptr;
    std::string meta_str;
    sqlite3_exec(db,
        ("SELECT meta FROM goals WHERE id = " + std::to_string(goal_id) + ";").c_str(),
        [](void* p, int, char** v, char**) -> int {
          if (v[0]) *static_cast<std::string*>(p) = v[0];
          return 0;
        }, &meta_str, &err);
    ASSERT_TRUE("meta 已落库", !meta_str.empty());
    auto meta = nlohmann::json::parse(meta_str);
    ASSERT_TRUE("meta.tasks 存在", meta.contains("tasks") && meta["tasks"].size() == 2);
    ASSERT_TRUE("meta.completed 含 t1/t2",
                meta.contains("completed") && meta["completed"].contains("t1")
                && meta["completed"].contains("t2"));

    // ── 2) 模拟中断：清 t2 结果 + 置 pending ──
    meta["completed"].erase("t2");
    std::string upd = "UPDATE goals SET meta = '" + meta.dump() +
        "', status = 'active' WHERE id = " + std::to_string(goal_id) + ";";
    // JSON 单引号风险：nlohmann dump 无单引号（转义为 \"）——安全
    sqlite3_exec(db, upd.c_str(), nullptr, nullptr, nullptr);
    sqlite3_exec(db, ("UPDATE goals SET status = 'pending' WHERE parent_id = "
        + std::to_string(goal_id) + " AND description = 'B';").c_str(),
        nullptr, nullptr, nullptr);
    sqlite3_close(db);

    // scan：报 1 个可恢复（t1 done / t2 未完成 → done<total）
    nlohmann::json sreq;
    sreq["type"] = "agent_decompose_scan";
    auto scan = svc.handle_request("sess_r", sreq);
    ASSERT_EQ("scan 报 1 可恢复", scan.value("resumable", -1), 1);

    // ── 3) resume：续跑（t1 已完成不重跑——mock 下无法从输出区分
    // 重跑与否，断言以结果完整性为主：resume ok 且 completed 恢复 2 项）
    nlohmann::json rreq;
    rreq["type"] = "agent_decompose_resume";
    rreq["goal_id"] = goal_id;
    rreq["max_concurrent"] = 1;
    auto rresp = svc.handle_request("sess_r", rreq);
    ASSERT_TRUE("resume ok", rresp.value("ok", false));

    // resume 后 meta completed 恢复 2 项
    sqlite3* db2 = nullptr;
    sqlite3_open(dbpath.c_str(), &db2);
    std::string meta2_str;
    sqlite3_exec(db2,
        ("SELECT meta FROM goals WHERE id = " + std::to_string(goal_id) + ";").c_str(),
        [](void* p, int, char** v, char**) -> int {
          if (v[0]) *static_cast<std::string*>(p) = v[0];
          return 0;
        }, &meta2_str, &err);
    auto meta2 = nlohmann::json::parse(meta2_str);
    ASSERT_EQ("resume 后 completed 恢复 2 项",
              (int)meta2.value("completed", nlohmann::json::object()).size(), 2);
    sqlite3_close(db2);

    // scan：恢复后 0 可恢复
    auto scan2 = svc.handle_request("sess_r", sreq);
    ASSERT_EQ("恢复后 scan=0", scan2.value("resumable", -1), 0);
  }

  unsetenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE");
  std::filesystem::remove_all("/tmp/decomp_resume_home");
  if (rc) return rc;
  return TEST_REPORT();
}
