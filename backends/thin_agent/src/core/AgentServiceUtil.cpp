#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/HookSystem.h"  // v0.52.29
#include "thin_agent/core/JsonCoerce.h"
#include "thin_agent/core/JsonExtract.h"

#include "thin_agent/Version.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <future>
#include <map>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <deque>
#include <cctype>
#include <regex>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <sstream>
#include <unordered_map>

#include "thin_agent/llm/ProviderFactory.h"
#include "thin_agent/llm/CloudLlmClient.h"
#include "thin_agent/llm/CurlHttpClient.h"
#include "thin_agent/core/CredentialPool.h"
#include "thin_agent/core/Utf8Util.h"
#include "thin_agent/core/ExternalInfoClient.h"
#include "thin_agent/core/ExternalIntentHandlers.h"
#include "thin_agent/core/PathValidator.h"
#include "thin_agent/core/DialogSlotRecall.h"
#include "thin_agent/core/IntentOnnx.h"
#include "thin_agent/core/IntentScorer.h"
#include "thin_agent/core/ChatPolicy.h"
#include "thin_agent/core/WorkflowManager.h"
#include "thin_agent/core/MediaPlan.h"
#include "thin_agent/RuntimePaths.h"
#include "thin_agent/local/HybridRouter.h"
#include "thin_agent/local/TemplateModel.h"
#include "thin_agent/local/GgufModel.h"
#include "thin_agent/local/ModelPool.h"
#include "thin_agent/agent/AgentLoop.h"
#include "thin_agent/agent/ToolCallContext.h"
#include "thin_agent/agent/AgentTracer.h"
#include "thin_agent/agent/EmbeddingProvider.h"
#include "thin_agent/agent/ToolRegistry.h"
#include "thin_agent/agent/AgentRole.h"
#include "thin_agent/plugin/PluginLoader.h"
#include "thin_agent/Version.h"
#include "thin_agent/agent/TokenBudget.h"
#include "thin_agent/agent/ConvergenceTracker.h"
#include "thin_agent/agent/McpClient.h"
#include "thin_agent/agent/StdioTransport.h"
#include "thin_agent/agent/HttpTransport.h"
#include "thin_agent/core/PseudoTerminal.h"
#include "thin_agent/core/BackgroundProcessManager.h"
#include "thin_agent/core/CommandValidator.h"
#include "thin_agent/core/SandboxExecutor.h"
#include "thin_agent/log/LogEvent.h"

// v0.42.3: cron 任务过滤上下文（thread_local，消除数据竞争）
// （非 static：单测 test_cron_unattended_hitl 需 extern 置位模拟 cron 线程）

