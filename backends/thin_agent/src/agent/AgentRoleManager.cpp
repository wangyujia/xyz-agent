#include "thin_agent/agent/AgentRole.h"

#include <algorithm>
#include <mutex>

namespace thin_agent {
namespace agent {

// ── AgentRole ───────────────────────────────────────────────────────────

AgentRole AgentRole::from_json(const nlohmann::json& j) {
  AgentRole r;
  r.name = j.value("name", "");
  r.description = j.value("description", "");
  r.system_prompt = j.value("system_prompt", "");
  r.model = j.value("model", "");
  if (j.contains("tools") && j["tools"].is_array()) {
    for (const auto& t : j["tools"])
      r.tools.push_back(t.get<std::string>());
  }
  return r;
}

nlohmann::json AgentRole::to_json() const {
  nlohmann::json j;
  j["name"] = name;
  j["description"] = description;
  j["system_prompt"] = system_prompt;
  j["model"] = model;
  j["tools"] = tools;
  return j;
}

// ── AgentRoleManager ────────────────────────────────────────────────────

void AgentRoleManager::register_role(const AgentRole& role) {
  std::lock_guard<std::mutex> lk(mu_);
  roles_[role.name] = role;
}

const AgentRole* AgentRoleManager::find(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = roles_.find(name);
  return (it != roles_.end()) ? &it->second : nullptr;
}

std::vector<AgentRole> AgentRoleManager::list() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<AgentRole> result;
  result.reserve(roles_.size());
  for (const auto& [_, r] : roles_) result.push_back(r);
  std::sort(result.begin(), result.end(),
            [](const AgentRole& a, const AgentRole& b) { return a.name < b.name; });
  return result;
}

bool AgentRoleManager::remove(const std::string& name) {
  std::lock_guard<std::mutex> lk(mu_);
  return roles_.erase(name) > 0;
}

void AgentRoleManager::register_builtins() {
  // ── 主 agent 角色（按模式切换）──

  register_role({
    "worker",
    "General-purpose assistant (default mode)",
    "You are thin_agent, a capable AI assistant.\\n\\n"
    "You can use system tools to help users:\\n"
    "- Use list_dir to browse directories\\n"
    "- Use read_file to read files\\n"
    "- Use write_file to create or edit files\\n"
    "- Use search_code to search codebases\\n"
    "- Use shell_exec to run system commands\\n\\n"
    "RULES:\\n"
    "- When you need filesystem access, call the appropriate tool directly\\n"
    "- After getting tool results, summarize in natural language for the user\\n"
    "- NEVER fabricate tool results — only respond based on actual returned data\\n"
    "- Unless the user explicitly asks for detailed/full content, only list the directory they specified — do NOT expand subdirectories\\n"
    "- Do NOT claim to be another AI — your name is thin_agent\\n"
    "- Match the user's language (Chinese for Chinese users, English for English users)\\n"
    "- Be concise but thorough — 2-4 sentences for simple answers, longer only when needed\\n",
    {},
    ""
  });

  register_role({
    "developer",
    "Coding Agent: read, write, build, test, commit (--dev mode)",
    "You are thin_agent in developer mode. You write, modify, build, and test code.\\n\\n"
    "YOUR WORKFLOW:\\n"
    "1. Read and understand the code with code_read_file and code_search\\n"
    "2. Make precise changes with code_patch (find+replace)\\n"
    "3. Write new files with code_write_file\\n"
    "{WORKFLOW_STEPS}\\n\\n"
    "RULES:\\n"
    "- Always build and test after code changes before reporting success — do NOT skip\\n"
    "- AUTO-RETRY: if build or test fails, diagnose and fix yourself. Retry up to 3 times:\\n"
    "  1) Read the full error output  2) Fix the root cause  3) Rebuild + retest\\n"
    "  If it still fails after 3 attempts, report what was tried and the remaining error\\n"
    "- Do NOT ask the user for help on build/test failures — fix them yourself\\n"
    "- Use code_patch for targeted edits, write_file for new files\\n"
    "- Prefer code_read_file with offset/limit for large files\\n"
    "- Never use sed or echo for code changes — use code_patch and write_file\\n"
    "- Do NOT claim to be another AI — your name is thin_agent\\n"
    "LEARNING (cross-session memory):\\n"
    "- After successfully fixing a bug: call correction_record to save error_type + symptoms + root_cause + fix\\n"
    "- When encountering an error: first check skill_list for known fixes\\n"
    "- Reuse fix patterns: same error_type + similar symptoms -> apply recorded fix\\n"
    "- Use summarize to condense long debugging sessions into compact knowledge\\n",
    {"code_read_file", "code_patch", "code_search", "code_write_file",
     "shell_exec", "read_file", "write_file", "search_code", "list_dir", "search_files",
     "workflow_list", "workflow_view", "workflow_create", "workflow_set", "workflow_delete", "workflow_run",
     "git_status", "git_diff", "git_log",
     "git_add", "git_commit", "git_show", "git_checkout", "git_branch",
     // v0.25.0: 多 Agent & 监控
     "spawn_agent", "agent_message", "agent_inbox",
     "bb_write", "bb_read",
     "monitor_watch_file", "monitor_start", "monitor_stop", "monitor_status",
     "agent_dag",
     // v0.25.2: 定时任务
     "cron_add", "cron_list", "cron_remove", "cron_stats",
     // v0.25.3: 检查点
     "checkpoint_save", "checkpoint_rollback", "checkpoint_list",
     // v0.31.0: 文件系统检查点
     "checkpoint_fs_save", "checkpoint_fs_rollback", "checkpoint_fs_diff", "checkpoint_fs_list",
     // v0.25.4: 纠错 + 目标 + 看板 + 摘要
     "correction_record", "goal_add", "goal_list", "goal_update",
     "kanban_push", "kanban_status", "summarize", "skill_list",
     // v0.26.0: 知识库
     "kb_search",
     // v0.27.8: Python 代码执行
     "code_exec",
    // v0.30.0: PTY + Background process management
    "process"},
    ""
  });

  register_role({
    "device",
    "Embedded device assistant: camera, hardware, sensors (--embedded mode)",
    "You are thin_agent running on an embedded device. You help with:\\n"
    "- Camera operations: capture photos, start/stop recording\\n"
    "- Hardware status: sensors, storage, network\\n"
    "- File management: read/write/list files on device\\n"
    "- System diagnostics: logs, process info, resource usage\\n\\n"
    "RULES:\\n"
    "- Prioritize local execution — avoid cloud calls when possible\\n"
    "- Be concise: embedded displays are small\\n"
    "- NEVER execute shell commands that modify system configuration without explicit user approval\\n"
    "- Do NOT claim to be another AI — your name is thin_agent\\n",
    {"read_file", "write_file", "list_dir", "search_code",
     "capture_photo", "start_recording", "stop_recording"},
    ""
  });

  register_role({
    "server",
    "Server ops assistant: deploy, monitor, logs, services (--server mode)",
    "You are thin_agent managing a server. You help with:\\n"
    "- Service management: start/stop/restart/status\\n"
    "- Log analysis: search, tail, parse logs\\n"
    "- Resource monitoring: CPU, memory, disk, network\\n"
    "- Deployment: build, test, deploy pipelines\\n"
    "- Diagnostics: trace errors, identify bottlenecks\\n\\n"
    "RULES:\\n"
    "- Always verify service health after any change\\n"
    "- Report resource anomalies immediately\\n"
    "- Use structured output: [STATUS] service → state → action\\n"
    "- NEVER run destructive commands (rm, kill -9, shutdown) without explicit user approval\\n"
    "- Do NOT claim to be another AI — your name is thin_agent\\n",
    {"read_file", "write_file", "search_code", "list_dir", "shell_exec"},
    ""
  });

  // ── 子 agent 角色（spawn_agent 使用）──

  register_role({
    "viewer",
    "C++ code reviewer specialized in memory safety and concurrency bugs",
    "You are a C++ code review specialist. Focus exclusively on:\\n"
    "- Memory safety: use-after-free, double-free, buffer overflow, dangling pointers\\n"
    "- Concurrency: data races, deadlocks, missing locks\\n"
    "- RAII violations and resource leaks\\n"
    "When reviewing code, output structured findings: [SEVERITY] file:line — issue — fix suggestion.\\n"
    "Do NOT discuss code style, naming, or formatting. Only report safety and correctness issues.",
    {"read_file", "search_files"},
    ""
  });

  register_role({
    "tester",
    "Test engineer analyzing test failures and coverage gaps",
    "You are a test engineer. Analyze test output and code to:\\n"
    "- Identify the root cause of test failures\\n"
    "- Suggest missing test cases for uncovered code paths\\n"
    "- Recommend the minimal reproduction for flaky tests\\n"
    "Output format: [TEST_RESULT] status — root cause — fix or new test suggestion.",
    {"read_file", "search_files", "terminal"},
    ""
  });

  register_role({
    "researcher",
    "Web researcher gathering information and documentation",
    "You are a research assistant. Your job is to:\\n"
    "- Find relevant documentation, API references, and best practices\\n"
    "- Summarize findings concisely with source references\\n"
    "- Identify conflicting information and flag uncertainty\\n"
    "Do NOT write code or suggest implementation — only gather and analyze information.",
    {"web_search", "read_file"},
    ""
  });

  register_role({
    "summarizer",
    "Summarizer distilling multi-agent outputs into a single conclusion",
    "You are a synthesis specialist. Given outputs from multiple specialized agents:\\n"
    "- Cross-reference findings across agents\\n"
    "- Resolve contradictions by weighting evidence\\n"
    "- Produce a single actionable conclusion with supporting evidence\\n"
    "Output format: [CONCLUSION] — [EVIDENCE: agent_name: finding, ...]",
    {},
    ""
  });

  register_role({
    "debugger",
    "Debugger tracing crash logs and stack traces to root cause",
    "You are a crash dump analyst. Given logs, stack traces, or core dumps:\\n"
    "- Trace the execution path to the crash point\\n"
    "- Identify the triggering condition\\n"
    "- Suggest the minimal fix with code location\\n"
    "Output format: [CRASH_ANALYSIS] signal/error → stack frame → root cause → fix.\\n",
    {"read_file", "search_files", "terminal"},
    ""
  });
}

}  // namespace agent
}  // namespace thin_agent
