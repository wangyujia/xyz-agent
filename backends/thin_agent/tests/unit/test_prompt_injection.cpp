// unit_prompt_injection: 主FC路径注入链测试 (v0.38.0 ~ v0.38.2)
//
// 验证所有注入组件的 to_prompt_injection() 输出可以直接拼入 system prompt，
// 模拟主 FC 路径的真实注入链路：
//   语义记忆 → 技能自进化 → 长期目标 → 纠错学习
//
// 这些组件的 API 已在 unit_agent_core 中有基本覆盖，
// 本测试聚焦"注入到 system prompt 后的完整闭环正确性"。

#include <iostream>
#include <string>
#include <vector>
#include <cstdio>

#include "thin_agent/agent/MemoryManager.h"
#include "thin_agent/agent/SkillManager.h"
#include "thin_agent/agent/GoalManager.h"
#include "thin_agent/agent/ErrorCorrectionStore.h"
#include "thin_agent/agent/EmbeddingProvider.h"
#include "thin_agent/agent/VectorStore.h"

using namespace thin_agent;
using namespace thin_agent::agent;

// ── 测试框架 ──────────────────────────────────────────────────────
namespace {
int failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
  }
}

void check_str_contains(const std::string& haystack,
                        const std::string& needle, const char* msg) {
  if (haystack.find(needle) == std::string::npos) {
    std::cerr << "FAIL: " << msg << " — 期望包含 \"" << needle
              << "\"，实际: \"" << haystack.substr(0, 200) << "\"\n";
    ++failures;
  }
}

std::string make_temp_db(const std::string& suffix) {
  auto t = std::chrono::steady_clock::now().time_since_epoch();
  long n = std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
  return "/tmp/test_prompt_inj_" + suffix + "_" + std::to_string(n) + ".db";
}

}  // namespace

// ════════════════════════════════════════════════════════════════════
// 模拟主 FC 路径 system_prompt 构建（v0.38.0 ~ v0.38.2 全注入链）
// ════════════════════════════════════════════════════════════════════

void test_full_injection_chain() {
  // 初始化各组件（模拟 AgentService 构造函数）
  std::string mem_db = make_temp_db("mem");
  std::string skill_db = make_temp_db("skill");
  std::string goal_db = make_temp_db("goal");
  std::string corr_db = make_temp_db("corr");

  auto emb = std::make_shared<LocalHashEmbeddingProvider>(256);
  MemoryManager mem_mgr(emb, mem_db, false);

  auto skill_store = std::make_shared<VectorStore>();
  skill_store->init(skill_db, emb->dimension());
  SkillManager skill_mgr(emb, skill_store);

  GoalManager goal_mgr(goal_db);
  ErrorCorrectionStore corr_store(corr_db);

  // 准备数据（模拟 Agent 运行后累积的知识）
  // 1. 语义记忆
  mem_mgr.remember("项目使用 CMake 构建，GCC 13 编译", "fact", "s1", 0.9f);

  // 2. 技能
  skill_mgr.save_skill("t1", "编译C++项目", "cmake → make",
                       "Tools: shell_exec → shell_exec", {"cmake", "make"});

  // 3. 长期目标
  goal_mgr.create("完成 thin_agent v0.39 发布");

  // 4. 纠错
  corr_store.record("shell_exec", "command not found: cmake", "先安装 cmake: apt install cmake");
  corr_store.record("shell_exec", "command not found: cmake", "先安装 cmake: apt install cmake");

  // 模拟主 FC 路径 system_prompt 构建
  std::string system_prompt = "你是 thin_agent，用中文回复，简洁专业。";
  std::string user_query = "编译C++项目";

  // v0.38.0: 语义记忆注入
  {
    std::string mem_ctx = mem_mgr.build_context(user_query, 3);
    if (!mem_ctx.empty()) {
      system_prompt += "\n\n" + mem_ctx;
    }
  }

  // v0.38.1: 技能注入
  {
    auto matched = skill_mgr.match_skills(user_query, 3, 0.0f);
    std::string skill_inj = SkillManager::to_prompt_injection(matched);
    if (!skill_inj.empty()) {
      system_prompt += "\n\n" + skill_inj;
    }
  }

  // v0.38.2: 目标注入
  {
    std::string goal_ctx = goal_mgr.to_prompt_injection();
    if (!goal_ctx.empty()) {
      system_prompt += "\n\n" + goal_ctx;
    }
  }

  // v0.38.2: 纠错注入
  {
    std::vector<std::string> known_tools = {"shell_exec", "read_file", "write_file"};
    std::string corr_ctx = corr_store.to_prompt_injection(known_tools, 3);
    if (!corr_ctx.empty()) {
      system_prompt += "\n\n" + corr_ctx;
    }
  }

  // ── 验证完整 system_prompt ──
  check(!system_prompt.empty(), "chain: system_prompt 非空");

  // 基础提示保留
  check_str_contains(system_prompt, "thin_agent", "chain: 基础提示保留");

  // 各组件注入检查（可能因 LocalHash 相似度阈值而部分缺失，
  // 但 goal 和 correction 注入不依赖语义匹配，必然存在）
  check_str_contains(system_prompt, "v0.39", "chain: 目标注入成功");
  check_str_contains(system_prompt, "cmake", "chain: 纠错注入成功");

  // 输出完整 prompt 供人工审查
  std::cout << "=== Full system_prompt ===" << std::endl;
  std::cout << system_prompt.substr(0, 1000) << std::endl;
  std::cout << "=== Total length: " << system_prompt.size() << " chars ===" << std::endl;

  // 清理
  std::remove(mem_db.c_str());
  std::remove(skill_db.c_str());
  std::remove(goal_db.c_str());
  std::remove(corr_db.c_str());
}