namespace thin_agent { namespace svc_util {

// v0.42.3: cron 任务过滤上下文（thread_local，消除数据竞争）
// （非 static：单测 test_cron_unattended_hitl 需 extern 置位模拟 cron 线程）



std::string resolve_api_key(CredentialPool* pool,
                            const std::string& api_key_env,
                            const std::string& provider) {
  if (pool) {
    auto k = pool->get(provider, "api_key");
    if (!k.empty()) return k;
  }
  if (!api_key_env.empty()) {
    const char* v = std::getenv(api_key_env.c_str());
    if (v && *v) return v;
  }
  return "";
}

thread_local nlohmann::json g_cron_filter;
thread_local bool g_fc_dangerous_denied = false;
std::atomic<int> g_spawn_nest_depth{0};
std::atomic<int> g_active_sub_agents{0};


/// 自动生成 trace_id。
std::string next_auto_trace(uint64_t seq) { return "trace-" + std::to_string(seq); }
/// 自动生成 cmd_id。
std::string next_auto_cmd(uint64_t seq) { return "cmd-" + std::to_string(seq); }

/// 转小写副本（ASCII）。
std::string to_lower_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

/// 判断 haystack 是否包含 needles 中任一非空子串。
bool contains_any(const std::string& haystack, const std::vector<std::string>& needles) {
  for (const auto& n : needles) {
    if (!n.empty() && haystack.find(n) != std::string::npos) return true;
  }
  return false;
}

/// 按点号拆分 JSON 点分路径（如 "style.default"）。
std::vector<std::string> split_dot_path(const std::string& path) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : path) {
    if (ch == '.') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(ch);
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

/// 按点分路径访问 JSON 子节点；路径不存在时返回 nullptr。
const nlohmann::json* json_at_path(const nlohmann::json& root, const std::string& path) {
  const nlohmann::json* cur = &root;
  for (const auto& key : split_dot_path(path)) {
    if (!cur->is_object() || !cur->contains(key)) return nullptr;
    cur = &((*cur)[key]);
  }
  return cur;
}

/// 使用 ChatPolicy 关键词表判断 haystack 是否命中 keywords.<key> 任一词。
bool contains_any_policy(const std::string& haystack, const std::string& key) {
  return contains_any(haystack, policy_keywords(key));
}

/// 去除首尾空白字符。
std::string trim_copy(std::string s) {
  const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

/// 从首个命中关键词之后截取查询尾句，并剥离 query_filler_prefixes。
std::string extract_query_after_keywords(const std::string& text,
                                         const std::vector<std::string>& keywords) {
  std::string tail;
  for (const auto& kw : keywords) {
    if (kw.empty()) continue;
    const auto pos = text.find(kw);
    if (pos != std::string::npos) {
      tail = text.substr(pos + kw.size());
      break;
    }
  }
  if (tail.empty()) return "";
  tail = trim_copy(std::move(tail));

  bool changed = true;
  while (changed && !tail.empty()) {
    changed = false;
    for (const auto& f : query_filler_prefixes()) {
      if (f.empty()) continue;
      if (tail.rfind(f, 0) == 0) {
        tail = trim_copy(tail.substr(f.size()));
        changed = true;
        break;
      }
    }
  }
  return trim_copy(std::move(tail));
}

/// 判断字符串是否为纯无符号十进制整数。
bool is_unsigned_integer_token(const std::string& s) {
  if (s.empty()) return false;
  for (unsigned char ch : s) {
    if (!std::isdigit(ch)) return false;
  }
  return true;
}

/// 解析「查询文本 [limit]」：尾词为数字时作为 limit，否则使用 default_limit。
std::pair<std::string, int> parse_query_and_limit(std::string tail, int default_limit = 20) {
  tail = trim_copy(std::move(tail));
  int limit = default_limit;
  if (tail.empty()) return {"", limit};

  const auto last_space = tail.find_last_of(" \t\r\n");
  if (last_space != std::string::npos) {
    const std::string last_tok = trim_copy(tail.substr(last_space + 1));
    if (is_unsigned_integer_token(last_tok)) {
      try {
        limit = std::stoi(last_tok);
      } catch (...) {
        limit = default_limit;
      }
      tail = trim_copy(tail.substr(0, last_space));
    }
  }

  if (limit <= 0) limit = default_limit;
  if (limit > 200) limit = 200;
  return {tail, limit};
}

/// 读取 style.default，缺省为 normal / 传入 fallback。
std::string policy_style_default(const std::string& fallback = "normal") {
  const auto& p = chat_policy();
  const auto* node = json_at_path(p, "style.default");
  if (node && node->is_string()) return node->get<std::string>();
  return fallback;
}

/// 读取 style.<style>.<scope>.<key> 前缀/后缀文案。
std::string policy_style_text(const std::string& style,
                              const std::string& key,
                              const std::string& fallback = "") {
  const auto& p = chat_policy();
  const auto* node = json_at_path(p, "style." + style + "." + key);
  if (node && node->is_string()) return node->get<std::string>();
  return fallback;
}

/// 为本地回复拼接 style 配置中的前缀（profile/local/cloud 等 scope）。
std::string apply_style_prefix(const std::string& scope,
                               const std::string& key,
                               const std::string& body,
                               const std::string& fallback_style = "normal") {
  const std::string style_name = policy_style_default(fallback_style);
  const std::string prefix = policy_style_text(style_name, scope + "." + key, "");
  if (prefix.empty()) return body;
  return prefix + body;
}

/// 判断是否为技能/工具枚举类查询（只列技能，不含自我介绍和工作流）。
/// 匹配："目前有哪些技能"、"技能有哪些"、"有什么功能"、"list your tools" 等。
bool is_skills_only_query(const std::string& text) {
  const std::string norm = normalize_text_for_policy(text);
  // 排除"你是谁"等身份类查询
  if (norm.find("你是谁") != std::string::npos ||
      norm.find("who are you") != std::string::npos ||
      norm.find("what are you") != std::string::npos) {
    return false;
  }
  // 技能/工具/功能 关键词
  static const std::vector<std::string> kSkillWords = {
    "技能", "skill", "工具", "tool", "功能", "能力", "capability",
    "可以做什么", "能做什么", "会做什么",
  };
  bool has_skill_word = false;
  for (const auto& w : kSkillWords) {
    if (norm.find(w) != std::string::npos) { has_skill_word = true; break; }
  }
  if (!has_skill_word) return false;
  // 必须有"有哪些/什么/哪些/列出/list/show/what"等提问词
  static const std::vector<std::string> kQueryWords = {
    "有哪些", "什么", "哪些", "列出", "list", "show", "what",
    "目前有", "现在有", "当前有", "available",
  };
  for (const auto& w : kQueryWords) {
    if (norm.find(w) != std::string::npos) return true;
  }
  return false;
}

/// 按当前角色动态生成自我介绍文本（detailed 模式显示完整能力清单）
std::string build_profile_body(const std::string& lang,
                                      const agent::AgentRoleManager* role_mgr,
                                      const SkillRegistry& skill_registry,
                                      bool detailed = false,
                                      bool skills_only = false) {
  const bool is_dev = (std::getenv("THIN_AGENT_DEV_MODE") != nullptr &&
                       std::string(std::getenv("THIN_AGENT_DEV_MODE")) == "1");
  // TODO: 未来通过 --embedded / --server 参数设置对应环境变量
  const bool is_embedded = false;
  const bool is_server = false;
  std::string role_name = is_dev ? "developer"
                        : is_embedded ? "device"
                        : is_server ? "server"
                        : "worker";
  const agent::AgentRole* role = role_mgr ? role_mgr->find(role_name) : nullptr;

  if (role && !role->system_prompt.empty()) {
    const bool is_zh = (lang == "zh");

    // 角色 → 显示名映射
    static const std::unordered_map<std::string, std::pair<std::string, std::string>> kRoleNames = {
      // 主 agent 角色
      {"worker",    {"工作智能体",     "Work Assistant"}},
      {"developer", {"编码智能体",     "Coding Agent"}},
      {"device",    {"智能硬件智能体", "Embedded Device Assistant"}},
      {"server",    {"服务器智能体",   "Server Assistant"}},
      // 子 agent 角色
      {"viewer",     {"代码审查员",     "Code Reviewer"}},
      {"tester",     {"测试工程师",     "Test Engineer"}},
      {"researcher", {"研究员",        "Researcher"}},
      {"summarizer", {"总结者",        "Summarizer"}},
      {"debugger",   {"调试器",        "Debugger"}},
    };
    auto it = kRoleNames.find(role_name);
    std::string display_zh = (it != kRoleNames.end()) ? it->second.first  : "thin_agent";
    std::string display_en = (it != kRoleNames.end()) ? it->second.second : "thin_agent";

    std::string body;
    if (!skills_only) {
    if (is_zh) {
      body = "你好！我是 " + display_zh + "。\n\n";
      if (detailed) body += "详细能力清单：\n\n";

      if (role_name == "developer") {
        body += "我可以帮你编写、修改、构建和测试代码。\n"
                "工作流：\n"
                "1. 用 code_read_file / code_search 阅读代码\n"
                "2. 用 code_patch 精准修改\n"
                "3. 用 code_write_file 创建新文件\n"
                "{WORKFLOW_STEPS}\n";
      } else if (role_name == "device") {
        body += "我运行在嵌入式设备上，帮你管理硬件和传感器。\n"
                "核心能力：\n"
                "- 摄像头：拍照 / 录像\n"
                "- 硬件状态：传感器、存储、网络\n"
                "- 文件管理：读写设备文件\n"
                "- 系统诊断：日志、进程、资源\n";
      } else if (role_name == "server") {
        body += "我管理服务器，帮你运维和监控。\n"
                "核心能力：\n"
                "- 服务管理：启动 / 停止 / 重启 / 状态\n"
                "- 日志分析：搜索、查看、解析日志\n"
                "- 资源监控：CPU、内存、磁盘、网络\n"
                "- 部署流水线：构建、测试、发布\n"
                "- 诊断排错：追踪错误、定位瓶颈\n";
      } else {
        body += "我是你的 AI 助手，可以用系统工具帮你处理文件和命令。\n";
      }
      body += "\n";
    } else {
      body = "Hello! I'm " + display_en + ".\n\n";
      if (detailed) body += "Detailed capabilities:\n\n";

      if (role_name == "developer") {
        body += "I can write, modify, build, and test code.\n"
                "Workflow:\n"
                "1. Read code with code_read_file / code_search\n"
                "2. Edit precisely with code_patch\n"
                "3. Create files with code_write_file\n"
                "{WORKFLOW_STEPS}\n";
      } else if (role_name == "device") {
        body += "I run on an embedded device. I help with:\n"
                "- Camera: capture photos, record video\n"
                "- Hardware status: sensors, storage, network\n"
                "- File management: read/write device files\n"
                "- System diagnostics: logs, processes, resources\n";
      } else if (role_name == "server") {
        body += "I manage servers. I help with:\n"
                "- Service management: start/stop/restart/status\n"
                "- Log analysis: search, tail, parse logs\n"
                "- Resource monitoring: CPU, memory, disk, network\n"
                "- Deployment: build, test, release pipelines\n"
                "- Diagnostics: trace errors, identify bottlenecks\n";
      } else {
        body += "I'm an AI assistant that can help you with files and commands.\\n";
      }
      body += "\\n";
    }

    // 注入当前工作流（dev 模式下替换默认构建步骤）
    if (role_name == "developer") {
      auto wf = WorkflowManager::instance().current();
      body = is_zh
        ? WorkflowManager::instance().inject_into_profile_zh(body, wf)
        : WorkflowManager::instance().inject_into_profile_en(body, wf);
    }
    }  // if (!skills_only)

    // 列出角色允许的工具（带描述）
    const auto& role_tools = role->tools;
    if (!role_tools.empty()) {
      // 工具名 → {中文描述, 英文描述}
      static const std::unordered_map<std::string, std::pair<std::string, std::string>> kToolDescs = {
        {"code_read_file",  {"读取代码文件（行号+分页）",         "read code files (line numbers + pagination)"}},
        {"code_patch",      {"精准查找替换修改",                  "precise find-and-replace edits"}},
        {"code_search",     {"搜索代码内容（支持文件过滤）",          "search code with file filtering"}},
        {"code_write_file", {"创建/覆写代码文件（自动建目录）",         "create/overwrite code files (auto mkdir)"}},
        {"shell_exec",      {"执行Shell命令（隔离沙箱；宿主持久文件在 /host_tmp，write_file 写的 /tmp/xxx 对应沙箱 /host_tmp/xxx；沙箱内 /tmp 每次全新不保留）", "execute shell commands (isolated sandbox; host files under /host_tmp, write_file /tmp/xxx == sandbox /host_tmp/xxx; sandbox /tmp is fresh per call)"}},
        {"read_file",       {"读取文件内容",                      "read file contents"}},
        {"write_file",      {"写入文件（自动创建父目录，持久化）", "write files (auto-create parent dir, persistent)"}},
        {"search_code",     {"grep 搜索文件内容",                  "grep search file contents"}},
        {"list_dir",        {"列出目录内容",                      "list directory contents"}},
        {"search_files",    {"搜索文件（按名称/glob）",            "search files by name/glob"}},
        {"capture_photo",   {"拍摄照片",                         "capture photos"}},
        {"start_recording", {"开始录像",                         "start recording"}},
        {"stop_recording",  {"停止录像",                         "stop recording"}},
        {"workflow_list",   {"列出所有工作流",                     "list all workflows"}},
        {"workflow_view",   {"查看工作流详情",                     "view workflow details"}},
        {"workflow_create", {"创建工作流",                        "create a workflow"}},
        {"workflow_set",    {"切换当前工作流",                     "switch to a workflow"}},
        {"workflow_delete", {"删除工作流",                        "delete a workflow"}},
        {"workflow_run",    {"执行工作流(含自动重试)",              "execute workflow (auto-retry on failure)"}},
        {"git_status",      {"查看Git仓库状态",                  "show git working tree status"}},
        {"git_diff",        {"查看Git差异",                     "show git diff"}},
        {"git_log",         {"查看Git提交历史",                  "show git commit log"}},
        {"git_add",         {"暂存文件到Git暂存区",               "stage files for commit"}},
        {"git_commit",      {"提交Git改动",                    "commit staged changes"}},
    {"git_show",        {"查看提交详情",                    "show commit diff"}},
    {"git_checkout",    {"切换分支/恢复文件",                "checkout branch or restore file"}},
    {"git_branch",      {"列出分支",                       "list branches"}},
    // ── v0.25.0: 多 Agent & 监控工具 ──
    {"spawn_agent",    {"派生子Agent执行任务（指定角色）",       "spawn a sub-agent with a role to run a task"}},
    {"agent_message",  {"向其他Agent发送消息",                 "send message to another agent via bus"}},
    {"agent_inbox",    {"查看Agent收件箱",                     "peek an agent's incoming messages"}},
    {"bb_write",       {"写入共享黑板",                        "write to the shared blackboard"}},
    {"bb_read",        {"读取共享黑板",                        "read from the shared blackboard"}},
    {"monitor_watch_file", {"监控文件变化（匹配错误模式）",          "watch a file for changes (error pattern matching)"}},
    {"monitor_start",  {"启动后台监控线程",                     "start the background monitor thread"}},
    {"monitor_stop",   {"停止后台监控",                        "stop the background monitor"}},
    {"monitor_status", {"查看监控状态",                        "get monitor status and recent alerts"}},
    {"agent_dag",      {"执行DAG依赖编排（并行Agent任务）",        "execute DAG with dependency-ordered parallel agent tasks"}},
    // ── v0.25.2: 定时任务 ──
    {"cron_add",       {"添加定时任务",                       "add a scheduled cron task"}},
    {"cron_list",      {"列出所有定时任务",                     "list all cron tasks"}},
    {"cron_remove",    {"删除定时任务",                       "remove a cron task by id"}},
    {"cron_stats",     {"查看调度器统计",                      "show cron scheduler stats"}},
    // ── v0.25.3: 检查点 ──
    {"checkpoint_save",   {"保存会话快照",                      "save a session checkpoint snapshot"}},
    {"checkpoint_rollback", {"恢复到最近快照",                    "rollback to the last checkpoint"}},
    {"checkpoint_list",   {"列出所有快照",                      "list all checkpoints for a session"}},
    {"checkpoint_fs_save",   {"保存文件系统快照",                 "save a filesystem checkpoint snapshot"}},
    {"checkpoint_fs_rollback", {"回滚文件系统到指定快照",           "rollback filesystem to a checkpoint"}},
    {"checkpoint_fs_diff",   {"查看与快照的文件差异",              "diff filesystem against a checkpoint"}},
    {"checkpoint_fs_list",   {"列出所有文件系统快照",              "list all filesystem checkpoints"}},
    // ── v0.25.4: 纠错 + 目标 + 看板 + 摘要 ──
    {"correction_record", {"记录工具纠错（跨会话学习）",              "record a tool correction for cross-session learning"}},
    {"goal_add",          {"创建长期目标",                      "create a long-term goal"}},
    {"goal_list",         {"列出所有目标",                      "list all goals"}},
    {"goal_update",       {"更新目标状态",                      "update a goal's status"}},
    {"kanban_push",       {"添加任务到看板",                     "push a task to the kanban board"}},
    {"kanban_status",     {"查看看板状态",                      "view kanban board status"}},
    {"summarize",         {"触发会话摘要",                      "trigger conversation summarization"}},
    // ── v0.26.0: 知识库 ──
    {"kb_search",         {"搜索代码知识库（FTS5全文检索）",        "search the codebase knowledge base (FTS5)"}},
    // ── v0.27.8: Python 代码执行 ──
    {"code_exec",         {"执行Python代码片段（子进程隔离）",          "execute Python code (subprocess isolation)"}},
    // ── 技能管理 ──
    {"skill_list",        {"列出所有可用技能",                      "list all available skills"}},
    // ── v0.30.0: PTY + Background ──
    {"process",          {"管理后台进程(poll/wait/kill)",         "manage background processes (poll/wait/kill)"}},
      };
      if (is_zh) {
        body += "当前可用技能：\n";
      } else {
        body += "Available skills:\n";
      }
      for (const auto& t : role_tools) {
        // v0.54.6: 改名 desc_it——此前与外层 `it` 同名（-Wshadow=local 判官命中）
        auto desc_it = kToolDescs.find(t);
        if (desc_it != kToolDescs.end()) {
          body += "- " + t + "：" + (is_zh ? desc_it->second.first : desc_it->second.second) + "\n";
        } else {
          body += "- " + t + "\n";
        }
      }
    }

    // detailed 模式追加运行信息（skills_only 不显示）
    if (detailed && !skills_only) {
      std::string mode_str = is_dev ? (is_zh ? "开发模式" : "dev mode")
                            : is_zh ? "标准模式" : "standard mode";
      if (is_zh) {
        body += "\n---\n运行时：" + std::string(thin_agent::kThinAgentVersion)
             + " | 角色：" + display_zh
             + " | 模式：" + mode_str + "\n";
      } else {
        body += "\n---\nRuntime: " + std::string(thin_agent::kThinAgentVersion)
             + " | Role: " + display_en
             + " | Mode: " + mode_str + "\n";
      }
    }

    return body;
  }

  // 兜底：使用静态模板
  return policy_text_for_lang("profile.concise", lang, policy_text("profile.concise", ""));
}

/// 全局替换子串（非正则）。
std::string replace_all_copy(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  std::string::size_type pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

/// 将模板中的 {key} 占位符替换为 kv 中的值。
std::string render_template(std::string tpl, const std::vector<std::pair<std::string, std::string>>& kv) {
  for (const auto& [k, v] : kv) {
    tpl = replace_all_copy(std::move(tpl), "{" + k + "}", v);
  }
  return tpl;
}

/// 记忆/任务证据行，用于 memory_search 结果排序与去重。
struct EvidenceRow {
  int priority = 99;
  long long order = 0;
  std::string created_at;
  long long audit_id = -1;
  std::string kind;
  std::string task_id;
  std::string source;
  std::string row_ref;
  std::string text;
};

/// 证据行优先级：failed/cancelled 审计优先于成功任务。
int evidence_priority_for_row(const std::string& kind,
                              const std::string& task_state,
                              const std::string& audit_to_state) {
  if (kind == "task_audit") {
    if (audit_to_state == "failed") return 0;
    if (audit_to_state == "cancelled") return 1;
    return 2;
  }
  if (task_state == "failed") return 3;
  if (task_state == "cancelled") return 4;
  return 5;
}

/// 比较两条证据的时间新旧（created_at / audit_id / order）。
bool evidence_is_more_recent(const EvidenceRow& a, const EvidenceRow& b) {
  if (a.created_at != b.created_at) return a.created_at > b.created_at;
  if (a.audit_id != b.audit_id) return a.audit_id > b.audit_id;
  return a.order < b.order;
}

/// 同一 task_id 下选择更优证据：优先级高者优先，同优先级取更新。
bool evidence_better_for_same_task(const EvidenceRow& cand, const EvidenceRow& cur) {
  if (cand.priority != cur.priority) return cand.priority < cur.priority;
  return evidence_is_more_recent(cand, cur);
}

/// 将 task/audit 检索行转为 EvidenceRow。
EvidenceRow build_evidence_row(const nlohmann::json& row, long long order) {
  const std::string kind = row.value("kind", std::string("task"));
  const std::string task_id = row.value("task_id", std::string("-"));
  const std::string source = row.value("source", std::string("task_sqlite"));
  const std::string row_ref = row.value("row_ref", std::string(""));
  const std::string task_state = row.value("state", std::string(""));
  const std::string audit_to_state = row.value("to_state", std::string(""));
  const int priority = evidence_priority_for_row(kind, task_state, audit_to_state);

  if (kind == "task_audit") {
    return EvidenceRow{
        priority,
        order,
        row.value("created_at", std::string("")),
        static_cast<long long>(row.value("audit_id", -1)),
        kind,
        task_id,
        source,
        row_ref,
        "audit " + task_id + " " +
            row.value("from_state", std::string("?")) + "->" +
            row.value("to_state", std::string("?")) +
            " code=" + std::to_string(row.value("code", -1)),
    };
  }

  return EvidenceRow{
      priority,
      order,
      row.value("updated_at", row.value("created_at", std::string(""))),
      -1,
      kind,
      task_id,
      source,
      row_ref,
      "task " + task_id +
          " action=" + row.value("action", std::string("")) +
          " state=" + task_state,
  };
}

/// 从 memory_search 命中结果渲染维护结论与证据点（含 latest_basis）。
nlohmann::json build_task_audit_evidence_render_from_results(const nlohmann::json& results,
                                                             const std::string& query,
                                                             int limit_applied) {
  if (!results.is_array()) return {{"text", ""}};

  int task_hits = 0;
  int audit_hits = 0;
  std::vector<EvidenceRow> evidence_rows;

  long long seq = 0;
  for (const auto& row : results) {
    if (!row.is_object()) continue;
    if (row.value("source", std::string()) != "task_sqlite") continue;
    const std::string kind = row.value("kind", std::string("task"));
    if (kind == "task_audit") ++audit_hits;
    else ++task_hits;
    evidence_rows.push_back(build_evidence_row(row, seq++));
  }
  if (task_hits + audit_hits == 0) return {{"text", ""}};

  std::unordered_map<std::string, EvidenceRow> best_by_task;
  std::vector<EvidenceRow> deduped;
  deduped.reserve(evidence_rows.size());

  for (const auto& row : evidence_rows) {
    auto it = best_by_task.find(row.task_id);
    if (it == best_by_task.end()) {
      best_by_task.emplace(row.task_id, row);
      continue;
    }
    if (evidence_better_for_same_task(row, it->second)) {
      it->second = row;
    }
  }

  for (const auto& kv : best_by_task) deduped.push_back(kv.second);

  std::stable_sort(deduped.begin(), deduped.end(), [](const EvidenceRow& a, const EvidenceRow& b) {
    if (a.priority != b.priority) return a.priority < b.priority;
    if (evidence_is_more_recent(a, b)) return true;
    if (evidence_is_more_recent(b, a)) return false;
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.task_id != b.task_id) return a.task_id < b.task_id;
    return a.order < b.order;
  });

  std::string line1;
  const std::string conclusion_tpl = policy_text("memory.maintenance_conclusion", "");
  if (!conclusion_tpl.empty()) {
    line1 = render_template(
        conclusion_tpl,
        {{"total_hits", std::to_string(task_hits + audit_hits)},
         {"task_hits", std::to_string(task_hits)},
         {"audit_hits", std::to_string(audit_hits)},
         {"query", query},
         {"limit", std::to_string(limit_applied)}});
  } else {
    line1 = "维护结论：task/audit 命中 " + std::to_string(task_hits + audit_hits) +
            " 条（task=" + std::to_string(task_hits) + "，audit=" +
            std::to_string(audit_hits) + "）";
    if (!query.empty()) line1 += "，query=" + query;
    line1 += "，limit=" + std::to_string(limit_applied) + "。";
  }

  std::string line2 = "证据点：";
  if (evidence_rows.empty()) {
    line2 += "task_sqlite 有命中（无可展示样本）";
  } else {
    const size_t pick_n = std::min<size_t>(2, deduped.size());
    const std::string evidence_tpl = policy_text("memory.maintenance_evidence", "");
    if (!evidence_tpl.empty() && pick_n >= 2) {
      const std::string top1 = deduped[0].text;
      const std::string top2 = deduped[1].text;
      const std::string latest_tag = " [latest]";
      line2 = render_template(
          evidence_tpl,
          {{"top1", top1},
           {"top2", top2},
           {"latest_tag", latest_tag}});
    } else {
      for (size_t i = 0; i < pick_n; ++i) {
        if (i > 0) line2 += "；";
        const bool is_latest = (i == 0);
        line2 += "#" + std::to_string(i + 1) + " " + deduped[i].text + (is_latest ? " [latest]" : "");
      }
    }
  }

  nlohmann::json out = {{"text", line1 + "\n" + line2}};
  if (!deduped.empty()) {
    const std::string selected_at = deduped[0].created_at.empty()
        ? (deduped[0].row_ref.empty() ? std::string("selected") : deduped[0].row_ref)
        : deduped[0].created_at;
    out["latest_basis"] = {
        {"task_id", deduped[0].task_id},
        {"kind", deduped[0].kind},
        {"source", deduped[0].source},
        {"row_ref", deduped[0].row_ref},
        {"priority", deduped[0].priority},
        {"created_at", deduped[0].created_at},
        {"audit_id", deduped[0].audit_id},
        {"reason", "priority+recency+tie_break"},
        {"selected_at", selected_at},
    };
  }
  return out;
}

/// 仅返回证据渲染 JSON 中的 text 字段。
std::string build_task_audit_evidence_text_from_results(const nlohmann::json& results,
                                                        const std::string& query,
                                                        int limit_applied) {
  return build_task_audit_evidence_render_from_results(results, query, limit_applied)
      .value("text", std::string(""));
}

/// 仅有 source_counts 无明细行时，渲染简化版维护结论。
nlohmann::json build_task_audit_evidence_render_from_source_counts(const nlohmann::json& source_counts,
                                                                   const std::string& query,
                                                                   int limit_applied,
                                                                   const nlohmann::json& latest_basis = nlohmann::json::object()) {
  if (!source_counts.is_object()) return {{"text", ""}};
  const int task_source_hits = source_counts.value("task_sqlite", 0);
  if (task_source_hits <= 0) return {{"text", ""}};

  std::string line1;
  const std::string conclusion_tpl = policy_text("memory.maintenance_conclusion", "");
  if (!conclusion_tpl.empty()) {
    line1 = render_template(
        conclusion_tpl,
        {{"total_hits", std::to_string(task_source_hits)},
         {"task_hits", std::to_string(task_source_hits)},
         {"audit_hits", "0"},
         {"query", query},
         {"limit", std::to_string(limit_applied)}});
  } else {
    line1 = "维护结论：task/audit 来源命中 " + std::to_string(task_source_hits) +
            " 条（source=task_sqlite）";
    if (!query.empty()) line1 += "，query=" + query;
    line1 += "，limit=" + std::to_string(limit_applied) + "。";
  }

  std::string line2;
  const std::string summary_tpl = policy_text("memory.maintenance_summary_evidence", "");
  if (!summary_tpl.empty() && latest_basis.is_object() && !latest_basis.empty()) {
    line2 = render_template(
        summary_tpl,
        {{"latest_task_id", latest_basis.value("task_id", std::string("-"))},
         {"latest_kind", latest_basis.value("kind", std::string("-"))},
         {"latest_row_ref", latest_basis.value("row_ref", std::string("-"))}});
  } else {
    line2 = "证据点：详见 observation.source_counts 与 observation.summary。";
  }

  nlohmann::json out = {{"text", line1 + "\n" + line2}};
  if (latest_basis.is_object() && !latest_basis.empty()) out["latest_basis"] = latest_basis;
  return out;
}

/// 构造 memory replay 提示 payload，供客户端复现检索条件。
nlohmann::json build_memory_replay_hint(const nlohmann::json& latest_basis,
                                       const std::string& query,
                                       int limit_applied,
                                       const std::string& req_type) {
  if (!latest_basis.is_object() || latest_basis.empty()) return nlohmann::json::object();
  return {
      {"row_ref", latest_basis.value("row_ref", std::string(""))},
      {"selected_at", latest_basis.value("selected_at", std::string(""))},
      {"reason", latest_basis.value("reason", std::string(""))},
      {"payload",
       {
           {"type", req_type.empty() ? "memory_search" : req_type},
           {"query", query},
           {"limit", limit_applied},
       }},
  };
}

/// 任务流水线 error_class → 统一错误码。
std::string pipeline_error_code_from_class(const std::string& error_class) {
  if (error_class == "param") return "PIPELINE_PARAM_INVALID";
  if (error_class == "task") return "PIPELINE_TASK_FAILED";
  if (error_class == "engine") return "PIPELINE_ENGINE_UNAVAILABLE";
  return "";
}

/// 流水线决策 reason 字段：成功 PIPELINE_EXECUTED，失败为对应错误码。
std::string pipeline_decision_reason(bool failed, const std::string& pipeline_error_code) {
  return failed ? pipeline_error_code : "PIPELINE_EXECUTED";
}

/// 构造流水线错误对象（class / code / failed_at）。
nlohmann::json pipeline_error_object(const std::string& error_class,
                                     const std::string& error_code,
                                     int failed_at) {
  return {
      {"class", error_class},
      {"code", error_code},
      {"failed_at", failed_at},
  };
}

/// 构造流水线策略输入 observation 片段。
nlohmann::json pipeline_policy_input(const std::string& policy,
                                     bool failed,
                                     int completed,
                                     const std::string& pipeline_error_code) {
  return {
      {"route", "local_task_pipeline"},
      {"policy", policy},
      {"failed", failed},
      {"completed", completed},
      {"pipeline_error_code", pipeline_error_code},
  };
}

/// 构造流水线执行结果 observation 片段。
nlohmann::json pipeline_outcome_object(bool failed,
                                       const std::string& error_class,
                                       int failed_at) {
  const std::string error_code = pipeline_error_code_from_class(error_class);
  const auto error = pipeline_error_object(error_class, error_code, failed ? failed_at : -1);
  return {
      {"failed", failed},
      {"reason", pipeline_decision_reason(failed, error_code)},
      {"error", error},
  };
}

/// 意图路由用文本归一化（委托 ChatPolicy）。
std::string normalize_text_for_intent(std::string text) {
  return normalize_text_for_policy(std::move(text));
}

/// 按空白切分为词 token 列表。
std::vector<std::string> tokenize_words(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (unsigned char ch : text) {
    if (std::isspace(ch)) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(static_cast<char>(ch));
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

/// 判断 token 集合是否包含指定词。
bool contains_token(const std::unordered_set<std::string>& tokens, const std::string& word) {
  return tokens.find(word) != tokens.end();
}

/// 解析 profile stub ONNX 模型路径（按候选目录查找）。
std::string resolve_intent_model_path() {
  static const std::vector<std::string> candidates = {
      thin_agent::default_intent_stub_onnx_path(),
      "models/intent/intent_stub.onnx",
      "../models/intent/intent_stub.onnx",
      "/root/code/thin_agent/models/intent/intent_stub.onnx",
  };
  for (const auto& p : candidates) {
    if (std::filesystem::exists(p)) return p;
  }
  return candidates.front();
}

/// 前向声明：询问模型/运行时（定义见后文）。
bool is_model_runtime_query(const std::string& norm);

/// 规则引擎本地意图分类：关键词 + 槽位提取，必要时回退 ONNX/模糊打分。
nlohmann::json classify_local_intent(const std::string& text) {
  const std::string norm = normalize_text_for_intent(text);
  const auto token_vec = tokenize_words(norm);
  const std::unordered_set<std::string> tokens(token_vec.begin(), token_vec.end());

  const bool ask_self = contains_any_policy(norm, "profile");
  const bool ask_profile_short = contains_any_policy(norm, "profile_short");
  const bool ask_profile_detail = contains_any_policy(norm, "profile_detail");
  const bool ask_status = contains_any_policy(norm, "status");
  const bool ask_status_advice = contains_any_policy(norm, "status_advice_suggest") &&
                                 contains_any_policy(norm, "status_advice_status") &&
                                 contains_any_policy(norm, "status_advice_task_exec");
  const bool ask_event = contains_any_policy(norm, "event");
  const bool ask_memory_recent = contains_any_policy(norm, "memory_recent");
  const bool ask_memory_history = contains_any_policy(norm, "memory_history");
  const bool ask_memory_search = contains_any_policy(norm, "memory_search");
  const bool ask_memory_summary = contains_any_policy(norm, "memory_summary");
  const bool ask_weather = contains_any_policy(norm, "weather") || is_weather_extra_trigger(norm);
  const bool ask_news = contains_any_policy(norm, "news");
  const bool ask_search = contains_any_policy(norm, "search");
  const bool ask_external_general = contains_any_policy(norm, "external_general") ||
                                    contains_any_policy(norm, "general");
  // v0.46.0: 自主性意图（巡检/监控/目标/快照/遗忘/摘要）— 配置
  // keywords.autonomy。此前规则层无此分支："监控...运行情况" 含 status
  // 关键词（运行情况）→ 被 status 吸走 → local 模板，monitor 工具不可达。
  const bool ask_autonomy = contains_any_policy(norm, "autonomy");

  const bool ask_model_status = is_model_runtime_query(norm);

  if (ask_model_status) {
    return {{"intent", "status"}, {"confidence", 0.98}, {"backend", "rules"}, {"slots", nlohmann::json::object()}};
  }

  // 文件操作类请求（路径 + 操作词）不应匹配到 profile，
  // 因为路径里可能包含 "thin_agent" 等 profile 关键词子串。
  // v0.45.4: 补齐中文操作词（创建/写入/生成/删除等），
  // 否则 "创建 /tmp/x 内容 hello thin_agent" 会因内容含
  // thin_agent 字样被误判为 profile（能力验证实测暴露）。
  const bool is_file_op = AgentService::is_file_op_request(norm);
  const bool any_agent_token =
      contains_token(tokens, "thin_agent") || contains_token(tokens, "thin") || contains_token(tokens, "agent");
  // v0.47.6: 文本含操作动词时不触发 profile（用户在下达指令而非询问身份）。
  // 避免 "返回JSON: {tool:'thin_agent'}" 被误判为 profile 意图。
  static const char* kProfileActionVerbs[] = {
      "返回", "记住", "读取", "写入", "查看", "搜索", "运行", "执行",
      "创建", "删除", "修改", "编译", "安装", "解释", "说明", "翻译",
      "计算", "生成", "发送", "保存", "加载", "测试", "调试", "部署",
      "explain", "remember", "return", "create", "delete", "search",
      "write", "read", "execute", "compile", "build", "describe",
      "translate", "calculate", "generate"};
  bool has_action_verb = false;
  for (const char* v : kProfileActionVerbs) {
    if (norm.find(v) != std::string::npos) { has_action_verb = true; break; }
  }
  const bool profile_trigger =
      (ask_self || ask_profile_short || ask_profile_detail || any_agent_token) &&
      !is_file_op && !has_action_verb;  // v0.47.6: 操作动词时不触发

  if (profile_trigger) {
    nlohmann::json slots = nlohmann::json::object();
    if (ask_profile_detail) {
      slots["profile_mode"] = "detailed";
      return {{"intent", "profile"}, {"confidence", 0.95}, {"backend", "rules"}, {"slots", std::move(slots)}};
    }
    slots["profile_mode"] = "concise";
    if (ask_profile_short) {
      double profile_conf = 0.0;
      std::string err;
      const std::string model_path = resolve_intent_model_path();
      if (infer_profile_confidence_onnx(model_path, &profile_conf, &err)) {
        return {{"intent", "profile"}, {"confidence", profile_conf}, {"backend", "onnx"}, {"slots", std::move(slots)}};
      }
      return {{"intent", "profile"}, {"confidence", 0.86}, {"backend", "local-model"}, {"slots", std::move(slots)}, {"model_error", err}, {"model_path", model_path}};
    }
    return {{"intent", "profile"}, {"confidence", 0.99}, {"backend", "rules"}, {"slots", std::move(slots)}};
  }
  if (ask_status_advice) {
    return {{"intent", "general"}, {"confidence", 0.58}, {"backend", "rules"}, {"slots", nlohmann::json::object()}};
  }
  // v0.46.0: autonomy 优先于 status（"监控...运行情况" 不应被 status 吸走）。
  // 纯状态提问（"你现在什么状态"）不含 autonomy 关键词，仍走 status。
  // v0.49.2: autonomy 的关键词较宽（部署配置含"摘要"），memory 族
  // 关键词更精确（"记忆摘要"/"记忆检索"），精确意图先于宽泛能力意图判定
  // ——否则"请做记忆摘要"被 autonomy（"摘要"）抢走，memory_summary 不可达。
  if (ask_memory_recent) {
    return {{"intent", "memory_recent"}, {"confidence", 0.97}, {"backend", "rules"}, {"slots", nlohmann::json::object()}};
  }
  if (ask_memory_history) {
    return {{"intent", "memory_history"}, {"confidence", 0.97}, {"backend", "rules"}, {"slots", nlohmann::json{{"limit", 20}}}};
  }
  if (ask_memory_search) {
    const auto parsed = parse_query_and_limit(
        extract_query_after_keywords(text, policy_keywords("memory_search")),
        20);
    return {{"intent", "memory_search"},
            {"confidence", 0.97},
            {"backend", "rules"},
            {"slots", nlohmann::json{{"query", parsed.first}, {"limit", parsed.second}}}};
  }
  if (ask_memory_summary) {
    const auto parsed = parse_query_and_limit(
        extract_query_after_keywords(text, policy_keywords("memory_summary")),
        20);
    return {{"intent", "memory_summary"},
            {"confidence", 0.97},
            {"backend", "rules"},
            {"slots", nlohmann::json{{"query", parsed.first}, {"limit", parsed.second}}}};
  }
  if (ask_autonomy) {
    return {{"intent", "autonomy"}, {"confidence", 0.97}, {"backend", "rules"}, {"slots", nlohmann::json::object()}};
  }
  // v0.52.3: 复杂任务信号（能力类别，同 autonomy 模式）——多步/多文件
  // 任务走分解编排（agent_decompose_and_run）而非单层 FC 循环。
  // 关键词保守（重构/多文件/分阶段等明确措辞），宁漏判不误判：
  // 误路由 = 简单任务被拆成 3-7 个子代理烧钱；漏判 = 单层循环照样能做。
  // 放在 autonomy 后、status 前（"分阶段查看状态"仍走 status——
  // 复杂任务词必伴操作动词，纯查询措辞不含这些词）。
  const bool ask_complex_task = contains_any_policy(norm, "complex_task");
  if (ask_complex_task && !ask_status) {
    return {{"intent", "complex_task"}, {"confidence", 0.95}, {"backend", "rules"},
            {"slots", nlohmann::json::object()}};
  }
  if (ask_status) {
    return {{"intent", "status"}, {"confidence", 0.98}, {"backend", "rules"}, {"slots", nlohmann::json::object()}};
  }
  if (ask_event) {
    return {{"intent", "event_recent"}, {"confidence", 0.97}, {"backend", "rules"}, {"slots", nlohmann::json::object()}};
  }

  if (ask_weather) {
    nlohmann::json slots = nlohmann::json::object();
    const std::string city = detect_city_slot(norm);
    if (!city.empty()) slots["city"] = city;
    const std::string date = resolve_weather_date_slot(norm);
    if (!date.empty()) slots["date"] = date;
    return {{"intent", "weather"}, {"confidence", city.empty() ? 0.64 : 0.84}, {"backend", "rules"}, {"slots", std::move(slots)}};
  }

  if (ask_news) {
    nlohmann::json slots = nlohmann::json::object();
    const std::string topic = detect_news_topic_slot(norm);
    if (!topic.empty()) slots["topic_or_scope"] = topic;
    const std::string tr = resolve_news_time_range_slot(norm);
    if (!tr.empty()) slots["time_range"] = tr;
    return {{"intent", "news"}, {"confidence", topic.empty() ? 0.62 : 0.83}, {"backend", "rules"}, {"slots", std::move(slots)}};
  }

  if (ask_search) {
    nlohmann::json slots = nlohmann::json::object();
    const std::string q = extract_query_after_keywords(text, policy_keywords("search"));
    if (!q.empty()) slots["query"] = q;
    return {{"intent", "search"}, {"confidence", q.empty() ? 0.58 : 0.80}, {"backend", "rules"}, {"slots", std::move(slots)}};
  }

  if (ask_external_general) {
    return {{"intent", "general"}, {"confidence", 0.52}, {"backend", "rules"}, {"slots", nlohmann::json::object()}};
  }

  const auto fuzzy = classify_intent_fuzzy(text, nullptr);
  const double fuzzy_conf = fuzzy.value("confidence", 0.0);
  if (fuzzy_conf >= intent_clarify_threshold() && fuzzy.value("intent", "unknown") != "unknown") {
    return fuzzy;
  }

  return {{"intent", "unknown"}, {"confidence", 0.0}, {"backend", "rules"}, {"slots", nlohmann::json::object()}};
}

/// 云策略文本归一化：去标点并小写，供 parse 启发式匹配。
std::string normalize_cloud_strategy_text(const std::string& s) {
  std::string out = s;
  static const std::vector<std::string> punct = {
      "，", "。", "！", "？", "：", "；", "（", "）", "【", "】", "、", "~",
      ",", ".", "!", "?", ":", ";", "(", ")", "[", "]", "\"", "'",
      "\xE2\x80\x99", "\xE2\x80\x98"};
  for (const auto& p : punct) {
    std::string::size_type pos = 0;
    while ((pos = out.find(p, pos)) != std::string::npos) {
      out.replace(pos, p.size(), " ");
      pos += 1;
    }
  }
  return to_lower_copy(std::move(out));
}

/// 是否需要把数据源内容送 LLM 翻译成提问语言：源语言与提问语言不一致即翻译。
bool needs_translation(const std::string& query_lang, const std::string& source_lang) {
  if (source_lang.empty()) return false;
  return query_lang != source_lang;
}

bool contains_ascii_alpha(const std::string& text) {
  for (unsigned char c : text) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
  }
  return false;
}

std::vector<std::string> translate_texts(const DemoConfigCompat& cfg,
                                         const std::vector<std::string>& texts,
                                         const std::string& target_lang,
                                         const std::string& target_sample);

/// 策略 B：按 query/template 语言决定是否把整段回复送 LLM 翻译。
/// 当 template_lang 与 reply_target 相同但 policy 实际回退到英文模板时，按渲染文本语言补翻译。
std::string apply_reply_translation_if_needed(const DemoConfigCompat& cfg,
                                              std::string reply,
                                              const std::string& query_lang,
                                              const std::string& template_lang,
                                              const std::string& sample) {
  const std::string target = reply_target_language(query_lang, template_lang);
  bool should_translate = needs_reply_translation(query_lang, template_lang);
  if (!should_translate) {
    const std::string rendered_lang = detect_query_language(reply);
    if (rendered_lang != target) {
      const bool zh_family =
          rendered_lang == "zh" && (target == "zh" || target == "zh-TW");
      if (!zh_family) should_translate = true;
    }
  }
  if (!should_translate) return reply;
  const auto tr = translate_texts(cfg, {reply}, target, sample);
  if (!tr.empty() && !tr[0].empty()) return tr[0];
  return reply;
}

/// cloud / auto 模式下开放类查询可走云端策略。
bool should_route_open_queries_to_cloud(const std::string& mode) {
  return mode == "cloud" || mode == "auto";
}

/// ── 跨轮上下文辅助函数 ──

/// 粗略估计 token 数：英文 ~4 chars/token，中文 ~1.5 chars/token
size_t estimate_tokens(const std::string& text) {
  if (text.empty()) return 0;
  size_t utf8_chars = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if ((text[i] & 0xC0) != 0x80) ++utf8_chars;
  }
  // 混合假设：约 2.5 chars/token
  return std::max<size_t>(1, text.size() / 3 + utf8_chars / 6);
}

/// 按 token 预算裁剪历史消息（最近 full_turns 轮全文，更早截断）
/// 返回裁剪后的消息（原位修改副本，不修改原始 mem）
// v0.52.25: 解析 FC 循环 token 预算窗口——配置显式值优先，
// 否则按 model_name 推断（GLM-5.2/glm-4.x=128k，DeepSeek=64k，未知保守 32k）。
int resolve_context_window(const DemoConfigCompat& cfg) {
  if (cfg.context_window_tokens > 0) return cfg.context_window_tokens;
  return infer_context_window(cfg.model_name);
}

void trim_memory_by_token_budget(std::vector<ChatMessage>& mem,
                                        size_t budget_tokens,
                                        int full_context_turns,
                                        size_t truncated_max_chars) {
  if (mem.empty()) return;

  // 从尾部累积 token 预算（最新的消息优先保留）
  size_t used = 0;
  size_t keep_from = mem.size();
  int full_count = full_context_turns * 2;  // 2 messages per turn

  for (size_t i = mem.size(); i > 0; --i) {
    const auto& msg = mem[i - 1];
    // v0.53.81: 零拷贝快路径——近 full_context 轮或短消息直接以原
    /// content 估 token;此前每条无条件值拷贝(长会话=每消息全历史
    /// 拷贝税);仅确需截断的老消息才物化副本
    if (mem.size() - (i - 1) <= static_cast<size_t>(full_count) ||
        msg.content.size() <= truncated_max_chars) {
      const size_t tok = estimate_tokens(msg.content);
      if (used + tok > budget_tokens && i < mem.size()) {
        keep_from = i;
        break;
      }
      used += tok;
      keep_from = i - 1;
      continue;
    }
    std::string content = msg.content.substr(0, truncated_max_chars) + "...";
    size_t tok = estimate_tokens(content);
    if (used + tok > budget_tokens && i < mem.size()) {
      keep_from = i;
      break;
    }
    used += tok;
    keep_from = i - 1;
  }

  if (keep_from > 0)
    mem.erase(mem.begin(), mem.begin() + static_cast<long>(keep_from));
}

/// ③ 项目结构扫描：递归列出目录树（忽略配置的目录），返回 ascii tree 字符串
std::string scan_project_structure(const std::string& root_path,
                                          int max_depth) {
  namespace fs = std::filesystem;
  std::string out;
  // 忽略列表
  static const std::vector<std::string> kDefaultIgnore = {
    ".git", "node_modules", "build", "__pycache__", ".pytest_cache",
    "third_party", "models", ".cache", "cmake-build-*"
  };

  auto should_ignore = [&](const std::string& name) -> bool {
    for (const auto& pat : kDefaultIgnore) {
      if (name == pat) return true;
    }
    // Hidden files/dirs (except .thin_agent)
    if (name.size() > 0 && name[0] == '.' && name != ".thin_agent") return true;
    return false;
  };

  std::function<void(const fs::path&, int, const std::string&)> walk;
  walk = [&](const fs::path& dir, int depth, const std::string& prefix) {
    if (depth > max_depth) return;
    std::error_code ec;
    std::vector<fs::directory_entry> entries;
    for (auto& e : fs::directory_iterator(dir, ec)) entries.push_back(e);
    if (ec) return;

    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); });

    for (size_t i = 0; i < entries.size(); ++i) {
      bool last = (i == entries.size() - 1);
      const auto& name = entries[i].path().filename().string();
      if (should_ignore(name)) continue;

      bool is_dir = entries[i].is_directory(ec);
      if (ec) is_dir = false;

      out += prefix + (last ? "└── " : "├── ") + name + (is_dir ? "/" : "") + "\n";
      if (is_dir) {
        walk(entries[i].path(), depth + 1,
             prefix + (last ? "    " : "│   "));
      }
    }
  };

