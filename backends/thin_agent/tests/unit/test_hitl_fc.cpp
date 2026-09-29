/// test_hitl_fc.cpp — v0.49.0 HITL 统一风险分级测试
///
/// 测试覆盖：
/// 1. tool_risk_of 分级表（Safe/Mutating/Dangerous）
/// 2. ProjectContext 对写文件族风险的影响（rw=Mutating, 无ctx=Dangerous）
/// 3. tool_risk_str 转换
/// 4. CloudChatResult needs_approval 字段默认值

#include "thin_agent/core/PathValidator.h"
#include "thin_agent/llm/CloudLlmClient.h"

#include <iostream>
#include <string>

using namespace thin_agent;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
  if (cond) { ++g_pass; } \
  else { ++g_fail; std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl; } \
} while(0)

// ═══════════════════════════════════════════════════
// 1. 风险分级表
// ═══════════════════════════════════════════════════

void test_risk_table() {
  std::cerr << "\n=== test_risk_table ===" << std::endl;

  // Safe 工具
  CHECK(tool_risk_of("read_file") == ToolRisk::Safe, "read_file is Safe");
  CHECK(tool_risk_of("list_dir") == ToolRisk::Safe, "list_dir is Safe");
  CHECK(tool_risk_of("code_read_file") == ToolRisk::Safe, "code_read_file is Safe");
  CHECK(tool_risk_of("search_code") == ToolRisk::Safe, "search_code is Safe");
  CHECK(tool_risk_of("kb_search") == ToolRisk::Safe, "kb_search is Safe");
  CHECK(tool_risk_of("status") == ToolRisk::Safe, "status is Safe");
  CHECK(tool_risk_of("ping") == ToolRisk::Safe, "ping is Safe");

  // Dangerous 工具
  CHECK(tool_risk_of("shell_exec") == ToolRisk::Dangerous, "shell_exec is Dangerous");
  CHECK(tool_risk_of("execute_code") == ToolRisk::Dangerous, "execute_code is Dangerous");
  CHECK(tool_risk_of("git_reset") == ToolRisk::Dangerous, "git_reset is Dangerous");
  CHECK(tool_risk_of("git_push") == ToolRisk::Dangerous, "git_push is Dangerous");
  CHECK(tool_risk_of("delete_file") == ToolRisk::Dangerous, "delete_file is Dangerous");
  CHECK(tool_risk_of("rm") == ToolRisk::Dangerous, "rm is Dangerous");

  // Mutating（默认兜底）
  CHECK(tool_risk_of("capture_photo") == ToolRisk::Mutating, "capture_photo is Mutating");
  CHECK(tool_risk_of("git_commit") == ToolRisk::Mutating, "git_commit is Mutating");
  CHECK(tool_risk_of("some_unknown_tool") == ToolRisk::Mutating, "unknown tool defaults to Mutating");
}

// ═══════════════════════════════════════════════════
// 2. ProjectContext 影响
// ═══════════════════════════════════════════════════

void test_project_context_risk() {
  std::cerr << "\n=== test_project_context_risk ===" << std::endl;

  // 无上下文 → write_file 族是 Dangerous
  ProjectContextTLS::clear();
  CHECK(tool_risk_of("write_file") == ToolRisk::Dangerous,
        "write_file without project ctx is Dangerous");
  CHECK(tool_risk_of("code_write_file") == ToolRisk::Dangerous,
        "code_write_file without project ctx is Dangerous");
  CHECK(tool_risk_of("code_patch") == ToolRisk::Dangerous,
        "code_patch without project ctx is Dangerous");

  // ro 模式 → 仍然 Dangerous（写不进去，但如果有漏洞风险高）
  PathValidator::ProjectContext ctx;
  ctx.project_root = "/tmp/test_hitl_proj";
  ctx.read_only = true;
  ctx.valid = true;
  ProjectContextTLS::set(ctx);
  CHECK(tool_risk_of("write_file") == ToolRisk::Dangerous,
        "write_file in ro project is Dangerous");

  // rw 模式 → Mutating（受白名单约束，不需 HITL）
  ctx.read_only = false;
  ProjectContextTLS::set(ctx);
  CHECK(tool_risk_of("write_file") == ToolRisk::Mutating,
        "write_file in rw project is Mutating");
  CHECK(tool_risk_of("code_patch") == ToolRisk::Mutating,
        "code_patch in rw project is Mutating");

  // shell_exec 无论何种上下文都是 Dangerous
  CHECK(tool_risk_of("shell_exec") == ToolRisk::Dangerous,
        "shell_exec always Dangerous");

  ProjectContextTLS::clear();
}

// ═══════════════════════════════════════════════════
// 3. risk_str
// ═══════════════════════════════════════════════════

void test_risk_str() {
  std::cerr << "\n=== test_risk_str ===" << std::endl;

  CHECK(std::string(tool_risk_str(ToolRisk::Safe)) == "safe", "Safe str");
  CHECK(std::string(tool_risk_str(ToolRisk::Mutating)) == "mutating", "Mutating str");
  CHECK(std::string(tool_risk_str(ToolRisk::Dangerous)) == "dangerous", "Dangerous str");
}

// ═══════════════════════════════════════════════════
// 4. CloudChatResult HITL 字段
// ═══════════════════════════════════════════════════

void test_chat_result_hitl_fields() {
  std::cerr << "\n=== test_chat_result_hitl_fields ===" << std::endl;

  CloudChatResult r;
  CHECK(!r.needs_approval, "default needs_approval is false");
  CHECK(r.pending_tool_name.empty(), "default pending_tool_name empty");
  CHECK(r.pending_tool_args.empty(), "default pending_tool_args empty");

  r.needs_approval = true;
  r.pending_tool_name = "shell_exec";
  r.pending_tool_args = "{\"command\":\"ls\"}";
  CHECK(r.needs_approval && r.pending_tool_name == "shell_exec",
        "HITL fields settable");
}

int main() {
  test_risk_table();
  test_project_context_risk();
  test_risk_str();
  test_chat_result_hitl_fields();

  std::cerr << "\n══════════════════════════════════════" << std::endl;
  std::cerr << "HITL: " << g_pass << " passed, " << g_fail << " failed" << std::endl;
  std::cerr << "══════════════════════════════════════" << std::endl;

  return g_fail > 0 ? 1 : 0;
}
