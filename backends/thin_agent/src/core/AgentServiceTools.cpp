#include "thin_agent/core/FanoutLimits.h"  // v0.54.2: 扇出/预算钳制
#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/sandbox_paths.h"
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

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "thin_agent/core/AgentServiceUtil.h"

namespace thin_agent {

using namespace thin_agent::svc_util;

void AgentService::sync_skill_to_tool_registry() {
  // v0.52.19: 从构造期一次性桥接改为可重入增量同步——MCP 晚连/
  // 热注册的新工具调本函数即可进子代理工具区。幂等：已存在不覆盖。
  auto schemas = skill_registry_.build_tools_schema();
  int bridged = 0;
  for (const auto& t : schemas) {
    if (!t.contains("function")) continue;
    const auto& fn = t["function"];
    std::string name = fn.value("name", "");
    if (name.empty()) continue;
    if (agent::ToolRegistry::instance().find(name) != nullptr) continue;

    agent::ToolSchema ts;
    ts.name = name;
    ts.description = fn.value("description", name);
    if (fn.contains("parameters") && fn["parameters"].is_object()) {
      const auto& params = fn["parameters"];
      if (params.contains("properties") && params["properties"].is_object()) {
        std::unordered_set<std::string> required;
        if (params.contains("required") && params["required"].is_array()) {
          for (const auto& rq : params["required"])
            if (rq.is_string()) required.insert(rq.get<std::string>());
        }
        for (auto pit = params["properties"].begin();
             pit != params["properties"].end(); ++pit) {
          agent::ToolParameter tp;
          tp.name = pit.key();
          tp.type = pit.value().value("type", "string");
          tp.description = pit.value().value("description", "");
          tp.required = required.count(pit.key()) > 0;
          ts.parameters.push_back(tp);
        }
      }
    }
    // 桥接执行：dispatch_cpp 转发 + success→ok/平铺→result 字段适配
    ts.execute = [this, name](const nlohmann::json& p) -> nlohmann::json {
      auto raw = skill_registry_.dispatch_cpp(name, p);
      nlohmann::json out;
      bool ok = raw.value("ok", raw.value("success", false));
      out["ok"] = ok;
      if (raw.contains("result")) {
        out["result"] = raw["result"];
      } else {
        nlohmann::json rest = nlohmann::json::object();
        for (auto it = raw.begin(); it != raw.end(); ++it) {
          if (it.key() == "ok" || it.key() == "success" || it.key() == "error")
            continue;
          rest[it.key()] = it.value();
        }
        out["result"] = std::move(rest);
      }
      out["error"] = raw.value("error", "");
      return out;
    };
    agent::ToolRegistry::instance().register_tool(std::move(ts));
    ++bridged;
  }
  if (bridged > 0) {
    log_event("bridge", LogLevel::Info,
              "SkillRegistry→ToolRegistry bridged", {{"count", bridged}});
  }

  // v0.52.24: 反向同步（ToolRegistry→SkillRegistry）——双向互补
  // 收敛（#9 方案 B）。子代理侧真实注册的工具（如 v0.43 的
  // spawn_agent/delegate_task、AgentDirectory 平台工具）回流主
  // FC 循环：主循环 execute_one_tool 经 dispatch_cpp 也能调用。
  // 字段适配反向：{ok,result}→{success,...平铺}。
  int backflowed = 0;
  for (const auto& t : agent::ToolRegistry::instance().all_tools()) {
    if (skill_registry_.find_action(t.name) != nullptr) continue;
    // 描述与参数 schema 从 ToolSchema 重建 ActionDef
    skill_registry_.register_cpp_handler(t.name,
      [this, name = t.name](const nlohmann::json& p) -> nlohmann::json {
        auto raw = agent::ToolRegistry::instance().call(
            name, p, 0 /*无超时——调用方自控*/);
        nlohmann::json out;
        out["success"] = raw.ok;
        if (raw.result.is_object() && !raw.result.empty()) {
          for (auto it = raw.result.begin(); it != raw.result.end(); ++it)
            out[it.key()] = it.value();
        } else if (!raw.result.is_null()) {
          out["output"] = raw.result.dump();
        }
        if (!raw.error.empty()) out["error"] = raw.error;
        return out;
      });
    ++backflowed;
  }
  if (backflowed > 0) {
    log_event("bridge", LogLevel::Info,
              "ToolRegistry→SkillRegistry backflowed", {{"count", backflowed}});
  }
}

nlohmann::json AgentService::execute_skill_pipeline(
    const nlohmann::json& pipeline,
    const std::string& session_id,
    const std::string& idem_token,
    EventCallback on_event) {

  const std::string os = SkillRegistry::detect_os();
  nlohmann::json steps = nlohmann::json::array();
  nlohmann::json ctx = nlohmann::json::object();  // 步骤间变量上下文
  bool failed = false;
  int failed_at = -1;
  std::string fail_reason;

  if (!pipeline.is_array()) {
    return {{"ok", false}, {"error", "pipeline_not_array"}, {"steps", steps}};
  }

  for (size_t i = 0; i < pipeline.size(); ++i) {
    const auto& step = pipeline[i];
    std::string action = step.value("action", "");
    nlohmann::json params = step.contains("params") && step["params"].is_object()
                                ? step["params"] : nlohmann::json::object();

    // $vars 解析
    for (auto it = params.begin(); it != params.end(); ++it) {
      if (it.value().is_string()) {
        std::string resolved = SkillRegistry::resolve_ctx_var(it.value().get<std::string>(), ctx);
        params[it.key()] = resolved;
      }
    }

    // 解析相对路径 "." → 实际工作目录
    for (auto it = params.begin(); it != params.end(); ++it) {
      if (!it.value().is_string()) continue;
      std::string key = it.key();
      std::string val = it.value().get<std::string>();
      bool is_path_param = (key == "dir" || key == "path" || key == "file");
      if (is_path_param && (val == "." || val.rfind("./", 0) == 0)) {
        try {
          std::string cwd = std::filesystem::current_path().string();
          if (val == ".") {
            params[it.key()] = cwd;
          } else if (val.rfind("./", 0) == 0) {
            params[it.key()] = cwd + val.substr(1);  // "./foo" → "/cwd/foo"
          }
        } catch (...) {}
      }
    }

    const auto* handler = skill_registry_.find_handler(action, os);
    nlohmann::json step_result = {{"index", static_cast<int>(i + 1)},
                                   {"action", action},
                                   {"params", params},
                                   {"success", false}};

    // v0.54.33: media 动作禁止进入 skill pipeline（纵深——contract 已拦混跑，
    // 此处防止纯媒体误入后撞 cam_media stub 假成功）。
    static const std::unordered_set<std::string> kMediaActions = {
        "capture_photo", "start_recording", "stop_recording", "fetch_capture_results"};
    if (kMediaActions.count(action)) {
      step_result["error"] = "media_action_requires_task_engine";
      steps.push_back(step_result);
      failed = true;
      failed_at = static_cast<int>(i + 1);
      fail_reason = step_result["error"];
      break;
    }

    // v0.54.32: 已注册 cpp handler 时优先走 cpp（read_file/list_dir/shell_exec 等），
    // 避免 chat_policy 仍标 type=shell 时用裸 popen 绕过 CommandValidator / safe_*。
    const bool use_cpp = skill_registry_.has_cpp_handler(action) ||
                         (handler && handler->type == "cpp");

    if (!handler && !use_cpp) {
      step_result["error"] = "unknown_action:" + action;
      steps.push_back(step_result);
      failed = true;
      failed_at = static_cast<int>(i + 1);
      fail_reason = step_result["error"];
      break;
    }

    // push progress with action details
    if (on_event) {
      std::string detail;
      // v0.54.6: 改名 step_params——此前与函数外层 `params` 同名（-Wshadow=local 判官命中）。
      const auto step_params = step.value("params", nlohmann::json::object());
      if (action == "read_file" || action == "write_file") {
        detail = step_params.value("path", step_params.value("file", "?"));
      } else if (action == "list_dir") {
        detail = step_params.value("dir", step_params.value("path", "?"));
      } else if (action == "search_code") {
        detail = step_params.value("pattern", "?") + std::string(" @ ") + step_params.value("path", step_params.value("dir", "."));
      } else if (action == "search_files") {
        detail = step_params.value("pattern", step_params.value("glob", "?")) + std::string(" @ ") + step_params.value("dir", step_params.value("path", "."));
      } else if (action == "shell_exec") {
        detail = step_params.value("command", step_params.value("cmd", "?"));
      } else if (action == "capture_photo") {
        detail = step_params.value("count", "1") + std::string(" 张");
      } else if (action == "start_recording") {
        detail = step_params.value("mode", "normal");
        if (step_params.contains("duration_sec")) detail += " " + step_params["duration_sec"].dump() + "s";
      } else if (action == "stop_recording") {
        detail = "停止";
      } else {
        detail = action;
      }
      // truncate long details
      if (detail.size() > 60) detail = detail.substr(0, 57) + "...";
      static const std::unordered_map<std::string, std::string> action_emoji = {
        {"read_file", "📖"}, {"write_file", "✏️"}, {"search_code", "🔎"},
        {"search_files", "🔍"}, {"list_dir", "📂"}, {"shell_exec", "💻"},
        {"capture_photo", "📸"}, {"start_recording", "⏺️"}, {"stop_recording", "⏹️"}
      };
      auto emoji_it = action_emoji.find(action);
      std::string emoji = (emoji_it != action_emoji.end()) ? emoji_it->second : "⚙️";
      on_event("thinking", {{"tier", 2}, {"msg", emoji + " " + action + ": " + detail}, {"cmd_id", idem_token}});
    }

    if (use_cpp) {
      auto cpp_result = skill_registry_.dispatch_cpp(action, params);
      if (cpp_result.contains("error") && cpp_result["error"].is_string() &&
          cpp_result["error"].get<std::string>().find("no_cpp_handler") != std::string::npos) {
        step_result["error"] = "unknown_action:" + action;
        steps.push_back(step_result);
        failed = true;
        failed_at = static_cast<int>(i + 1);
        fail_reason = step_result["error"];
        break;
      }
      step_result["success"] = cpp_result.value("success", false);
      if (cpp_result.contains("error")) step_result["error"] = cpp_result["error"];
      if (cpp_result.contains("output")) step_result["output"] = cpp_result["output"];
      if (cpp_result.contains("exit_code")) step_result["exit_code"] = cpp_result["exit_code"];

      std::string var_name = step.contains("output") && step["output"].is_string()
                                 ? step["output"].get<std::string>() : ("step" + std::to_string(i + 1));
      ctx[var_name] = step_result.contains("output")
                          ? (step_result["output"].is_string() ? step_result["output"].get<std::string>()
                                                              : step_result["output"].dump())
                          : "";
    } else if (handler && handler->type == "shell") {
      std::string cmd = SkillRegistry::expand_vars(handler->cmd, params);

      // 白名单校验
      std::string validate_err = skill_registry_.validate_shell_command(cmd, os);
      if (!validate_err.empty()) {
        step_result["error"] = validate_err;
        steps.push_back(step_result);
        failed = true;
        failed_at = static_cast<int>(i + 1);
        fail_reason = validate_err;
        break;
      }

      // v0.54.32: 与 FC/shell_exec 路径对齐——危险命令硬阻断 + 风险分级
      std::string block_reason;
      if (!CommandValidator::is_safe(cmd, &block_reason)) {
        step_result["error"] = "blocked:" + block_reason;
        steps.push_back(step_result);
        failed = true;
        failed_at = static_cast<int>(i + 1);
        fail_reason = step_result["error"];
        break;
      }
      if (grade_shell_risk(cmd) == ToolRisk::Dangerous) {
        step_result["error"] = "blocked_by_risk_grading";
        steps.push_back(step_result);
        failed = true;
        failed_at = static_cast<int>(i + 1);
        fail_reason = step_result["error"];
        break;
      }

      // 执行 shell（带 timeout 看门狗；配置 max_exec_seconds，默认 30）
      int max_sec = 30;
      {
        std::string max_sec_str = policy_text("whitelist.common_rules.max_exec_seconds", "");
        if (!max_sec_str.empty()) {
          try { max_sec = std::stoi(max_sec_str); } catch (...) {}
        }
        if (max_sec < 1) max_sec = 1;
        if (max_sec > 120) max_sec = 120;
      }
      std::string output;
#ifndef _WIN32
      const std::string run_cmd = "timeout " + std::to_string(max_sec) + " " + cmd;
#else
      const std::string& run_cmd = cmd;
#endif
      FILE* pipe = popen(run_cmd.c_str(), "r");
      if (!pipe) {
        step_result["error"] = "popen_failed";
        steps.push_back(step_result);
        failed = true;
        failed_at = static_cast<int>(i + 1);
        fail_reason = "popen_failed";
        break;
      }

      char buf[4096];
      size_t total = 0;
      size_t max_bytes = 65536;
      while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        output += buf;
        total += strlen(buf);
        if (total > max_bytes) {
          output += "\n[output truncated at " + std::to_string(max_bytes) + " bytes]";
          break;
        }
      }
      int rc = pclose(pipe);
#ifdef _WIN32
      int exit_code = rc;
#else
      // v0.54.32: 正确解析 wait status；pclose==-1 不再当作成功
      int exit_code = (rc < 0) ? -1 : (WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
#endif

      step_result["success"] = (exit_code == 0);
      step_result["output"] = output;
      step_result["exit_code"] = exit_code;
      step_result["output_size"] = output.size();
      if (exit_code != 0) {
        step_result["error"] = "exit_code:" + std::to_string(exit_code);
      }

      // 存入上下文供下游 $vars 引用
      std::string var_name = step.contains("output") && step["output"].is_string()
                                 ? step["output"].get<std::string>() : ("step" + std::to_string(i + 1));
      ctx[var_name] = output;

      if (on_event && exit_code == 0) {
        on_event("thinking", {{"tier", 2},
                               {"msg", "✅ " + action + " 完成 (" + std::to_string(output.size()) + " 字节)"},
                               {"cmd_id", idem_token}});
      }
    } else {
      step_result["error"] = "unsupported_handler_type";
      steps.push_back(step_result);
      failed = true;
      failed_at = static_cast<int>(i + 1);
      fail_reason = step_result["error"];
      break;
    }

    steps.push_back(step_result);
    if (!step_result.value("success", false)) {
      failed = true;
      failed_at = static_cast<int>(i + 1);
      if (fail_reason.empty()) {
        fail_reason = step_result.contains("error") && step_result["error"].is_string()
                          ? step_result["error"].get<std::string>()
                          : "step_failed";
      }
      break;
    }
  }

  return {
    {"ok", !failed},
    {"pipeline_steps", steps},
    {"failed", failed},
    {"failed_at", failed_at},
    {"fail_reason", fail_reason},
    {"ctx", ctx},
  };
}

/// 注册内置 C++ 工具 handler（v0.49.1: 按域拆分为 5 个子方法，原 1160 行）
void AgentService::register_cpp_handlers() {
  register_file_shell_tools();
  register_media_agent_tools();
  register_cron_tools();
  register_skill_checkpoint_tools();
  register_meta_memory_tools();
  register_web_search_tool();
}

/// web_search 工具（v0.53.11）：FC 循环/子代理的自主搜索能力。
/// 此前搜索只在意图层生效（用户明说"搜索…"才触发 external intent）；
/// 注册为工具后 LLM 可在推理中自主决定搜索——知识盲区自补。
/// 数据源走 fetch_search("auto") 决策链：chat_policy web_search.provider
/// （WS web_search_config 运行时可改可存）→ yaml external_provider → bing 保底。
static constexpr int kWebSearchTimeoutMs = 12000;  ///< web_search 工具单次请求预算

void AgentService::register_web_search_tool() {
  skill_registry_.register_cpp_handler("web_search",
    [this](const nlohmann::json& params) -> nlohmann::json {
      std::string query = params.value("query", "");
      if (query.empty()) return {{"success", false}, {"error", "query_required"}};
      std::string lang = params.value("lang", "zh");
      int count = json_coerce_int(params, "count", 5);
      if (count < 1) count = 1;
      if (count > 8) count = 8;  // 与适配器单页上限一致

      CurlHttpClient http(kWebSearchTimeoutMs);
      auto r = ExternalInfoClient::fetch_search(http, query, lang, "auto");
      if (!r.ok) {
        return {{"success", false},
                {"error", r.error.empty() ? "search_failed" : r.error},
                {"source", r.source}, {"http_status", r.http_status}};
      }
      // results 截到 count + LLM 友好文本化
      nlohmann::json results = r.data.value("results", nlohmann::json::array());
      while (results.size() > static_cast<size_t>(count)) results.erase(results.end() - 1);
      std::string text = ExternalInfoClient::format_search_results(
          {{"query", query}, {"results", results}}, lang);
      return {
        {"success", true},
        {"query", query},
        {"source", r.source},
        {"count", results.size()},
        {"results", results},
        {"output", text}  // LLM 直读文本（子代理 prompt 友好）
      };
    });
}

/// 文件与 shell 工具（write_file/read_file/shell_exec/process/search_files）（v0.49.1 从 register_cpp_handlers 按域拆出）
void AgentService::register_file_shell_tools() {
  // v0.52.16: read_file 注册为 cpp handler——主 FC 循环里它是
  // execute_one_tool 的内置直实现，从未进注册表；子代理（经
  // ToolRegistry 桥接）调用时 no_cpp_handler:read_file（真 e2e
  // t4 审查任务实测：viewer 仅有 read_file/search_files 可用，
  // 两者全挂导致审查无法执行）。逻辑与主循环同源
  //（safe_read_file_paged，无路径注入）。
  skill_registry_.register_cpp_handler("read_file",
    [](const nlohmann::json& params) -> nlohmann::json {
      std::string path = params.value("path", "");
      if (path.empty()) return {{"success", false}, {"error", "path_required"}};
      int offset = params.value("offset", 1);
      int limit = json_coerce_int(params, "limit", 500);
      auto rr = safe_read_file_paged(path, offset, limit);
      if (!rr.success) {
        return {{"success", false}, {"error", rr.error}};
      }
      return {
        {"success", true},
        {"output", rr.output},
        {"total_lines", rr.total_lines},
        {"path", path}
      };
    });

  skill_registry_.register_cpp_handler("write_file",
    [](const nlohmann::json& params) -> nlohmann::json {
      std::string path = params.value("path", "");
      std::string content = params.value("content", "");
      if (path.empty()) return {{"success", false}, {"error", "path_required"}};

      auto wr = safe_write_file(path, content);
      if (!wr.success) {
        return {{"success", false}, {"error", wr.error}};
      }
      nlohmann::json resp = {
        {"success", true},
        {"output", "wrote " + std::to_string(wr.bytes_written) + " bytes to " + wr.normalized_path},
        {"exit_code", 0},
        {"path", wr.normalized_path},
        {"bytes_written", static_cast<int>(wr.bytes_written)}
      };
      if (!wr.syntax_error.empty()) resp["syntax_check"] = wr.syntax_error;
      return resp;
    });

  // shell_exec: 白名单内命令通过 popen 执行
  // v0.53.18: list_dir 幽灵工具补实现——此前 schema/角色白名单有名无 handler
  //（真任务实测 no_cpp_handler:list_dir，LLM 被迫绕路 ls）。
  skill_registry_.register_cpp_handler("list_dir",
    [](const nlohmann::json& params) -> nlohmann::json {
      std::string dir = params.value("dir", params.value("path", "."));
      std::error_code ec;
      if (!std::filesystem::exists(dir, ec) || ec)
        return {{"success", false}, {"error", "dir_not_found"}, {"dir", dir}};
      nlohmann::json entries = nlohmann::json::array();
      for (auto it = std::filesystem::directory_iterator(
               dir, std::filesystem::directory_options::skip_permission_denied, ec);
           it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code sec;
        auto fsize = it->is_regular_file(sec) ? it->file_size(sec) : 0;
        entries.push_back({{"name", it->path().filename().string()},
                           {"type", it->is_directory(sec) ? "dir" : "file"},
                           {"size", fsize}});
        if (entries.size() >= 500) break;  // 防巨目录
      }
      return {{"success", true}, {"dir", dir}, {"count", entries.size()}, {"entries", entries}};
    });

  skill_registry_.register_cpp_handler("shell_exec",
    [](const nlohmann::json& params) -> nlohmann::json {
      std::string command = params.value("command", params.value("cmd", ""));
      if (command.empty()) return {{"success", false}, {"error", "command_required"}};

      // v0.33.0: 危险命令硬阻断（始终生效，不依赖配置）
      std::string block_reason;
      if (!CommandValidator::is_safe(command, &block_reason)) {
        return {{"success", false}, {"error", "blocked:" + block_reason}};
      }
      // v0.54.32: 与 FC 主路径对齐——多命令拼接 / deny 子串（curl 等）一律拒绝
      if (grade_shell_risk(command) == ToolRisk::Dangerous) {
        return {{"success", false}, {"error", "blocked_by_risk_grading"}};
      }

      // 加载配置
      auto forbid = policy_string_list("whitelist.common_rules.forbid_patterns");
      int max_bytes = 65536;
      int max_sec = 30;
      std::string max_bytes_str = policy_text("whitelist.common_rules.max_output_bytes", "");
      std::string max_sec_str = policy_text("whitelist.common_rules.max_exec_seconds", "");
      if (!max_bytes_str.empty()) { try { max_bytes = std::stoi(max_bytes_str); } catch (...) {} }
      if (!max_sec_str.empty())   { try { max_sec   = std::stoi(max_sec_str);   } catch (...) {} }

      // 禁止模式检查（黑白名单都生效）
      for (const auto& pattern : forbid) {
        if (command.find(pattern) != std::string::npos) {
          return {{"success", false}, {"error", "forbidden_pattern:" + pattern}};
        }
      }

      // 访问控制：whitelist 模式下检查命令是否在白名单内
      std::string access = policy_text("shell_exec.access", "whitelist");
      // --dev 模式：运行时覆盖为 blacklist（不改配置文件）
      const char* dev = std::getenv("THIN_AGENT_DEV_MODE");
      if (dev && std::string(dev) == "1") access = "blacklist";
      if (access == "whitelist") {
#ifdef __linux__
        auto allowed = policy_string_list("whitelist.linux");
#elif defined(__APPLE__) && defined(__MACH__)
        auto allowed = policy_string_list("whitelist.macos");
#elif defined(_WIN32)
        auto allowed = policy_string_list("whitelist.windows");
#elif defined(__ANDROID__)
        auto allowed = policy_string_list("whitelist.android");
#else
        auto allowed = policy_string_list("whitelist.linux");  // fallback
#endif
        std::string first_word = command;
        auto space_pos = first_word.find(' ');
        if (space_pos != std::string::npos) first_word = first_word.substr(0, space_pos);
        auto slash_pos = first_word.rfind('/');
        if (slash_pos != std::string::npos) first_word = first_word.substr(slash_pos + 1);

        bool cmd_allowed = false;
        for (const auto& a : allowed) {
          if (a == first_word) { cmd_allowed = true; break; }
        }
        if (!cmd_allowed) {
          return {{"success", false}, {"error", "not_in_whitelist:" + first_word}};
        }
      }
      // blacklist 模式：不做额外检查，直接执行

      // 执行
      bool pty_mode = params.value("pty", false);
      bool bg_mode = params.value("background", false);
      int timeout_ms = max_sec * 1000;
      if (params.contains("timeout") && params["timeout"].is_number())
        timeout_ms = params["timeout"].get<int>() * 1000;

      // ── PTY 模式 ──
      if (pty_mode) {
        std::string stdin_text = params.value("stdin", "");
        auto result = pty_exec(command, stdin_text, timeout_ms);
        return {
          {"success", result.exit_code == 0},
          {"output", result.output},
          {"exit_code", result.exit_code},
          {"elapsed_ms", result.elapsed_ms},
          {"timed_out", result.timed_out}
        };
      }

      // ── Background 模式 ──
      if (bg_mode) {
        auto& mgr = BackgroundProcessManager::instance();
        auto sid = mgr.start(command, timeout_ms);
        if (sid.empty()) return {{"success", false}, {"error", "bg_fork_failed"}};
        return {
          {"success", true},
          {"output", "Background process started"},
          {"session_id", sid}
        };
      }

      // ── v0.36.0: 沙箱执行（替代 popen）──
      bool sandbox_mode = params.value("sandbox", true);  // 默认开启
      if (sandbox_mode) {
        command = rewrite_sandbox_tmp_paths(command);
        SandboxExecutor::Config sandbox_cfg;
        sandbox_cfg.timeout_ms = max_sec * 1000;
        sandbox_cfg.max_output_bytes = max_bytes;
        sandbox_cfg.max_memory_mb = 256;
        sandbox_cfg.allow_network = params.value("allow_network", false);

        auto result = SandboxExecutor::execute(command, sandbox_cfg);
        std::string out_norm = unrewrite_sandbox_tmp_paths(result.output);
        return {
          {"success", result.exit_code == 0 && result.error.empty()},
          {"output", out_norm},
          {"exit_code", result.exit_code},
          {"elapsed_ms", result.elapsed_ms},
          {"timed_out", result.timed_out},
          {"sandbox", SandboxExecutor::native_sandbox_available()}
        };
      }

      // ── 兼容模式：popen 前台执行 ──
      FILE* pipe = popen(command.c_str(), "r");
      if (!pipe) return {{"success", false}, {"error", "popen_failed"}};

      std::string output;
      char buf[4096];
      auto start = std::chrono::steady_clock::now();
      while (fgets(buf, sizeof(buf), pipe)) {
        // v0.52.23: 协作式取消（超时/上层放弃后读循环提前退）
        if (agent::current_tool_ctx() &&
            agent::current_tool_ctx()->cancelled()) {
          break;
        }
        output += buf;
        if (static_cast<int>(output.size()) > max_bytes) {
          output = output.substr(0, max_bytes) + "\n...[truncated]";
          break;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > max_sec) {
          output += "\n...[timeout after " + std::to_string(max_sec) + "s]";
          break;
        }
      }
      int rc = pclose(pipe);
      // v0.54.16: `WIFEXITED`/`WEXITSTATUS` 是 POSIX wait 宏，**MSVC/MinGW 都没有** ⇒ Windows 侧
      // 编不过（本文件含 `_WIN32` 分支却在公共路径用了它们）。Windows 的 `_pclose` 直接返回子进程
      // 退出码（MS 文档：exit status of the command），无"信号未退出"语义 ⇒ 恒视作已正常退出。
#ifdef _WIN32
      int exit_code = rc;
#else
      int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif

      return {
        {"success", exit_code == 0},
        {"output", output},
        {"exit_code", exit_code}
      };
    });

  // process: 管理后台进程（poll/wait/kill）
  skill_registry_.register_cpp_handler("process",
    [](const nlohmann::json& params) -> nlohmann::json {
      std::string action = params.value("action", "poll");
      std::string session_id = params.value("session_id", "");

      if (session_id.empty())
        return {{"success", false}, {"error", "session_id_required"}};

      auto& mgr = BackgroundProcessManager::instance();

      if (action == "poll") {
        auto state = mgr.poll(session_id);
        return {
          {"success", true},
          {"session_id", state.id},
          {"running", state.running},
          {"exit_code", state.exit_code},
          {"output", state.output}
        };
      }

      if (action == "wait") {
        int timeout_ms = params.value("timeout", 0) * 1000;
        auto state = mgr.wait(session_id, timeout_ms);
        return {
          {"success", true},
          {"session_id", state.id},
          {"running", state.running},
          {"exit_code", state.exit_code},
          {"output", state.output}
        };
      }

      if (action == "kill") {
        auto state = mgr.kill(session_id);
        return {
          {"success", !state.running},
          {"session_id", state.id},
          {"running", state.running},
          {"exit_code", state.exit_code},
          {"output", state.output}
        };
      }

      if (action == "write") {
        std::string data = params.value("data", "");
        // v0.54.22: 如实回报 —— 此前无条件 success:true，"wrote N bytes" 在写失败时是**假报**
        const bool ok = mgr.write_stdin(session_id, data);
        if (!ok) {
          return {{"success", false},
                  {"error", "write_failed"},
                  {"hint", "进程不存在/已退出，或其 stdin 已关闭"}};
        }
        return {{"success", true}, {"output", "wrote " + std::to_string(data.size()) + " bytes"}};
      }

      return {{"success", false}, {"error", "unknown_action:" + action}};
    });

  // v0.54.33: search_code 纯 C++ 实现——此前 Pipeline/FC 都拼 grep 字符串，
  // pattern 可破单引号注入（实测 `x' /tmp; id; echo '` 可过弱闸）。
  skill_registry_.register_cpp_handler("search_code",
    [](const nlohmann::json& params) -> nlohmann::json {
      std::string pattern = params.value("pattern", "");
      std::string dir = params.value("dir", params.value("path", "."));
      int max_results = json_coerce_int(params, "max_results", 50);
      if (max_results < 1) max_results = 1;
      if (max_results > 200) max_results = 200;
      if (pattern.empty()) return {{"success", false}, {"error", "pattern_required"}};
      if (dir.empty()) dir = ".";

      // 路径不得含 shell 元字符（纵深：即使误走 shell 也不易注入）
      if (dir.find_first_of(";|&`$<>") != std::string::npos) {
        return {{"success", false}, {"error", "invalid_dir_chars"}};
      }

      std::regex re;
      bool use_regex = true;
      try {
        re = std::regex(pattern, std::regex::ECMAScript);
      } catch (const std::regex_error&) {
        use_regex = false;  // 非法正则 → 字面量子串匹配
      }

      static const std::unordered_set<std::string> kExt = {
          ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".py", ".js", ".ts",
          ".tsx", ".jsx", ".java", ".go", ".rs", ".md", ".json", ".yaml", ".yml",
          ".sh", ".cmake", ".txt", ".toml"};

      std::ostringstream out;
      int matches = 0;
      std::error_code ec;
      std::filesystem::recursive_directory_iterator it(
          dir, std::filesystem::directory_options::skip_permission_denied, ec);
      if (ec) return {{"success", false}, {"error", "search_failed:" + ec.message()}};

      for (; it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (agent::current_tool_ctx() && agent::current_tool_ctx()->cancelled()) {
          return {{"success", false}, {"error", "cancelled"}};
        }
        if (ec) { ec.clear(); continue; }
        if (it.depth() > 16) continue;
        std::error_code sec;
        auto st = it->symlink_status(sec);
        if (sec || !std::filesystem::is_regular_file(st)) continue;
        const auto ext = it->path().extension().string();
        if (!kExt.count(ext)) continue;

        std::ifstream in(it->path());
        if (!in) continue;
        std::string line;
        int lineno = 0;
        while (std::getline(in, line)) {
          ++lineno;
          if (line.size() > 8192) continue;  // 跳过疑似二进制/超长行
          bool hit = false;
          if (use_regex) {
            try {
              hit = std::regex_search(line, re);
            } catch (...) {
              hit = false;
            }
          } else {
            hit = line.find(pattern) != std::string::npos;
          }
          if (!hit) continue;
          out << it->path().string() << ":" << lineno << ":" << line << "\n";
          if (++matches >= max_results) {
            out << "...[truncated at " << max_results << " matches]\n";
            goto done;
          }
        }
      }
    done:
      std::string text = out.str();
      if (text.empty()) text = "(no matches)";
      return {{"success", true},
              {"output", text},
              {"exit_code", 0},
              {"match_count", matches}};
    });

  // search_files: 递归搜索匹配 glob 模式的文件
  skill_registry_.register_cpp_handler("search_files",
    [](const nlohmann::json& params) -> nlohmann::json {
      // v0.52.16: 参数 coerce——LLM 常把数值参数传成字符串
      // （"max_results":"50"），nlohmann value<T> 遇类型不匹配抛
      // type_error.302 → 桥接到子代理时报 tool execution exception
      // （真 e2e t4 实测：最宽松 * 探测也挂）。全部走 coerce 容错。
      std::string pattern;
      if (params.contains("pattern") && params["pattern"].is_string())
        pattern = params["pattern"].get<std::string>();
      else if (params.contains("glob") && params["glob"].is_string())
        pattern = params["glob"].get<std::string>();
      std::string dir;
      if (params.contains("dir") && params["dir"].is_string())
        dir = params["dir"].get<std::string>();
      else if (params.contains("path") && params["path"].is_string())
        dir = params["path"].get<std::string>();
      if (dir.empty()) dir = ".";
      int max_results = json_coerce_int(params, "max_results", 50);
      if (pattern.empty()) return {{"success", false}, {"error", "pattern_required"}};
      if (dir.empty()) dir = ".";
      // glob → regex
      std::string re_str;
      for (char c : pattern) {
        if (c == '*') re_str += ".*";
        else if (c == '?') re_str += ".";
        else if (c == '.' || c == '+' || c == '^' || c == '$' || c == '(' ||
                 c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
                 c == '|' || c == '\\') { re_str += '\\'; re_str += c; }
        else re_str += c;
      }
      try {
        std::regex re(re_str, std::regex::icase);
        std::vector<std::string> matches;
        std::error_code ec;
        // v0.45.8: 符号链接环防护 — recursive_directory_iterator 遇
        // symlink 环（如 /run/udev）会抛 filesystem_error；用 error_code
        // 重载 + 跳过 symlink 目录 + 限制深度，避免整个搜索被一个环中断。
        std::filesystem::recursive_directory_iterator it(
            dir, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) return {{"success", false}, {"error", "search_failed:" + ec.message()}};
        std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
          // v0.52.23: 协作式取消——调用方超时后置位，遍历提前退出
          if (agent::current_tool_ctx() &&
              agent::current_tool_ctx()->cancelled()) {
            return {{"success", false}, {"error", "cancelled"}};
          }
          if (ec) { ec.clear(); continue; }  // 单目录错误跳过，不中断整个搜索
          if (it.depth() > 16) continue;     // 深度上限，防超深目录树
          std::error_code sec;
          auto st = it->symlink_status(sec);  // 不跟随 symlink 的类型判断
          if (sec || !std::filesystem::is_regular_file(st)) continue;
          if (std::regex_match(it->path().filename().string(), re)) {
            matches.push_back(it->path().string());
            if (static_cast<int>(matches.size()) >= max_results) break;
          }
        }
        std::string output = "找到 " + std::to_string(matches.size()) + " 个匹配 '" + pattern + "' 的文件:\n";
        for (const auto& m : matches) output += m + "\n";
        return {{"success", true}, {"output", output}, {"exit_code", 0},
                {"count", matches.size()}, {"files", matches}};
      } catch (const std::exception& e) {
        return {{"success", false}, {"error", std::string("search_failed:") + e.what()}};
      }
    });
}