  // 标题
  out = "PROJECT STRUCTURE (" + fs::path(root_path).filename().string() + ")\n";
  walk(fs::path(root_path), 0, "");
  return out;
}

/// 构造发给云端策略顾问的 user JSON（本地规则结果 + 对话状态 + 近期轮次）。
std::string build_cloud_user_payload(const std::string& text,
                                     const nlohmann::json& intent_info,
                                     const DialogContext& dialog_ctx,
                                     const std::vector<ChatMessage>& recent_turns) {
  const DialogContext* ctx_ptr = dialog_ctx.last_intent.empty() ? nullptr : &dialog_ctx;
  nlohmann::json top_intents = nlohmann::json::array();
  for (const auto& item : top_intent_scores(text, 3, ctx_ptr)) {
    top_intents.push_back({{"intent", item.first}, {"score", item.second}});
  }
  nlohmann::json recent = nlohmann::json::array();
  for (const auto& t : recent_turns) recent.push_back({{"role", t.role}, {"content", t.content}});
  return nlohmann::json{
      {"user_text", text},
      {"local_rules_intent", intent_info.value("intent", "unknown")},
      {"local_rules_confidence", intent_info.value("confidence", 0.0)},
      {"local_top_intents", top_intents},
      {"dialog_state",
       {{"last_intent", dialog_ctx.last_intent},
        {"last_route", dialog_ctx.last_route},
        {"last_slots", dialog_ctx.last_slots},
        {"ttl_turns", dialog_ctx.ttl_turns}}},
      {"recent_turns", recent},
  }.dump();
}