// ════════════════════════════════════════════════════════════════════
// 空状态注入 — 各组件在无数据时不应污染 system prompt
// ════════════════════════════════════════════════════════════════════

void test_empty_state_injection() {
  std::string mem_db = make_temp_db("mem_e");
  std::string skill_db = make_temp_db("skill_e");
  std::string goal_db = make_temp_db("goal_e");
  std::string corr_db = make_temp_db("corr_e");

  auto emb = std::make_shared<LocalHashEmbeddingProvider>(256);
  MemoryManager mem_mgr(emb, mem_db, false);

  auto skill_store = std::make_shared<VectorStore>();
  skill_store->init(skill_db, emb->dimension());
  SkillManager skill_mgr(emb, skill_store);

  GoalManager goal_mgr(goal_db);
  ErrorCorrectionStore corr_store(corr_db);

  // 空状态：所有注入应返回空字符串
  std::string mem_ctx = mem_mgr.build_context("test", 3);
  check(mem_ctx.empty(), "empty: 语义记忆空返回空");

  auto matched = skill_mgr.match_skills("test", 3, 0.5f);
  std::string skill_inj = SkillManager::to_prompt_injection(matched);
  check(skill_inj.empty(), "empty: 技能空返回空");

  // goal: 没有目标时 to_prompt_injection 行为
  // (GoalManager 可能返回空或模板文字)
  auto goals = goal_mgr.list();
  check(goals.empty(), "empty: 目标列表为空");

  // correction: 没有纠错时
  auto corr_inj = corr_store.to_prompt_injection({"shell_exec"}, 3);
  // 空库时 use_count < 2 不会注入
  check(corr_inj.empty(), "empty: 纠错空返回空");

  std::remove(mem_db.c_str());
  std::remove(skill_db.c_str());
  std::remove(goal_db.c_str());
  std::remove(corr_db.c_str());
}

// ════════════════════════════════════════════════════════════════════
// GoalManager to_prompt_injection 独立验证
// ════════════════════════════════════════════════════════════════════

void test_goal_injection_format() {
  std::string db = make_temp_db("goal_fmt");
  GoalManager mgr(db);

  auto g1 = mgr.create("实现 Vision 模块");
  auto g2 = mgr.create("修复编译警告");

  std::string inj = mgr.to_prompt_injection();
  check(!inj.empty(), "goal_fmt: 注入非空");
  check_str_contains(inj, "Vision", "goal_fmt: 含目标 1");
  check_str_contains(inj, "编译警告", "goal_fmt: 含目标 2");

  // 完成 g1 后验证 g2 仍然注入
  mgr.update_status(g1.id, "done", 100);
  std::string inj2 = mgr.to_prompt_injection();
  check_str_contains(inj2, "编译警告", "goal_fmt: 完成g1后g2仍在注入");
  // g1 不应再出现
  check(inj2.find("Vision") == std::string::npos, "goal_fmt: 已完成目标不再注入");

  std::remove(db.c_str());
}

// ════════════════════════════════════════════════════════════════════
// ErrorCorrectionStore to_prompt_injection 独立验证
// ════════════════════════════════════════════════════════════════════

void test_correction_injection_format() {
  std::string db = make_temp_db("corr_fmt");
  ErrorCorrectionStore store(db);

  // 需要 use_count >= 2 才会被注入（record 是 upsert）
  store.record("shell_exec", "permission denied", "添加 sudo 前缀");
  store.record("shell_exec", "permission denied", "添加 sudo 前缀");
  store.record("code_patch", "string not found", "检查缩进和空白");

  auto inj = store.to_prompt_injection({"shell_exec", "code_patch"}, 5);

  check(!inj.empty(), "corr_fmt: 高频纠错注入非空");
  check_str_contains(inj, "shell_exec", "corr_fmt: 含工具名");

  // 不在 known_tools 列表的工具不应注入
  auto inj_filtered = store.to_prompt_injection({"read_file"}, 5);
  check(inj_filtered.empty(), "corr_fmt: 过滤未知工具后返回空");

  std::remove(db.c_str());
}

// ════════════════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════════════════

int main() {
  test_full_injection_chain();
  test_empty_state_injection();
  test_goal_injection_format();
  test_correction_injection_format();

  if (failures == 0) {
    std::cout << "ALL unit_prompt_injection PASSED" << std::endl;
    return 0;
  }
  std::cerr << failures << " test(s) FAILED" << std::endl;
  return 1;
}
