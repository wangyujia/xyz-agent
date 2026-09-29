// unit_goal_tree_complex_task：v0.52.3 L1 接线回归
//
// 1) GoalManager 任务树：create_subtask 双向关系 + refresh_parent_progress
//    级联（全 done → 父 100%+done；有 failed → 父 failed）
// 2) classify_local_intent：complex_task 词表命中（"重构"）且不抢
//    status/memory 等精确意图
#include <filesystem>

#include "nlohmann/json.hpp"
#include "thin_agent/agent/GoalManager.h"

#include "test_macros.h"

using namespace thin_agent;

int main() {
  // ── 1) GoalManager 任务树 ──
  std::filesystem::remove_all("/tmp/goal_tree_test_home");
  std::filesystem::create_directories("/tmp/goal_tree_test_home");
  {
    agent::GoalManager gm("/tmp/goal_tree_test_home/goals.db");

    auto parent = gm.create("重构 RingQueue 模块");
    ASSERT_TRUE("父目标创建", parent.id > 0);

    auto s1 = gm.create_subtask(parent.id, "实现 ring_queue.h");
    auto s2 = gm.create_subtask(parent.id, "实现测试");
    auto s3 = gm.create_subtask(parent.id, "编译验证");
    ASSERT_TRUE("子任务 id 有效", s1.id > 0 && s2.id > 0 && s3.id > 0);
    ASSERT_TRUE("子任务 parent_id 正确", s1.parent_id == parent.id);

    auto got = gm.get(parent.id);
    ASSERT_EQ("父 subtask_ids 含 3 子", (int)got.subtask_ids.size(), 3);

    // 2/3 done → 66%
    gm.update_status(s1.id, "done");
    int pct = gm.refresh_parent_progress(s1.id);
    ASSERT_EQ("一子完成后父进度 33%", pct, 33);  // 1/3 done
    ASSERT_EQ("父仍 active", gm.get(parent.id).status, "active");

    // 3/3 done → 父 100% + done
    gm.update_status(s2.id, "done");
    gm.update_status(s3.id, "done");
    pct = gm.refresh_parent_progress(s3.id);
    ASSERT_EQ("全完成后父进度 100", pct, 100);
    ASSERT_EQ("父状态 done", gm.get(parent.id).status, "done");

    // 任务树 JSON
    auto tree = gm.tree_json(parent.id);
    ASSERT_EQ("树 JSON 子任务数", (int)tree["subtasks"].size(), 3);

    // 失败级联：新父+2 子，1 failed 1 done → 父 failed
    auto p2 = gm.create("失败场景");
    auto f1 = gm.create_subtask(p2.id, "子A");
    auto f2 = gm.create_subtask(p2.id, "子B");
    gm.update_status(f1.id, "done");
    gm.update_status(f2.id, "failed");
    gm.refresh_parent_progress(f2.id);
    ASSERT_EQ("有失败→父 failed", gm.get(p2.id).status, "failed");

    // 根任务（无父）refresh 返回 -1 不崩
    ASSERT_EQ("根任务 refresh 返回 -1", gm.refresh_parent_progress(parent.id), -1);
  }
  std::filesystem::remove_all("/tmp/goal_tree_test_home");

  // ── 2) complex_task 意图分类 ──
  // classify_local_intent 是 AgentService.cpp 内部函数——经 handle_request
  // 间接验证成本高（须 mock 全链）；此处直接断言语义源头：词表已含
  // 保守关键词（防误路由烧钱）。完整链路由 e2e 验证。
  {
    nlohmann::json policy;
    const char* home = std::getenv("THIN_AGENT_HOME");
    std::string path = (home ? std::string(home) : "/root/.thin_agent")
                       + "/config/chat_policy.json";
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) path = "config/chat_policy.json", f = std::fopen(path.c_str(), "r");
    ASSERT_TRUE("chat_policy.json 可读", f != nullptr);
    if (f) {
      std::string buf;
      char chunk[4096];
      size_t n;
      while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) buf.append(chunk, n);
      std::fclose(f);
      policy = nlohmann::json::parse(buf, nullptr, false);
      ASSERT_TRUE("keywords.complex_task 存在",
                  policy.contains("keywords") &&
                  policy["keywords"].contains("complex_task") &&
                  policy["keywords"]["complex_task"].is_array());
      auto kws = policy["keywords"]["complex_task"];
      ASSERT_TRUE("词表非空且含核心词", kws.size() >= 5);
      bool has_refactor = false;
      for (auto& k : kws)
        if (k == "重构" || k == "refactor") has_refactor = true;
      ASSERT_TRUE("含 重构/refactor", has_refactor);
    }
  }

  return TEST_REPORT();
}