/// 从云模型回复中移除推理链标记、markdown 围栏，便于 JSON 解析。
std::string strip_cloud_model_artifacts(const std::string& text) {
  std::string out = text;
  static const std::vector<std::pair<std::string, std::string>> kInlineMarkers = {
      {"_draft：", ""}, {"_draft:", ""}, {"_draft ", ""}, {"draft：", ""}, {"draft:", ""},
      {"_think：", ""}, {"_think:", ""}, {"think：", ""}, {"think:", ""},
  };
  for (const auto& [from, to] : kInlineMarkers) {
    std::string::size_type pos = 0;
    while ((pos = out.find(from, pos)) != std::string::npos) {
      out.replace(pos, from.size(), to);
    }
  }

  std::string cleaned;
  cleaned.reserve(out.size());
  std::istringstream stream(out);
  std::string line;
  while (std::getline(stream, line)) {
    size_t b = 0;
    while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
    size_t e = line.size();
    while (e > b && std::isspace(static_cast<unsigned char>(line[e - 1]))) --e;
    const std::string trimmed = line.substr(b, e - b);
    if (trimmed.empty()) continue;
    if (trimmed == "json" || trimmed == "```" || trimmed == "```json") continue;
    if (trimmed.rfind("```", 0) == 0) continue;
    static const std::vector<std::string> kLinePrefixes = {
        "_draft：", "_draft:", "_draft ", "draft：", "draft:",
        "_think：", "_think:", "think：", "think:",
    };
    bool skip_line = false;
    for (const auto& p : kLinePrefixes) {
      if (trimmed.rfind(p, 0) == 0) {
        skip_line = true;
        break;
      }
    }
    if (skip_line) continue;
    // 去掉云模型自动生成的数字编号（1. / 1) / 1 -  等）
    size_t idx = 0;
    while (idx < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[idx]))) ++idx;
    if (idx > 0 && idx < trimmed.size()) {
      char sep = trimmed[idx];
      // v0.53.16: 全角顿号/句点用字节串比较（-Wmultichar：'' 内多字节=
      // implementation-defined 多字符常量，碰巧工作但写法危险）
      static const std::string kSepDun = "\xE3\x80\x81";   // 、
      static const std::string kSepDot = "\xEF\xBC\x8E";   // ．
      const bool sep_fw = (std::string(1, sep) == std::string(1, sep));
      (void)sep_fw;
      const std::string sep_str(1, sep);
      if (sep == '.' || sep == ')' || sep == '-' ||
          sep_str == "\xE3\x80\x81" || sep_str == "\xEF\xBC\x8E") {
        size_t skip = idx + 1;
        while (skip < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[skip]))) ++skip;
        if (skip < trimmed.size()) {
          if (!cleaned.empty()) cleaned += '\n';
          cleaned += trimmed.substr(skip);
          continue;
        }
      }
    }
    if (!cleaned.empty()) cleaned += '\n';
    cleaned += line;
  }
  return cleaned;
}

/// 从混合文本中提取首个完整 JSON 对象子串。
std::string extract_json_object(const std::string& text) {
  const size_t start = text.find('{');
  if (start == std::string::npos) return "";
  int depth = 0;
  for (size_t i = start; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch == '{') ++depth;
    else if (ch == '}') {
      --depth;
      if (depth == 0) return text.substr(start, i - start + 1);
    }
  }
  return "";
}