/// 媒体/子Agent/看板/监控工具（capture/spawn_agent/bb_*/monitor_*/agent_dag）（v0.49.1 从 register_cpp_handlers 按域拆出，内容零变化）
void AgentService::register_media_agent_tools() {
  // media_ops: 交由 TaskEngine 执行（实际提交在 media pipeline 循环中完成）
  auto media_ok = [](const nlohmann::json&) -> nlohmann::json {
    return {{"success", true}, {"output", "ok"}};
  };
  (void)media_ok;  // v0.53.0: capture_* 已迁 cam_media 插件

  // spawn_agent: 派生子 Agent（留核心——引擎链）
  skill_registry_.register_cpp_handler("spawn_agent",
    [this](const nlohmann::json& params) -> nlohmann::json {
      std::string goal = params.value("goal", params.value("task", ""));
      if (goal.empty()) return {{"success", false}, {"error", "goal required"}};
      if (g_spawn_nest_depth >= 3) {
        // v0.54.11 (R93): 护栏拒绝此前**完全不可见**（运维盲区：e2e 只看到顶层 turns_exhausted，
        // 日志里 0 行护栏痕迹）。按本仓 [WARN] 风格补日志——它是"策略拒绝"，与
        // delegate_task/decompose 的 clamped 告警同类。
        std::cerr << "[WARN] spawn nesting limit (3) reached — rejected"
                     " (depth=" << g_spawn_nest_depth
                  << ", active=" << g_active_sub_agents.load() << ")\n";
        return {{"success", false},
                {"error", "spawn nesting limit (3) reached — decompose the task instead"}};
      }
      if (g_active_sub_agents.load() >= kMaxActiveSubAgents) {
        std::cerr << "[WARN] policy: too many active sub-agents (16) — rejected"
                     " (active=" << g_active_sub_agents.load() << ")\n";
        return {{"success", false},
                {"error", "policy: too many active sub-agents (16) — wait or decompose"}};
      }
      ++g_active_sub_agents;
      ++g_spawn_nest_depth;
      struct SrDepthGuard {
        ~SrDepthGuard() {
          --g_spawn_nest_depth;
          --g_active_sub_agents;
        }
      } sr_depth_guard;

      std::string role_name = params.value("role", "researcher");
      const agent::AgentRole* role = role_mgr_ ? role_mgr_->find(role_name) : nullptr;
      if (!role) {
        nlohmann::json avail = nlohmann::json::array();
        if (role_mgr_) for (auto& r : role_mgr_->list()) avail.push_back(r.name);
        return {{"success", false}, {"error", "unknown role: " + role_name},
                {"available_roles", avail}};
      }

      agent::AgentLoopConfig sub_cfg;
      sub_cfg.max_turns = params.value("max_turns", 8);
      sub_cfg.request_timeout_ms = params.value("timeout_ms", 60000);
      sub_cfg.human_in_the_loop = false;
      if (!role->tools.empty()) {
        for (const auto& t : role->tools) sub_cfg.allowed_tools.insert(t);
      }

      std::string role_prompt = role->system_prompt;
      if (role_prompt.empty()) role_prompt = "You are a " + role->name + " specialist.";

      std::string api_key;
      api_key = resolve_api_key(credential_pool_.get(), cfg_.api_key_env, cfg_.provider);

      auto sub_loop = std::make_unique<agent::AgentLoop>(cfg_, api_key,
                                                          agent::ToolRegistry::instance(), sub_cfg);
      if (memory_manager_) sub_loop->set_memory_manager(memory_manager_.get());
    if (skill_manager_) sub_loop->set_skill_manager(skill_manager_.get());  // v0.39.1
      std::string task_prompt = role_prompt + "\n\n## Task\n" + goal +
          "\n\nRespond with your findings directly. Do not ask follow-up questions.";

      std::string goal_ctx, corr_ctx;
      if (goal_mgr_) goal_ctx = goal_mgr_->to_prompt_injection();
      if (correction_store_) {
        std::vector<std::string> known_tools;
        for (const auto& t : agent::ToolRegistry::instance().all_tools())
          known_tools.push_back(t.name);
        corr_ctx = correction_store_->to_prompt_injection(known_tools);
      }
      std::string full_sys = goal_ctx + corr_ctx + task_prompt;

      ++sub_agent_counter_;
      std::string agent_id = role_name + "_" + std::to_string(sub_agent_counter_.load());

      if (agent_bus_) {
        auto msgs = agent_bus_->drain(agent_id);
        if (!msgs.empty()) {
          full_sys += "\n\n## Messages from other agents\n";
          for (const auto& m : msgs) full_sys += "- " + m + "\n";
        }
      }
      if (blackboard_ && blackboard_->size() > 0) {
        full_sys += blackboard_->to_prompt_injection();
      }

      auto result = sub_loop->run(goal, full_sys);

      return {
        {"success", result.ok},
        {"agent_id", agent_id},
        {"role", role_name},
        {"output", result.final_answer},
        {"turns", result.turns_used},
        {"tool_calls_count", result.tool_calls.size()},
        {"error", result.error}
      };
    });



  // v0.43.0: 注册 spawn_agent 到 ToolRegistry（子 Agent 可嵌套调用；留核心——引擎链）
  {
    auto& tool_reg = agent::ToolRegistry::instance();
    tool_reg.register_tool({
      "spawn_agent",
      "Spawn a sub-agent to complete a task. Returns {success,agent_id,output,turns}.",
      {{"goal", "string", "Task goal for the sub-agent", true}},
      [cfg = cfg_, pool = credential_pool_.get()](const nlohmann::json& p) -> nlohmann::json {
        std::string goal = p.value("goal", p.value("task", ""));
        if (goal.empty()) return {{"success", false}, {"error", "goal required"}};
        if (g_spawn_nest_depth >= 3) {
          // v0.54.11 (R93): 护栏拒绝补日志（原先完全不可见，见 AgentServiceTools 另一处注释）
          std::cerr << "[WARN] spawn nesting limit (3) reached — rejected"
                       " (depth=" << g_spawn_nest_depth
                    << ", active=" << g_active_sub_agents.load() << ")\n";
          return {{"success", false},
                  {"error", "spawn nesting limit (3) reached — decompose the task instead"}};
        }
        if (g_active_sub_agents.load() >= kMaxActiveSubAgents) {
          std::cerr << "[WARN] policy: too many active sub-agents (16) — rejected"
                       " (active=" << g_active_sub_agents.load() << ")\n";
          return {{"success", false},
                  {"error", "policy: too many active sub-agents (16) — wait or decompose"}};
        }
        ++g_active_sub_agents;
        ++g_spawn_nest_depth;
        struct TrDepthGuard {
          ~TrDepthGuard() {
            --g_spawn_nest_depth;
            --g_active_sub_agents;
          }
        } tr_depth_guard;
        std::string role_str = p.value("role", "researcher");
        agent::AgentLoopConfig sub_cfg;
        sub_cfg.max_turns = p.value("max_turns", 8);
        sub_cfg.request_timeout_ms = p.value("timeout_ms", 60000);
        std::string key = resolve_api_key(pool, cfg.api_key_env, cfg.provider);
        auto sub_loop = std::make_unique<agent::AgentLoop>(cfg, key,
            agent::ToolRegistry::instance(), sub_cfg);
        std::string sys = "You are a " + role_str + " specialist.\n## Task\n" + goal;
        auto r = sub_loop->run(goal, sys);
        return {{"success", r.ok}, {"agent_id", "sub_" + std::to_string(time(nullptr))},
                {"role", role_str}, {"output", r.final_answer},
                {"turns", r.turns_used}, {"error", r.error}};
      },
      false  // not dangerous
    });

    // delegate_task: 批量并发执行多个子任务
    tool_reg.register_tool({
      "delegate_task",
      "Execute multiple tasks in parallel sub-agents. Returns array of results.",
      {{"tasks", "string", "JSON array of {goal,context?} objects", true}},
      [cfg = cfg_, pool = credential_pool_.get()](const nlohmann::json& p) -> nlohmann::json {
        std::string tasks_str = p.value("tasks", "[]");
        nlohmann::json tasks;
        try { tasks = nlohmann::json::parse(tasks_str); } catch (...) { tasks = nlohmann::json::array(); }
        if (!tasks.is_array() || tasks.empty())
          return {{"success", false}, {"error", "tasks must be a non-empty JSON array"}};
        // v0.54.2 (R89): **扇出钳制**。此前 `tasks` 数量、并发、每任务预算**全由参数决定且无上限**
        // —— LLM（可被提示词/注入引导）一次调用即可起 N 个完整 AgentLoop（`std::async` 全部同时
        // 启动，无并发上限；`max_turns`/`timeout_ms` 也取自任务 JSON，默认 timeout 150s）。
        // 现在：默认 ≤8 项、每批并发 ≤4、每项 ≤12 轮 / ≤300s，并**如实回报**截断信息。
        static const size_t kMaxDelegateTasks = [] {
          const char* e = getenv("THIN_AGENT_DELEGATE_MAX_TASKS");
          const long v = e ? atol(e) : 0;
          return v > 0 ? static_cast<size_t>(v) : static_cast<size_t>(8);
        }();
        static const size_t kMaxDelegateConcurrent = [] {
          const char* e = getenv("THIN_AGENT_DELEGATE_MAX_CONCURRENT");
          const long v = e ? atol(e) : 0;
          return v > 0 ? static_cast<size_t>(v) : static_cast<size_t>(4);
        }();
        const auto fanout = clamp_fanout_count(tasks.size(), kMaxDelegateTasks);
        if (fanout.truncated) {
          std::cerr << "[WARN] delegate_task fan-out clamped: " << fanout.requested << "→"
                    << fanout.used << " tasks (max " << kMaxDelegateTasks
                    << "; raise THIN_AGENT_DELEGATE_MAX_TASKS to allow more)\n";
          nlohmann::json kept = nlohmann::json::array();
          for (size_t i = 0; i < fanout.used; ++i) kept.push_back(tasks[i]);
          tasks = std::move(kept);
        }
        std::vector<std::future<nlohmann::json>> futures;
        size_t launched = 0;
        for (const auto& t : tasks) {
          // 并发分批：每批 ≤ kMaxDelegateConcurrent（此前是一次性全量启动）
          if (futures.size() >= kMaxDelegateConcurrent) {
            for (auto& f : futures) f.wait();
            futures.clear();
          }
          ++launched;
          futures.push_back(std::async(std::launch::async, [cfg, pool, t]() {
            std::string goal = t.value("goal", "");
            if (goal.empty()) return nlohmann::json({{"success", false}, {"error", "goal required"}});
            std::string ctx = t.value("context", "");
            std::string full_goal = ctx.empty() ? goal : goal + "\n\nContext: " + ctx;
            std::string key = resolve_api_key(pool, cfg.api_key_env, cfg.provider);
            agent::AgentLoopConfig sub_cfg;
            // v0.54.2 (R89): 每项预算钳制（原值可被任务 JSON 放大：轮数/超时都无上限）
            const auto budget = clamp_subtask_budget(
                t.value("max_turns", 6), t.value("timeout_ms", 150000),
                0, 0);  // 0,0 → 默认上限 12 轮 / 300s
            sub_cfg.max_turns = budget.max_turns;
            // v0.52.17: 最后一个 30s 硬编码残留（delegate_task 子代理）
            // ——8 处 AgentLoop 构造点全量对齐（GLM 慢端点实测 30s 必超）
            sub_cfg.request_timeout_ms = static_cast<int>(budget.timeout_ms);
            auto loop = std::make_unique<agent::AgentLoop>(cfg, key,
                agent::ToolRegistry::instance(), sub_cfg);
            auto r = loop->run(full_goal, "You are a task specialist.\n## Task\n" + full_goal);
            return nlohmann::json({{"success", r.ok}, {"goal", goal},
                                   {"output", r.final_answer}, {"turns", r.turns_used},
                                   {"error", r.error}});
          }));
        }
        nlohmann::json results = nlohmann::json::array();
        for (auto& f : futures) {
          results.push_back(f.get());
        }
        // v0.54.2 (R89): 截断/钳制**如实回报**（不静默丢）
        return {{"success", true},
                {"count", tasks.size()},
                {"tasks_requested", fanout.requested},
                {"fanout_truncated", fanout.truncated},
                {"results", results}};
      },
      false
    });
  }

  // ── v0.25.3: CheckpointManager 工具 ──




  // ── v0.31.0: FilesystemCheckpoint 工具 ──





  // ── v0.25.4: ErrorCorrectionStore + GoalManager + KanbanBoard + Summarizer + SkillManager ──







}

/// 定时任务工具（cron_*）（v0.53.0: 全部迁 cron 插件——本函数保留骨架）
void AgentService::register_cron_tools() {
  // cron_add/list/remove/stats/update/pause/resume/trigger → libskill_cron.so
}

/// 技能/检查点/目标/看板工具（v0.53.0: checkpoint_*/goal_*/kanban_* 迁
/// state/kanban 插件；skill_*/correction 留核心——自进化闭环）
void AgentService::register_skill_checkpoint_tools() {
  // ToolRegistry 的 spawn_agent/delegate_task 注册留本函数（引擎链）
}

/// 元工具/记忆/会话搜索/巡检（summarize/kb_*/memory_*/session_*/find_tool/show_tool/patrol_*）（v0.49.1 从 register_cpp_handlers 按域拆出；v0.53.0: kb_search 迁 kb 插件）
void AgentService::register_meta_memory_tools() {


  // ── v0.27.5: FactStore 持久记忆工具 ──




  // ── v0.27.6: SessionStore 会话搜索工具 ──



  // ── v0.41.2: 按需发现工具 ──



  // ── v0.27.7: PatrolProbe 巡检工具 ──



}

}  // namespace thin_agent