/// 判断文本是否像未解析的云策略 JSON 残留。
bool looks_like_json_blob(const std::string& text) {
  const std::string norm = normalize_cloud_strategy_text(text);
  return norm.find("strategy") != std::string::npos &&
         norm.find("response_draft") != std::string::npos;
}

/// 从云策略 JSON 中选取可展示给用户的回复（draft / clarify）。
std::string pick_cloud_visible_reply(const nlohmann::json& policy) {
  const auto is_clean = [](const std::string& s) {
    return !s.empty() && !looks_like_json_blob(s) && s.find("_draft") == std::string::npos;
  };
  const std::string strategy = policy.value("strategy", "");
  const std::string clarify = policy.value("clarify_question", "");
  const std::string draft = policy.value("response_draft", "");
  const bool need_clarify = policy.value("need_clarify", false);

  if (is_clean(draft) && is_clean(clarify)) {
    if (need_clarify || strategy == "clarify") {
      return draft.size() >= clarify.size() ? draft : clarify;
    }
    return draft;
  }
  if (is_clean(draft)) return draft;
  if (is_clean(clarify)) return clarify;
  return "";
}

/// 解析云策略可见回复：优先 policy 字段，否则清理 raw 文本。
std::string resolve_cloud_reply_text(const nlohmann::json& policy, const std::string& cloud_raw_text) {
  std::string reply = pick_cloud_visible_reply(policy);
  if (!reply.empty()) return reply;
  reply = strip_cloud_model_artifacts(cloud_raw_text);
  if (looks_like_json_blob(reply)) reply.clear();
  return reply;
}

/// 解析云模型返回的策略 JSON；失败时按 ChatPolicy 启发式词表降级。
nlohmann::json parse_cloud_strategy_json(const std::string& text, const std::string& fallback_intent) {
  const std::string cleaned = strip_cloud_model_artifacts(text);
  nlohmann::json result = {
      {"strategy", "answer_direct"},
      {"intent", fallback_intent.empty() ? "general_query" : fallback_intent},
      {"confidence", 0.40},
      {"need_clarify", false},
      {"clarify_question", ""},
      {"local_route_hint", ""},
      {"response_draft", ""},
      {"risk", "medium"},
      {"tool_hint", nlohmann::json::array()},
      {"task_pipeline", nlohmann::json::array()},
      {"reason", "cloud_response_parse_fallback"},
      {"raw", text},
      {"parser_mode", "fallback"},
  };

  auto set_if_string = [&](const nlohmann::json& j, const char* key) {
    if (j.contains(key) && j[key].is_string()) result[key] = j[key].get<std::string>();
  };
  auto set_if_bool = [&](const nlohmann::json& j, const char* key) {
    if (j.contains(key) && j[key].is_boolean()) result[key] = j[key].get<bool>();
  };
  auto set_if_number = [&](const nlohmann::json& j, const char* key) {
    if (j.contains(key) && j[key].is_number()) result[key] = j[key].get<double>();
  };

  auto apply_parsed_json = [&](const nlohmann::json& j) {
    set_if_string(j, "strategy");
    set_if_string(j, "intent");
    set_if_number(j, "confidence");
    set_if_bool(j, "need_clarify");
    set_if_string(j, "clarify_question");
    set_if_string(j, "local_route_hint");
    set_if_string(j, "response_draft");
    if (j.contains("risk")) {
      if (j["risk"].is_string()) {
        result["risk"] = j["risk"].get<std::string>();
      } else if (j["risk"].is_number()) {
        const double r = j["risk"].get<double>();
        result["risk"] = r <= 0.34 ? "low" : (r <= 0.67 ? "medium" : "high");
      }
    }
    set_if_string(j, "reason");
    if (j.contains("tool_hint") && j["tool_hint"].is_array()) result["tool_hint"] = j["tool_hint"];
    if (j.contains("task_pipeline") && j["task_pipeline"].is_array()) result["task_pipeline"] = j["task_pipeline"];
    result["raw"] = strip_cloud_model_artifacts(text);
    result["parser_mode"] = "json";
    if (j.contains("reason") && j["reason"].is_string() && !j["reason"].get<std::string>().empty()) {
      result["reason"] = j["reason"];
    } else {
      result["reason"] = "cloud_strategy_parsed";
    }
  };

  auto try_parse_json_text = [&](const std::string& candidate) -> bool {
    if (candidate.empty()) return false;
    try {
      apply_parsed_json(nlohmann::json::parse(candidate));
      return true;
    } catch (...) {
      return false;
    }
  };

  if (try_parse_json_text(cleaned)) return result;

  const std::string embedded = extract_json_object(cleaned);
  if (try_parse_json_text(embedded)) return result;

  const std::string norm = normalize_cloud_strategy_text(cleaned);
  const bool heuristic_route_ok =
      cleaned.size() <= 240 && cleaned.find("```") == std::string::npos &&
      cleaned.find("#include") == std::string::npos;

  if (heuristic_route_ok && contains_any(norm, cloud_parse_heuristics("clarify"))) {
    result["strategy"] = "clarify";
    result["need_clarify"] = true;
    result["clarify_question"] = cleaned;
    result["reason"] = "cloud_text_hint_clarify";
  }

  if (heuristic_route_ok && contains_any(norm, cloud_parse_heuristics("local_status"))) {
    result["local_route_hint"] = "local_status";
    result["intent"] = "status";
    result["reason"] = "cloud_text_hint_local_status";
  } else if (heuristic_route_ok && contains_any(norm, cloud_parse_heuristics("local_profile"))) {
    result["local_route_hint"] = "local_profile";
    result["intent"] = "profile";
    result["reason"] = "cloud_text_hint_local_profile";
  }

  if (heuristic_route_ok && contains_any(norm, cloud_parse_heuristics("local_task_capture"))) {
    result["local_route_hint"] = "local_task_inline_capture_photo";
    result["intent"] = "task_capture";
    result["reason"] = "cloud_text_hint_local_task_capture";
  } else if (heuristic_route_ok && contains_any(norm, cloud_parse_heuristics("local_task_recording"))) {
    result["local_route_hint"] = "local_task_inline_start_recording";
    result["intent"] = "task_start_recording";
    result["reason"] = "cloud_text_hint_local_task_recording";
  }

  if (contains_any(norm, cloud_parse_heuristics("high_risk"))) {
    result["risk"] = "high";
    if (result.value("strategy", std::string("answer_direct")) == "answer_direct") {
      result["strategy"] = "reject";
      result["reason"] = "cloud_text_hint_high_risk_reject";
    }
  } else if (contains_any(norm, cloud_parse_heuristics("low_risk"))) {
    result["risk"] = "low";
  }

  if (contains_any(norm, cloud_parse_heuristics("reject"))) {
    result["strategy"] = "reject";
    if (result.value("reason", std::string()).empty() ||
        result.value("reason", std::string()) == "cloud_response_parse_fallback") {
      result["reason"] = "cloud_text_hint_reject";
    }
  }

  const std::string visible = pick_cloud_visible_reply(result);
  if (!visible.empty()) result["response_draft"] = visible;
  return result;
}

/// 从 JSON 对象安全读取字符串槽位（跳过 null / 非字符串）。
std::string json_safe_string(const nlohmann::json& obj, const char* key, const std::string& fallback = "") {
  if (!obj.is_object() || !obj.contains(key)) return fallback;
  const auto& v = obj[key];
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
  if (v.is_number_float()) return std::to_string(v.get<double>());
  return fallback;
}

/// 去除 slots 中的 null，避免后续 .get<std::string>() 崩溃。
nlohmann::json sanitize_json_slots(const nlohmann::json& slots) {
  nlohmann::json out = nlohmann::json::object();
  if (!slots.is_object()) return out;
  for (auto it = slots.begin(); it != slots.end(); ++it) {
    if (it.value().is_null()) continue;
    if (it.value().is_string() || it.value().is_number() || it.value().is_boolean()) {
      out[it.key()] = it.value();
    }
  }
  return out;
}

/// 云策略 local_route_hint 别名归一（如 local_memory → local_memory_recent）。
std::string normalize_cloud_route_hint(std::string hint) {
  if (hint == "local_memory") return "local_memory_recent";
  if (hint == "local_events") return "local_events";
  return hint;
}

/// 询问当前使用什么模型/运行时（应路由到 status）。
bool is_model_runtime_query(const std::string& norm) {
  if (contains_any_policy(norm, "model_status")) return true;
  if (norm.find("模型") == std::string::npos) return false;
  static const std::vector<std::string> kHints = {
      "什么", "用的", "用啥", "本地", "端侧", "onnx", "runtime", "哪个",
  };
  for (const auto& h : kHints) {
    if (norm.find(h) != std::string::npos) return true;
  }
  return norm.find("你的模型") != std::string::npos;
}

/// 明确询问本地/端侧/ONNX 模型（区别于泛化 model_status）。
bool is_local_runtime_model_query(const std::string& norm) {
  if (norm.find("本地模型") != std::string::npos || norm.find("端侧模型") != std::string::npos) {
    return true;
  }
  if (norm.find("onnx") != std::string::npos &&
      (norm.find("模型") != std::string::npos || norm.find("用的") != std::string::npos)) {
    return true;
  }
  return false;
}

/// 开放知识类 ONNX 相关问题（非模型状态查询）。
bool is_onnx_knowledge_query(const std::string& norm) {
  if (norm.find("onnx") == std::string::npos) return false;
  if (norm.find("不是问你") != std::string::npos) return true;
  static const std::vector<std::string> kAsk = {"是什么", "什么是", "指哪个", "另外一个", "哪个", "what is"};
  for (const auto& a : kAsk) {
    if (norm.find(a) != std::string::npos) return true;
  }
  return false;
}

/// is_onnx_knowledge_query 别名。
bool is_open_knowledge_query(const std::string& norm) { return is_onnx_knowledge_query(norm); }

/// 询问消息/推送能力（非新闻类）。
bool is_messaging_capability_query(const std::string& norm) {
  if (norm == "消息能力") return true;
  return norm.find("消息") != std::string::npos && norm.find("能力") != std::string::npos &&
         norm.find("新闻") == std::string::npos;
}

/// 纯对话确认词（好的/ok 等），无需 Tier 2。
bool is_bare_conversation_ack(const std::string& norm) {
  static const std::vector<std::string> kAck = {
      "好的", "好", "行", "嗯", "是的", "对", "ok", "okay", "yes", "yep", "yeah",
  };
  for (const auto& a : kAck) {
    if (norm == normalize_text_for_intent(a)) return true;
  }
  return false;
}

/// Tier 2 之前即可确定的本地路由（当前实现在 tier2 块之后才 return）。
bool should_skip_tier2_for_deterministic_local_route(const std::string& norm) {
  if (is_bare_conversation_ack(norm)) return true;
  if (is_messaging_capability_query(norm)) return true;
  if (is_onnx_knowledge_query(norm)) return true;
  if (is_supported_languages_query(norm)) return true;
  if (is_weather_location_recall_query(norm)) return true;
  if (is_news_topic_recall_query(norm)) return true;
  // 阶段4：媒体关键词命中但话轮歧义时，不跳过 Tier2（交给 LLM Router）
  if (contains_any_policy(norm, "task_capture_inline") ||
      contains_any_policy(norm, "action_capture") ||
      contains_any_policy(norm, "action_start_recording") ||
      contains_any_policy(norm, "task_start_recording_inline")) {
    if (is_ambiguous_media_utterance(norm, nullptr)) return false;
    return true;
  }
  return false;
}

/// intent_backend 字段映射为决策审计中的 layer 名称。
std::string intent_trace_layer_name(const std::string& backend) {
  if (backend == "fuzzy") return "fuzzy";
  if (backend == "onnx") return "onnx";
  if (backend == "cloud-classify") return "cloud_classify";
  if (backend == "local-model") return "local_model";
  return "rules";
}

/// 本地规则已达执行阈值或槽位已齐时跳过 Tier 2。
bool should_skip_tier2_for_executable_intent(const nlohmann::json& intent_info,
                                           const DialogContext& dialog_ctx,
                                           const std::string& text) {
  // 阶段4：媒体歧义句即使本地置信度高，也交给 Tier2 Router
  if (is_ambiguous_media_utterance(text, &dialog_ctx)) return false;
  const double conf = intent_info.value("confidence", 0.0);
  if (conf >= intent_execute_threshold()) return true;
  const std::string intent = intent_info.value("intent", "");
  const auto slots = intent_info.value("slots", nlohmann::json::object());
  IntentSpec spec;
  if (load_intent_spec(intent, &spec) && spec.kind == "external") {
    if (conf >= intent_clarify_threshold() && !spec.primary_slot.empty() &&
        !slots.value(spec.primary_slot, "").empty()) {
      return true;
    }
    if (spec.dialog_slot_policy == "referential" && dialog_ctx.last_intent == intent &&
        has_referential_context_markers(text) &&
        !recalled_slot_from_dialog(dialog_ctx, spec.primary_slot).empty()) {
      return true;
    }
  }
  return false;
}

/// 天气续问时从 dialog 补 city 并抬升 confidence 至执行阈值。
void maybe_boost_weather_intent_from_dialog(nlohmann::json& intent_info,
                                            const DialogContext& dialog_ctx,
                                            const std::string& text) {
  IntentSpec spec;
  if (!load_intent_spec(intent_info.value("intent", ""), &spec) || spec.kind != "external") return;
  if (spec.dialog_slot_policy != "referential") return;
  if (intent_info.value("confidence", 0.0) >= intent_execute_threshold()) return;
  if (dialog_ctx.last_intent != spec.name) return;
  const std::string slot_key = spec.primary_slot;
  if (slot_key.empty()) return;
  const std::string recalled = recalled_slot_from_dialog(dialog_ctx, slot_key);
  if (recalled.empty() || !has_referential_context_markers(text)) return;
  nlohmann::json slots = intent_info.value("slots", nlohmann::json::object());
  if (slots.value(slot_key, "").empty()) slots[slot_key] = recalled;
  intent_info["slots"] = std::move(slots);
  intent_info["confidence"] =
      std::max(intent_info.value("confidence", 0.0), intent_execute_threshold());
}

/// 指代/上下文追问：缺槽但仍需 Tier 2 补全槽位或改意图（见 DialogSlotRecall.h）。

/// 规则已识别 intent 且仅缺槽澄清时跳过 Tier 2（如「查天气」「看新闻」短句）。
bool should_skip_tier2_for_slot_clarify(const std::string& text, const nlohmann::json& intent_info) {
  if (has_referential_context_markers(text)) return false;
  const std::string norm = normalize_text_for_intent(text);
  if (utf8_char_count_at_least(norm, 13)) return false;
  const std::string intent = intent_info.value("intent", "");
  IntentSpec spec;
  if (!load_intent_spec(intent, &spec) || spec.kind != "external" || spec.primary_slot.empty()) {
    return false;
  }
  const auto slots = intent_info.value("slots", nlohmann::json::object());
  return slots.value(spec.primary_slot, std::string()).empty();
}

/// 按 IntentSpec.slot_detector 从文本解析主槽位。
std::string detect_slot_by_spec(const IntentSpec& spec,
                                const std::string& text,
                                const std::string& norm,
                                nlohmann::json& slots) {
  if (spec.slot_detector == "weather_city") {
    std::string city = json_safe_string(slots, "city", "");
    if (city.empty() || is_weather_date_token(normalize_text_for_policy(city))) {
      city.clear();
      city = resolve_weather_city_candidate(text, norm);
      if (!city.empty()) slots["city"] = city;
    }
    return city;
  }
  if (spec.slot_detector == "news_topic") {
    std::string topic = json_safe_string(slots, "topic_or_scope", "");
    if (topic.empty()) {
      topic = detect_news_topic_slot(norm);
      if (!topic.empty()) slots["topic_or_scope"] = topic;
    }
    return topic;
  }
  if (!spec.primary_slot.empty()) {
    return json_safe_string(slots, spec.primary_slot.c_str(), "");
  }
  return "";
}

/// 按 IntentSpec.dialog_slot_policy 从多轮上下文补主槽位。
std::string apply_dialog_slot_by_spec(const IntentSpec& spec,
                                      const std::string& text,
                                      const DialogContext& dialog_ctx,
                                      nlohmann::json& slots,
                                      const std::string& current) {
  if (!current.empty() || spec.primary_slot.empty()) return current;
  std::string recalled = current;
  if (!should_apply_dialog_slot(spec.name, spec.primary_slot, text, dialog_ctx, recalled)) {
    return current;
  }
  recalled = recalled_slot_from_dialog(dialog_ctx, spec.primary_slot);
  if (!recalled.empty()) slots[spec.primary_slot] = recalled;
  return recalled;
}

/// 解析 optional_slots 中的默认/resolver。
void fill_optional_slots_by_spec(const IntentSpec& spec,
                                 const std::string& norm,
                                 nlohmann::json& slots) {
  if (!spec.optional_slots.is_object()) return;
  for (auto it = spec.optional_slots.begin(); it != spec.optional_slots.end(); ++it) {
    if (!it.value().is_object()) continue;
    const std::string key = it.key();
    if (!json_safe_string(slots, key.c_str(), "").empty()) continue;
    const std::string resolver = it.value().value("resolver", "");
    const std::string def = it.value().value("default", "");
    std::string val;
    if (resolver == "weather_date") {
      val = resolve_weather_date_slot(norm);
    } else if (resolver == "news_time_range") {
      val = resolve_news_time_range_slot(norm);
    }
    if (val.empty()) val = def;
    if (!val.empty()) slots[key] = val;
  }
}

/// 按 IntentSpec.route_excludes 判断是否应跳过该本地意图路由。
bool intent_route_excluded(const IntentSpec& spec,
                           const std::string& text,
                           const std::string& text_lower) {
  for (const auto& ex : spec.route_excludes) {
    if (ex == "model_status" && is_model_runtime_query(text_lower)) return true;
    if (ex == "cloud_meta" && is_cloud_meta_question(text)) return true;
    if (ex == "messaging_capability" && is_messaging_capability_query(text_lower)) return true;
    if (ex == "supported_languages" && is_supported_languages_query(text)) return true;
  }
  (void)text;
  return false;
}

bool intent_predicate_matched(const IntentSpec& spec,
                                const std::string& text,
                                const std::string& text_lower) {
  const std::string& p = spec.predicate;
  if (p == "short_ack") return is_bare_conversation_ack(text_lower);
  if (p == "messaging_capability") return is_messaging_capability_query(text_lower);
  if (p == "onnx_knowledge") return is_onnx_knowledge_query(text_lower);
  if (p == "supported_languages") return is_supported_languages_query(text);
  (void)text;
  (void)spec;
  return false;
}

bool intent_triggered(const IntentSpec& spec,
                      const std::string& classified,
                      const std::string& text,
                      const std::string& text_lower) {
  if (!spec.predicate.empty()) return intent_predicate_matched(spec, text, text_lower);
  const bool keyword_hit =
      !spec.keyword_key.empty() && contains_any_policy(text_lower, spec.keyword_key);
  const bool intent_hit = (classified == spec.name);
  return keyword_hit || intent_hit;
}

/// 本地翻译：通过 ModelPool 级联（Qwen → Gemma），第一个成功即返回。
/// 无本地模型或全部失败返回空字符串。
std::string translate_local(const std::string& text, const std::string& target_desc) {
  std::string prompt = "Translate exactly into " + target_desc +
                       ". Return only the translation, no explanation:\n" + text;
  auto result = thin_agent::local::ModelPool::instance().cascade("translate", prompt, 128);
  return result.ok ? result.output : "";
}

/// 失败保留粗稿原文。
std::string refine_local(const std::string& draft, const std::string& target_desc,
                                 const std::string& original) {
  std::string prompt = "Polish this translation to sound more natural in " +
                       target_desc +
                       ". Original: " + original +
                       "\nDraft translation: " + draft +
                       "\nReturn only the polished translation:";
  auto result = thin_agent::local::ModelPool::instance().cascade("translate", prompt, 256);
  return result.ok ? result.output : draft;
}

/// target_lang=="other" 时用 target_sample（用户原话）向模型描述目标语种。
/// 本地优先级联：短文本 (<=50字) Qwen→Gemma 直出，长文本粗稿→精修链。
/// 本地全部失败则走云（fast→main）；云端也无则静默返回原文。
std::vector<std::string> translate_texts(const DemoConfigCompat& cfg,
                                         const std::vector<std::string>& texts,
                                         const std::string& target_lang,
                                         const std::string& target_sample = "") {
  if (texts.empty()) return texts;

  const std::string target_desc = language_display_name(target_lang);
  const bool is_short = texts.size() == 1 && texts[0].size() <= 50;

  // ── 1. 尝试本地翻译 ──
  bool has_local = !thin_agent::local::ModelPool::instance().match("translate").empty();
  if (has_local) {
    if (is_short) {
      std::string out = translate_local(texts[0], target_desc);
      if (!out.empty()) return {out};
    } else {
      std::string draft = translate_local(texts[0], target_desc);
      if (!draft.empty()) {
        std::string polished = refine_local(draft, target_desc, texts[0]);
        return {polished};
      }
    }
  }

  // ── 2. 本地全部失败 → 云端兜底 ──
  std::string api_key;
  if (!cfg.api_key_env.empty()) {
    const char* v = std::getenv(cfg.api_key_env.c_str());
    if (v && *v) api_key = v;
  }
  if (api_key.empty()) return texts;

  std::string system_prompt =
      "You are a translator. Return only a JSON array of translated strings, "
      "same length and order as the input, no extra text.";
  std::string payload;
  if (!target_desc.empty()) {
    payload = "Translate each item into " + target_desc + ".\n";
  } else if (!target_sample.empty()) {
    payload = "Translate each item into the same language as this reference text: \""
              + target_sample + "\".\n";
  } else {
    payload = "Translate each item into the user's language.\n";
  }
  for (size_t i = 0; i < texts.size(); ++i)
    payload += std::to_string(i + 1) + ". " + texts[i] + "\n";

  const std::vector<ChatMessage> messages = {
      ChatMessage{"system", system_prompt},
      ChatMessage{"user", payload},
  };

  // 优先 fast（快），失败回退 main
  auto fast_model = fast_path_config().model;
  DemoConfigCompat fast_cfg = cfg;
  fast_cfg.model_name = fast_model.empty() ? "glm-4.5-flash" : fast_model;
  // v0.53.16: 超时对齐 chat_policy fast_path.timeout_ms——此前硬编码 10s
  // 与注释"超时对齐配置"背离（配置 60s 从未生效），flash 慢窗口下必超时
  // 三连熔断（今天 GLM 容量窗口反复的隐藏根因之一）。
  fast_cfg.request_timeout_ms = fast_path_config().timeout_ms > 0
                                    ? fast_path_config().timeout_ms
                                    : 10000;

  CurlHttpClient http(fast_cfg.request_timeout_ms);  // v0.52.3: 超时对齐配置
  auto cloud = CloudLlmClient::chat_completion_with_fallback(
      http, fast_cfg, messages, nullptr);
  if (!cloud.ok) {
    cloud = CloudLlmClient::chat_completion_with_fallback(
        http, cfg, messages, "THIN_AGENT_TEST_CLOUD_TRANSLATE_RESPONSE");
  }
  if (!cloud.ok) return texts;
  try {
    auto j = nlohmann::json::parse(cloud.text);
    if (j.is_array() && j.size() >= texts.size()) {
      std::vector<std::string> out;
      for (size_t i = 0; i < texts.size(); ++i)
        out.push_back(j[i].is_string() ? j[i].get<std::string>() : texts[i]);
      return out;
    }
  } catch (...) {}
  return texts;
}

// v0.40.0: 工具执行结果 — 用于并行调度
struct ToolExecResult {
  std::string name;
  std::string id;
  std::string output;
  bool success = false;
  std::string args_snippet;
};

// v0.49.0: HITL 危险工具分级已提升至 PathValidator（tool_risk_of / tool_risk_str），
// FC 循环与 AgentLoop 共用，避免两处维护同一张表。

// v0.40.0: 执行单个工具调用（纯函数，无副作用）
// 将核心执行逻辑从 run_function_calling_loop 中提取出来，
// 使并行调度成为可能。
ToolExecResult execute_one_tool_impl(
    const CloudLlmClient::ParsedToolCall& tc,
    SkillRegistry& skill_registry,
    EventCallback on_event,
    bool use_zh_progress) {
  ToolExecResult r;
  r.name = tc.name;
  r.id = tc.id;
  r.args_snippet = utf8_truncate(tc.arguments.dump(), 200);

  // v0.52.29: tool_pre 钩子——可否决（deny→工具不执行，理由喂回模型）。
  // 无钩子注册时 dispatch 为 O(1) 直通，零开销。
  {
    nlohmann::json hp;
    hp["tool"] = tc.name;
    hp["args"] = tc.arguments;
    auto verdict = HookSystem::instance().dispatch(HookEvent::ToolPre, hp);
    if (verdict.deny) {
      r.output = "错误: 该操作被钩子拒绝 (tool_pre deny)。原因: " +
                 (verdict.reason.empty() ? "unspecified" : verdict.reason);
      r.success = false;
      return r;
    }
  }

  try {
    if (tc.name == "list_dir") {
      // v0.54.33: 改走 cpp handler（filesystem 遍历），不再拼 `ls -la` 字符串。
      std::string path = tc.arguments.value("path", tc.arguments.value("dir", "."));
      if (on_event) on_event("thinking", {{"tier", 3}, {"msg", "Listing " + path}});
      auto cpp_result = skill_registry.dispatch_cpp("list_dir", tc.arguments);
      r.output = cpp_result.dump();
      r.success = cpp_result.value("success", false);
      return r;
    } else if (tc.name == "read_file") {
      // v0.49.1: 改走 safe_read_file_paged（不进 shell，消除路径注入 + 统一路径校验）
      std::string path = tc.arguments.value("path", "");
      if (on_event) on_event("thinking", {{"tier", 3}, {"msg", "Reading " + path}});
      int offset = tc.arguments.value("offset", 1);
      int limit = json_coerce_int(tc.arguments, "limit", 500);
      auto rr = safe_read_file_paged(path, offset, limit);
      if (!rr.success) {
        r.output = "read_file failed: " + rr.error;
        return r;
      }
      nlohmann::json rj;
      rj["success"] = true;
      rj["output"] = rr.output;
      rj["total_lines"] = rr.total_lines;
      r.output = rj.dump();
      r.success = true;
      return r;
    } else if (tc.name == "write_file") {
      std::string path = tc.arguments.value("path", "");
      std::string content = tc.arguments.value("content", "");
      if (on_event) on_event("thinking", {{"tier", 3}, {"msg", "Writing " + path}});

      auto wr = safe_write_file(path, content, {.verify = false, .syntax_check = false});
      if (!wr.success) {
        r.output = "write_file failed: " + wr.error;
      } else {
        r.output = "write_file success: " + wr.normalized_path;
        r.success = true;
      }
      return r;
    } else if (tc.name == "search_code") {
      // v0.54.33: 改走 cpp handler（纯 C++ 遍历），不再拼 grep 字符串。
      if (on_event) on_event("thinking", {{"tier", 3}, {"msg", "Searching " + tc.arguments.value("pattern", "")}});
      auto cpp_result = skill_registry.dispatch_cpp("search_code", tc.arguments);
      r.output = cpp_result.dump();
      r.success = cpp_result.value("success", false);
      return r;
    } else {
      // SkillRegistry dispatch (plugins / cpp handlers)
      auto cpp_result = skill_registry.dispatch_cpp(tc.name, tc.arguments);
      bool is_unknown = cpp_result.contains("error") &&
                        cpp_result["error"].is_string() &&
                        cpp_result["error"].get<std::string>().find("no_cpp_handler:") == 0;
      if (cpp_result.is_object() && !is_unknown) {
        r.output = cpp_result.dump();
        r.success = true;
        if (cpp_result.contains("success") && cpp_result["success"].is_boolean())
          r.success = cpp_result["success"].get<bool>();
        else if (cpp_result.contains("error") && cpp_result["error"].is_string())
          r.success = false;
        return r;
      }
      r.output = "Unknown tool: " + tc.name;
      return r;
    }
    // v0.54.34: list_dir/read_file/write_file/search_code 均已 return；
    // 其余走 dispatch_cpp。旧「拼 cmd + 裸 popen」死路径已删除。
  } catch (const std::exception& e) {
    r.output = std::string("Execution error: ") + e.what();
  }

  return r;
}

/// v0.52.30: hook 事件名解析（handler 入参用）。未知返回 false。
bool parse_hook_event(const std::string& name,
                             thin_agent::HookEvent* out) {
  using HE = thin_agent::HookEvent;
  static const std::unordered_map<std::string, HE> kMap = {
      {"session_start", HE::SessionStart}, {"session_end", HE::SessionEnd},
      {"fc_start", HE::FcStart},           {"fc_end", HE::FcEnd},
      {"tool_pre", HE::ToolPre},           {"tool_post", HE::ToolPost},
      {"tool_error", HE::ToolError},       {"message_in", HE::MessageIn},
  };
  auto it = kMap.find(name);
  if (it == kMap.end()) return false;
  if (out) *out = it->second;
  return true;
}

/// v0.52.29: execute_one_tool wrapper——tool_pre（impl 内已做）+
/// tool_post/tool_error（统一出口收口，含所有早退路径）。
ToolExecResult execute_one_tool(
    const CloudLlmClient::ParsedToolCall& tc,
    SkillRegistry& skill_registry,
    EventCallback on_event,
    bool use_zh_progress) {
  auto r = execute_one_tool_impl(tc, skill_registry, on_event, use_zh_progress);
  // 钩子 deny 的输出不再触发 tool_error（deny 本身是正常裁决路径）
  nlohmann::json hp;
  hp["tool"] = tc.name;
  hp["success"] = r.success;
  hp["output_bytes"] = r.output.size();
  if (r.output.rfind("错误: 该操作被钩子拒绝", 0) == 0) {
    hp["denied_by_hook"] = true;
    HookSystem::instance().dispatch(HookEvent::ToolPost, hp);  // 仍通知观察者
    return r;
  }
  HookSystem::instance().dispatch(
      r.success ? HookEvent::ToolPost : HookEvent::ToolError, hp);
  return r;
}

/// v0.49.0: HITL 审批检查回调。
/// FC 循环在执行 Dangerous 工具前调用；返回 true=批准执行，false=暂停。
/// AgentService 的实现：记录 pending 状态 → 推送审批事件 → 返回 false。
/// FC 循环收到 false 后立即返回 needs_approval 结果，等 chat_approve 重入。
using ApprovalChecker = std::function<bool(const std::string& tool_name,
                                            const nlohmann::json& args)>;

/// ── Native function calling loop ──
/// Replaces the legacy pipeline JSON pre-planning with streaming
/// OpenAI-compatible function calling. The LLM calls tools directly,
/// we execute locally, and feed results back. Repeats until text reply.
CloudChatResult run_function_calling_loop(
    const DemoConfigCompat& cfg,
    const std::string& api_key,
    const std::vector<ChatMessage>& initial_messages,
    const nlohmann::json& tools,
    const std::string& /*session_id*/,
    const std::string& /*idem_token*/,
    EventCallback on_event,
    SkillRegistry& skill_registry,
    int min_iterations = 3,     // 最低迭代轮数（提前终止阈值）
    int max_iterations = 30,    // 自适应上限（由调用方根据任务复杂度传入）
    bool use_zh_progress = true,
    std::function<void(const std::string& action, const std::string& params_summary, const std::string& output_truncated)> on_tool_result = nullptr,
    std::function<bool()> abort_checker = nullptr,  // v0.47.3: 流式中断检查
    ApprovalChecker approval_checker = nullptr,   // v0.49.0: HITL 危险工具审批
    /// v0.52.26: FC journal 落盘钩子——每个工具边界后回调
    /// (iter, 新增消息快照 JSON)。空函数=不落盘（兼容旧路径）。
    std::function<void(int, const nlohmann::json&)> journal_append
        = nullptr,
    /// v0.53.30: 最终轮流式回放——SSE 网络层攒批的现实下，最终文本轮的
    /// chunks 在此逐个回放（近似流式；真逐 token 需 SSE 回调化改造=优化项）。
    StreamCallback on_chunk = nullptr) {
  CloudChatResult out;

  try {
  // v0.52.29: fc_start 钩子（goal 摘要=user 视角最后一条消息）
  {
    nlohmann::json hp;
    std::string goal;
    for (auto it = initial_messages.rbegin();
         it != initial_messages.rend(); ++it) {
      if (it->role == "user" && !it->content.empty()) {
        goal = it->content.substr(0, 200);
        break;
      }
    }
    hp["goal"] = goal;
    hp["messages"] = initial_messages.size();
    HookSystem::instance().dispatch(HookEvent::FcStart, hp);
  }
  // v0.52.29: fc_end RAII 收口——覆盖全部 return 路径（含循环终止/收敛/
  // 异常），析构时分发。iterations/结论在 return 前已写入 out，guard 引用之。
  struct FcEndHookGuard {
    const CloudChatResult& res;
    ~FcEndHookGuard() {
      nlohmann::json hp;
      hp["ok"] = res.ok;
      hp["iterations"] = res.fc_iterations_used;
      hp["text_len"] = res.text.size();
      hp["needs_approval"] = res.needs_approval;
      HookSystem::instance().dispatch(HookEvent::FcEnd, hp);
    }
  } fc_end_guard{out};
  std::vector<ChatMessage> messages = initial_messages;
  std::string tool_results_log;  // accumulate executed tool results for fallback summary
  // v0.52.1: 被自动拒绝的危险工具（cron 无人值守）——本轮执行完后以
  // 错误结果形式喂回模型，让模型感知"该操作被拒"并换安全路径收尾。
  // (name, args, tool_call_id)
  std::vector<std::tuple<std::string, std::string, std::string>> denied_tool_results;

  // ── v0.28.0: 三基础设施 — Token 预算 + 收敛追踪 + 中间压缩 ──
  // v0.52.25: 窗口按配置/模型推断，不再硬编码（旧注释 DeepSeek V4 Pro 已过时）
  TokenBudget token_budget(resolve_context_window(cfg));
  // 估算 system prompt（取 initial_messages 前几条，通常 system + 上下文）
  int system_est = 0;
  int init_count = std::min(3, static_cast<int>(initial_messages.size()));
  for (int i = 0; i < init_count; ++i)
    system_est += TokenBudget::estimate_message(initial_messages[i]);
  token_budget.set_system(system_est);

  ConvergenceTracker conv_tracker;
  int compression_iter = -1;   // 上次压缩的迭代轮次
  std::string last_hint_type;  // 上次注入的压力提示类型（避免重复）

  int consecutive_no_tools = 0;         // 连续无工具调用计数（提前终止）
  int tools_in_recent_window = 0;       // 最近窗口内的工具执行数（自动延期）
  static const int kRecentWindow = 3;   // 自动延期检测窗口
  static const int kExtendBy = 5;       // 每次延期的轮数
  int extend_count = 0;                 // 已延期次数
  static const int kMaxExtends = 3;     // 最多延期次数
  int effective_max = max_iterations;   // 当前有效上限（可能被延期扩展）

  for (int iter = 0; iter < effective_max; ++iter) {
    // v0.47.3: 流式中断检查 — 用户喊停时返回部分结果
    if (abort_checker && abort_checker()) {
      log_event("fc-loop", LogLevel::Warn, "ABORTED by user", {{"iter", iter}});
      if (!tool_results_log.empty()) {
        out.text = "已中断（用户停止）。\n\n已完成的操作：\n" + tool_results_log;
      } else {
        out.text = "已中断（用户停止）。";
      }
      out.ok = true;
      out.http_status = 200;
      return out;
    }
    if (on_event) {
      log_event("fc-loop", LogLevel::Debug, "iter start", {{"iter", iter}});
    }
    std::vector<std::string> stream_chunks;
    CurlHttpClient http(cfg.request_timeout_ms);  // v0.52.3: 超时对齐配置
    // v0.53.33: 真逐 token——SSE 网络到达即经 on_chunk 转发（首字延迟=
    // 网络+模型首个 token）。降级（mock/非 curl）时由最终轮回放兜底。
    bool pushed_tokens_this_iter = false;
    bool push_aborted = false;  // v0.53.36: /stop 后在途 SSE 的 token 不再推给客户端
    std::function<void(const std::string&)> token_pusher;
    if (on_chunk) {
      token_pusher = [&on_chunk, &pushed_tokens_this_iter, &push_aborted,
                      &abort_checker](const std::string& t) {
        if (push_aborted) return;
        if (abort_checker && abort_checker()) { push_aborted = true; return; }
        pushed_tokens_this_iter = true;
        on_chunk(t, false);
      };
    }
    // v0.53.38: 思考流透传——reasoning_content 增量→on_event("thinking")
    //（此前客户端 thinking 帧是假进度文案,思考型模型真思考内容被丢弃）
    std::function<void(const std::string&)> reasoning_pusher;
    if (on_event) {
      reasoning_pusher = [&on_event](const std::string& r) {
        on_event("thinking", {{"tier", 3}, {"content", r}, {"live", true}});
      };
    }
    // v0.53.8: 工具链多源 fallback（cloud_providers 空时与直调等价零开销）
    auto result = CloudLlmClient::chat_completion_with_tools_fallback(
        http, cfg, api_key, messages, tools, true, &stream_chunks, token_pusher,
        reasoning_pusher);

    // v0.47.1: 累积本次 FC 循环的总 usage 到 out（调用方再累加到全局计数器）
    if (result.ok && result.usage.total_tokens > 0) {
      out.usage.prompt_tokens += result.usage.prompt_tokens;
      out.usage.completion_tokens += result.usage.completion_tokens;
      out.usage.total_tokens += result.usage.total_tokens;
    }

    if (!result.ok) {
      // LLM 调用失败 — 如果已经执行过工具，返回部分结果而非完全丢弃
      if (!tool_results_log.empty()) {
        out.text = "Partial results (LLM call failed after tool execution):\n" + tool_results_log;
        out.ok = true;
        out.http_status = result.http_status;
        return out;
      }
      out.error = result.error;
      out.http_status = result.http_status;
      return out;
    }

    // Try to parse tool_calls from response
    auto tool_calls = CloudLlmClient::parse_tool_calls(result.text);
    log_event("fc-loop", LogLevel::Debug, "iter done",
              {{"iter", iter}, {"tool_calls", tool_calls.size()},
               {"text_len", result.text.size()}});
    // v0.53.30: 最终文本轮回放流式 chunk（此前云端 FC 全程零流式——
    // stream_chunks 收完即弃，zing/客户端的流式 UI 全是摆设路径）
    if (on_chunk && tool_calls.empty() && !stream_chunks.empty() &&
        !pushed_tokens_this_iter) {  // v0.53.33: 真逐 token 已推过则不回放（防双份）
      bool aborted_mid = false;
      for (const auto& c : stream_chunks) {
        if (abort_checker && abort_checker()) { aborted_mid = true; break; }  // v0.53.30: 回放可中断
        on_chunk(c, false);
      }
      if (!aborted_mid && !result.text.empty()) on_chunk("", true);
      if (aborted_mid) {
        // 对齐既有中断协议：文本标记（chat_result 里用户可见）
        out.text = "已中断（用户停止）。\n\n已生成的部分内容：\n" + result.text;
        out.ok = true;
        out.http_status = 200;
        out.aborted = true;
        return out;
      }
    }

    if (tool_calls.empty()) {
      // No tool_calls → text answer is final
      consecutive_no_tools++;
      // v0.53.33: 真逐 token 流的终结帧——增量推送路径（未走回放分支）
      if (on_chunk && pushed_tokens_this_iter) on_chunk("", true);
      // 提前终止：连续 2 轮无工具调用 且 已达最低迭代
      if (consecutive_no_tools >= 2 && iter >= min_iterations) {
        if (on_event) {
          on_event("thinking", {{"tier", 3}, {"msg", progress_msg("summarizing", use_zh_progress)}});
        }
        try {
          auto jr = nlohmann::json::parse(result.text);
          const auto& choices = jr["choices"];
          if (choices.is_array() && !choices.empty()) {
            const auto& msg = choices[0]["message"];
            if (msg.contains("content") && msg["content"].is_string()) {
              out.text = msg["content"].get<std::string>();
            }
          }
        } catch (...) {
          out.text = result.text;
        }
        if (out.text.empty()) out.text = result.text;
        out.ok = true;
        out.http_status = result.http_status;
        return out;
      }
      if (on_event) {
        on_event("thinking", {{"tier", 3}, {"msg", progress_msg("summarizing", use_zh_progress)}});
      }
      // result.text might be a wrapped JSON; extract just the content
      try {
        auto jr = nlohmann::json::parse(result.text);
        const auto& choices = jr["choices"];
        if (choices.is_array() && !choices.empty()) {
          const auto& msg = choices[0]["message"];
          if (msg.contains("content") && msg["content"].is_string()) {
            out.text = msg["content"].get<std::string>();
          }
        }
      } catch (...) {
        out.text = result.text;
      }
      // If empty after extraction, use raw
      if (out.text.empty()) {
        out.text = result.text;
      }
      out.ok = true;
      out.http_status = result.http_status;
      return out;
    }

    // 有工具调用 — 重置提前终止计数器
    consecutive_no_tools = 0;
    tools_in_recent_window++;

    // v0.49.0: HITL 审批 — 执行前检查是否有 Dangerous 工具
    // v0.52.1: 审批回调语义扩展——返回 false 时，FC 用 pa 携带的模式
    // 决定行为：交互会话 = 暂停等 chat_approve；cron 无人值守 = 从本轮
    // tool_calls 剔除该工具并以"已自动拒绝"错误结果喂回模型（任务不
    // 烂尾，模型可换安全路径收尾）。通过 thread_local 拒绝标记传递。
    if (approval_checker) {
      for (auto it = tool_calls.begin(); it != tool_calls.end();) {
        if (tool_risk_of(it->name, it->arguments) != ToolRisk::Dangerous) {
          ++it;
          continue;
        }
        const bool approved = approval_checker(it->name, it->arguments);
        if (approved) {
          ++it;
          continue;
        }
        if (g_fc_dangerous_denied) {  // v0.52.1: cron 无人值守自动拒绝
          g_fc_dangerous_denied = false;
          log_event("hitl", LogLevel::Warn, "FC denied tool removed from batch",
                    {{"tool", it->name}, {"iter", iter}});
          denied_tool_results.emplace_back(it->name, it->arguments.dump(), it->id);
          it = tool_calls.erase(it);
          continue;
        }
        // 交互会话：暂停等待用户确认
        out.ok = true;
        out.http_status = 200;
        out.needs_approval = true;
        out.pending_tool_name = it->name;
        out.pending_tool_args = it->arguments.dump();
        out.text = "⏸️ 危险操作需要确认: " + it->name +
                   "\n参数: " + utf8_truncate(it->arguments.dump(), 300) +
                   "\n\n回复「批准」或「取消」。";
        // v0.52.2: 方案 C — 快照暂停点消息（含本轮 assistant tool_calls，
        // 与下方"Add assistant message"同构）+ 迭代进度，供 approve 续跑
        out.fc_messages_snapshot = messages;
        {
          ChatMessage pause_msg;
          pause_msg.role = "assistant";
          pause_msg.content = "";
          nlohmann::json pause_tcs = nlohmann::json::array();
          int tc_idx = 0;
          for (const auto& tc2 : tool_calls) {
            pause_tcs.push_back({
                {"id", tc2.id.empty() ? "call_pause_" + std::to_string(tc_idx) : tc2.id},
                {"type", "function"},
                {"function", {{"name", tc2.name},
                              {"arguments", tc2.arguments.dump()}}}});
            ++tc_idx;
          }
          pause_msg.tool_calls = pause_tcs;
          out.fc_messages_snapshot.push_back(std::move(pause_msg));
        }
        out.fc_iterations_used = iter;
        return out;
      }
    }

    // Add assistant message with tool_calls
    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    nlohmann::json assistant_tool_calls = nlohmann::json::array();
    try {
      auto jr = nlohmann::json::parse(result.text);
      const auto& choices = jr["choices"];
      if (choices.is_array() && !choices.empty()) {
        const auto& msg = choices[0]["message"];
        if (msg.contains("content") && msg["content"].is_string()) {
          assistant_msg.content = msg["content"].get<std::string>();
        }
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
          assistant_tool_calls = msg["tool_calls"];
        }
      }
    } catch (...) {}
    assistant_msg.tool_calls = assistant_tool_calls;
    messages.push_back(assistant_msg);

    // v0.52.26: journal 计数基线（本轮 push 追踪）
    size_t out_tool_count = 0;
    const size_t denied_count_snapshot = denied_tool_results.size();

    // v0.40.0: 并行执行所有 tool call（同轮 LLM 响应无依赖）
    // 当 tool_calls > 1 时通过 std::async 并行执行，否则走快速路径
    {
      struct ToolBatchResult {
        ToolExecResult exec;
        size_t index;
      };

      if (tool_calls.size() > 1) {
        // v0.53.57: 批量工具并发上限——此前全量 std::async,LLM 一轮
        /// 20 个 code_exec=20 沙箱同起(v0.52.10 实测 22 核打满);
        /// C++17 无 counting_semaphore,mutex+cv 计数闸限 4
        /// (重型工具排队,轻型工具不受影响)
        constexpr int kMaxParallelTools = 4;
        std::mutex slot_mu;
        std::condition_variable slot_cv;
        int active_slots = 0;
        auto acquire_slot = [&]() {
          std::unique_lock<std::mutex> lk(slot_mu);
          slot_cv.wait(lk, [&] { return active_slots < kMaxParallelTools; });
          ++active_slots;
        };
        auto release_slot = [&]() {
          {
            std::lock_guard<std::mutex> lk(slot_mu);
            --active_slots;
          }
          slot_cv.notify_one();
        };
        std::vector<std::future<ToolBatchResult>> futures;
        for (size_t i = 0; i < tool_calls.size(); ++i) {
          futures.push_back(std::async(std::launch::async, [&, i]() -> ToolBatchResult {
            acquire_slot();
            struct SlotGuard {
              std::function<void()> rel;
              ~SlotGuard() { rel(); }
            } slot_guard{release_slot};
            log_event("fc-tool", LogLevel::Info, tool_calls[i].name,
                      {{"iter", iter}, {"args", tool_calls[i].arguments}});
            return {
              execute_one_tool(tool_calls[i], skill_registry, on_event, use_zh_progress),
              i
            };
          }));
        }
        // 收集结果、按序处理
        std::vector<ToolBatchResult> results;
        results.reserve(futures.size());
        for (auto& f : futures) results.push_back(f.get());
        // 按原始顺序排序（保险：std::async 不保证完成顺序）
        std::sort(results.begin(), results.end(),
                  [](const auto& a, const auto& b) { return a.index < b.index; });

        for (const auto& r : results) {
          auto tr = r.exec;
          out.tool_names_used.push_back(tr.name);
          ChatMessage tool_msg;
          tool_msg.role = "tool";
          tool_msg.tool_call_id = tr.id;
          tool_msg.name = tr.name;
          tool_msg.content = tr.output;
          messages.push_back(tool_msg);
          ++out_tool_count;  // v0.52.26: journal 计数
          tool_results_log += "\n[" + tr.name + "] " + utf8_truncate(tr.output, 500);
          conv_tracker.record(tr.name, tr.args_snippet, tr.success);
          if (on_tool_result)
            on_tool_result(tr.name, utf8_truncate(tr.args_snippet, 100), utf8_truncate(tr.output, 500));
        }
      } else {
        // 单工具：串行（零开销）
        for (size_t i = 0; i < tool_calls.size(); ++i) {
          const auto& tc = tool_calls[i];
          log_event("fc-tool", LogLevel::Info, tc.name,
                    {{"iter", iter}, {"args", tc.arguments}});
          auto tr = execute_one_tool(tc, skill_registry, on_event, use_zh_progress);
          out.tool_names_used.push_back(tr.name);
          ++out_tool_count;  // v0.52.26: journal 计数
          ChatMessage tool_msg;
          tool_msg.role = "tool";
          tool_msg.tool_call_id = tr.id;
          tool_msg.name = tr.name;
          tool_msg.content = tr.output;
          messages.push_back(tool_msg);
          tool_results_log += "\n[" + tr.name + "] " + utf8_truncate(tr.output, 500);
          conv_tracker.record(tr.name, tr.args_snippet, tr.success);
          if (on_tool_result)
            on_tool_result(tr.name, utf8_truncate(tr.args_snippet, 100), utf8_truncate(tr.output, 500));
        }
      }
    }

    // v0.52.1: 被自动拒绝的危险工具以错误结果喂回（cron 无人值守——
    // 模型感知拒绝原因后可换安全路径收尾，任务不烂尾）。tool_call_id
    // 对齐 assistant 消息的 tool_calls（OpenAI 协议要求配对）。
    for (const auto& [dname, dargs, did] : denied_tool_results) {
      ChatMessage tool_msg;
      tool_msg.role = "tool";
      tool_msg.tool_call_id = did;
      tool_msg.name = dname;
      tool_msg.content = "错误: 该操作被自动拒绝（无人值守定时任务不允许执行"
                         "危险工具）。请改用安全方式完成任务或说明限制。";
      messages.push_back(tool_msg);
      tool_results_log += "\n[" + dname + "] DENIED (unattended cron)";
    }
    denied_tool_results.clear();

    // v0.52.26: 断点续跑 journal——工具边界收敛点增量落盘（本轮新增：
    // assistant tool_calls 消息 + 全部 tool 结果消息）。快照在压缩前取，
    // 保证 resume 拿到的是未截断原文。
    if (journal_append) {
      nlohmann::json boundary_msgs = nlohmann::json::array();
      // 本轮 push_back 的消息数：assistant(1) + tool_results(n) + denied(n)
      // 回溯收集（messages 只增不删——压缩只改 content 不删条目，
      // 回溯安全；边界条数按本轮实际 push 数计算）
      size_t pushed = 1 + out_tool_count + denied_count_snapshot;
      if (pushed > messages.size()) pushed = messages.size();
      for (size_t k = messages.size() - pushed; k < messages.size(); ++k) {
        const auto& m = messages[k];
        nlohmann::json jm;
        jm["role"] = m.role;
        jm["content"] = m.content;
        if (!m.tool_call_id.empty()) jm["tool_call_id"] = m.tool_call_id;
        if (!m.name.empty()) jm["name"] = m.name;
        if (!m.tool_calls.empty()) jm["tool_calls"] = m.tool_calls;
        boundary_msgs.push_back(std::move(jm));
      }
      journal_append(iter + 1, boundary_msgs);
    }

    // ── v0.28.0: Token 预算 + 中间压缩 + 压力信号 ──────────
    token_budget.recalc(messages);
    token_budget.add_call();
    std::cerr << token_budget.status_line() << std::endl;
    std::cerr << conv_tracker.status_line() << std::endl;

    // 中间压缩：token 使用 >70% 时，截断旧工具结果（每次压缩间隔 ≥3 轮）
    if (token_budget.is_high() && iter > compression_iter + 2) {
      // 统计 assistant 消息数，跳过最近 2 个 assistant turn 的工具结果
      int assistant_count = 0;
      for (const auto& m : messages)
        if (m.role == "assistant") assistant_count++;
      int skip_assistant = std::max(0, assistant_count - 2);

      int compressed = 0, seen_assistant = 0;
      for (auto& msg : messages) {
        if (msg.role == "assistant") seen_assistant++;
        if (msg.role != "tool") continue;
        if (seen_assistant > skip_assistant) break;  // 保留最近的
        if (msg.content.size() > 250) {
          int orig = static_cast<int>(msg.content.size());
          msg.content = utf8_truncate(msg.content, 200)
                      + "\n...[compressed from " + std::to_string(orig) + " chars]";
          compressed++;
        }
      }
      if (compressed > 0) {
        compression_iter = iter;
        token_budget.recalc(messages);
        log_event("fc-loop", LogLevel::Info, "compressed tool outputs",
                  {{"compressed", compressed}, {"budget", token_budget.status_line()}});
      }
    }

    // 压力信号注入：收敛检测发现问题时，注入 system 提醒
    {
      std::string hint_type;
      if (conv_tracker.is_severe_loop()) hint_type = "severe_loop";
      else if (conv_tracker.is_looping()) hint_type = "looping";
      else if (conv_tracker.is_wrong_direction()) hint_type = "wrong_dir";
      else if (conv_tracker.is_stalled() && token_budget.is_high()) hint_type = "stalled_high";

      // v0.37.2: 严重循环 → 强制终止 FC，返回部分结果
      if (hint_type == "severe_loop") {
        log_event("fc-loop", LogLevel::Error, "SEVERE LOOP detected — terminating FC",
                  {{"repeat_streak", conv_tracker.repeat_streak}});
        out.text = "检测到工具调用循环（连续重复相同操作），已终止。\n\n已完成的操作：\n" + tool_results_log;
        if (out.text.empty()) out.text = "工具调用循环终止，无可用结果。";
        out.ok = true;
        out.http_status = 200;
        return out;
      }

      // v0.37.3: 高错误率 + 已尝试足够多轮 → 强制终止
      // （GLM 不断换不同工具但一直失败，如缺 g++/python 等环境依赖）
      if (conv_tracker.total_calls >= 8 && conv_tracker.error_rate() > 0.60f) {
        log_event("fc-loop", LogLevel::Error, "HIGH ERROR RATE — terminating FC",
                  {{"calls", conv_tracker.total_calls},
                   {"error_rate_pct", static_cast<int>(conv_tracker.error_rate() * 100)}});
        out.text = "工具调用持续失败（错误率 "
                 + std::to_string(static_cast<int>(conv_tracker.error_rate() * 100))
                 + "%），可能缺少环境依赖，已终止。\n\n已尝试的操作：\n" + tool_results_log;
        if (out.text.empty()) out.text = "工具调用持续失败，已终止。";
        out.ok = true;
        out.http_status = 200;
        return out;
      }

      if (!hint_type.empty() && hint_type != last_hint_type && iter > 1) {
        last_hint_type = hint_type;
        std::string hint = conv_tracker.pressure_hint();
        if (!hint.empty()) {
          ChatMessage hint_msg;
          hint_msg.role = "user";
          hint_msg.content = "[系统提醒 — 检测到工具调用可能重复] "
                           + hint
                           + " 请更换工具或调整参数，不要重复相同的操作。";
          messages.push_back(hint_msg);
          log_event("fc-loop", LogLevel::Warn, "pressure hint",
                    {{"hint", hint_type}});
        }
      }
    }

    // Token 预算驱动的迭代上限收紧（可选：上下文近乎耗尽时禁止再延期）
    if (token_budget.is_critical() && extend_count == 0) {
      log_event("fc-loop", LogLevel::Warn, "token budget critical, capping extensions");
      // 不直接改 effective_max，但后续延期逻辑尊重 token 预算
    }

    // ── 自动延期：离上限 ≤3 轮 且 最近窗口内有工具执行 → 扩展预算 ──
    if (iter >= effective_max - kRecentWindow && tools_in_recent_window > 0 &&
        extend_count < kMaxExtends) {
      effective_max += kExtendBy;
      extend_count++;
      tools_in_recent_window = 0;  // 重置窗口计数
      log_event("fc-loop", LogLevel::Info, "auto-extend",
                {{"iter", iter}, {"new_effective_max", effective_max},
                 {"extends", extend_count}});
    }
    // 滑动窗口：移除超出窗口的旧计数
    if (iter >= kRecentWindow) {
      // 简化实现：每 kRecentWindow 轮重置一次窗口
      if (iter % kRecentWindow == kRecentWindow - 1) {
        tools_in_recent_window = 0;
      }
    }
  }

  out.error = "max_function_calling_iterations_reached";
  if (!tool_results_log.empty()) {
    out.text = "Tool execution results:" + tool_results_log;
  }
  return out;
  } catch (const std::exception& e) {
    out.error = std::string("exception: ") + e.what();
    return out;
  } catch (...) {
    out.error = "unknown_exception";
    return out;
  }
}


}  // namespace svc_util
}  // namespace thin_agent

// v0.45.4: 文件操作类请求判定（规则引擎排除 profile 误判用）。
