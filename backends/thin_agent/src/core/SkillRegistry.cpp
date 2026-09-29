#include "thin_agent/core/SkillRegistry.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "thin_agent/log/LogEvent.h"  // v0.53.90: 技能异常可观测

namespace thin_agent {

// ── OS 检测 ──

std::string SkillRegistry::detect_os() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
#  if TARGET_OS_IPHONE
  return "ios";
#  else
  return "macos";
#  endif
#elif defined(__ANDROID__)
  return "android";
#else
  return "linux";
#endif
}

std::string SkillRegistry::shell_cmd_separator(const std::string& os) {
  if (os == "windows") return " & ";
  return "; ";
}

// ── 加载 ──

bool SkillRegistry::load_from_policy(const nlohmann::json& skills_json,
                                     const nlohmann::json& whitelist_json) {
  skills_.clear();
  action_map_.clear();

  // 解析 skills
  if (!skills_json.is_object()) return false;
  for (auto it = skills_json.begin(); it != skills_json.end(); ++it) {
    const auto& sk = it.value();
    if (!sk.is_object()) continue;

    SkillDef sd;
    sd.name = it.key();
    sd.enabled = sk.value("enabled", true);
    sd.desc = sk.value("desc", "");

    if (sk.contains("actions") && sk["actions"].is_array()) {
      for (const auto& ac : sk["actions"]) {
        ActionDef ad;
        ad.name = ac.value("name", "");
        ad.desc = ac.value("desc", "");

        if (ac.contains("params") && ac["params"].is_object()) {
          for (auto pit = ac["params"].begin(); pit != ac["params"].end(); ++pit) {
            ad.params[pit.key()] = pit.value().is_string() ? pit.value().get<std::string>() : "";
          }
        }

        // v0.27.1: 解析可选参数列表（不进 LLM required 数组）
        if (ac.contains("optional") && ac["optional"].is_array()) {
          for (const auto& opt : ac["optional"]) {
            if (opt.is_string()) ad.optional_params.push_back(opt.get<std::string>());
          }
        }

        if (ac.contains("handlers") && ac["handlers"].is_object()) {
          for (auto hit = ac["handlers"].begin(); hit != ac["handlers"].end(); ++hit) {
            const auto& hv = hit.value();
            ActionHandler ah;
            ah.os = hit.key();
            ah.type = hv.value("type", "shell");
            ah.cmd = hv.value("cmd", "");
            ad.handlers.push_back(std::move(ah));
          }
        }

        sd.actions.push_back(std::move(ad));
      }
    }

    skills_.push_back(std::move(sd));
  }

  // 构建 action 索引
  for (const auto& sk : skills_) {
    for (const auto& ac : sk.actions) {
      action_map_[ac.name] = ac;
    }
  }

  // 解析 whitelist
  whitelist_ = ShellWhitelist{};
  if (whitelist_json.is_object()) {
    // common rules
    if (whitelist_json.contains("common_rules") && whitelist_json["common_rules"].is_object()) {
      const auto& cr = whitelist_json["common_rules"];
      whitelist_.max_output_bytes = cr.value("max_output_bytes", 65536);
      whitelist_.max_exec_sec = cr.value("max_exec_seconds", 30);
      if (cr.contains("forbid_patterns") && cr["forbid_patterns"].is_array()) {
        for (const auto& p : cr["forbid_patterns"]) {
          if (p.is_string()) whitelist_.forbid_patterns.push_back(p.get<std::string>());
        }
      }
    }

    // os-specific allowed commands
    for (auto it = whitelist_json.begin(); it != whitelist_json.end(); ++it) {
      if (it.key() == "common_rules") continue;
      if (!it.value().is_array()) continue;
      ShellWhitelist::OsEntry entry;
      entry.os = it.key();
      for (const auto& cmd : it.value()) {
        if (cmd.is_string()) entry.allowed.push_back(cmd.get<std::string>());
      }
      whitelist_.entries.push_back(std::move(entry));
    }
  }

  loaded_ = true;
  return true;
}

// ── 查询 ──

std::vector<const ActionDef*> SkillRegistry::get_enabled_actions() const {
  std::vector<const ActionDef*> out;
  for (const auto& sk : skills_) {
    if (!sk.enabled) continue;
    for (const auto& ac : sk.actions) {
      out.push_back(&ac);
    }
  }
  return out;
}

const ActionDef* SkillRegistry::find_action(const std::string& name) const {
  auto it = action_map_.find(name);
  return (it != action_map_.end()) ? &it->second : nullptr;
}

const ActionHandler* SkillRegistry::find_handler(const std::string& action_name,
                                                  const std::string& os) const {
  auto* ad = find_action(action_name);
  if (!ad) return nullptr;
  return ad->find_handler(os);
}

// ── LLM Prompt 生成 ──

std::string SkillRegistry::build_skill_prompt_text() const {
  std::ostringstream oss;
  for (const auto& sk : skills_) {
    if (!sk.enabled) continue;
    oss << "\n【" << sk.name << "】" << (sk.desc.empty() ? "" : " - " + sk.desc) << "\n";
    for (const auto& ac : sk.actions) {
      oss << "  - " << ac.name << ": " << ac.desc;
      if (!ac.params.empty()) {
        oss << " (参数: ";
        bool first = true;
        for (const auto& [k, v] : ac.params) {
          if (!first) oss << ", ";
          oss << k << "=" << v;
          first = false;
        }
        oss << ")";
      }
      oss << "\n";
    }
  }
  return oss.str();
}

// ── OpenAI function calling tools schema ──

nlohmann::json SkillRegistry::build_tools_schema() const {
  nlohmann::json tools = nlohmann::json::array();

  for (const auto& sk : skills_) {
    if (!sk.enabled) continue;
    for (const auto& ac : sk.actions) {
      nlohmann::json tool;
      tool["type"] = "function";

      nlohmann::json func;
      func["name"] = ac.name;
      func["description"] = ac.desc;
      if (!sk.desc.empty()) {
        func["description"] = std::string(func["description"]) + " (skill: " + sk.desc + ")";
      }

      // parameters JSON Schema
      nlohmann::json params;
      params["type"] = "object";

      nlohmann::json properties = nlohmann::json::object();
      nlohmann::json required = nlohmann::json::array();
      // v0.27.1: 构建 optional_params 查找集合
      std::unordered_set<std::string> opt_set(ac.optional_params.begin(), ac.optional_params.end());
      for (const auto& [k, v] : ac.params) {
        properties[k] = {{"type", "string"}, {"description", v}};
        if (opt_set.find(k) == opt_set.end()) {
          required.push_back(k);  // 只把非可选的参数放进 required
        }
      }

      if (ac.params.empty()) {
        properties = nlohmann::json::object();
      }
      params["properties"] = properties;
      params["required"] = required;

      func["parameters"] = params;
      tool["function"] = func;
      tools.push_back(tool);
    }
  }

  // v0.27.0: 也纳入 cpp_handlers_（如插件注册的 kb_index / kb_status 等）
  for (const auto& [name, _] : cpp_handlers_) {
    auto it = action_map_.find(name);
    if (it == action_map_.end()) continue;
    // 跳过已在 skills_ 中生成过的（避免重复）
    bool already_in_skills = false;
    for (const auto& sk : skills_) {
      if (!sk.enabled) continue;
      for (const auto& ac : sk.actions) {
        if (ac.name == name) { already_in_skills = true; break; }
      }
      if (already_in_skills) break;
    }
    if (already_in_skills) continue;

    const auto& ad = it->second;
    nlohmann::json tool;
    tool["type"] = "function";

    nlohmann::json func;
    func["name"] = ad.name;
    func["description"] = ad.desc;

    nlohmann::json params;
    params["type"] = "object";
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();
    // v0.27.1: 构建 optional_params 查找集合
    std::unordered_set<std::string> opt_set(ad.optional_params.begin(), ad.optional_params.end());
    for (const auto& [k, v] : ad.params) {
      properties[k] = {{"type", "string"}, {"description", v}};
      if (opt_set.find(k) == opt_set.end()) {
        required.push_back(k);
      }
    }
    params["properties"] = properties;
    params["required"] = required;
    func["parameters"] = params;
    tool["function"] = func;
    tools.push_back(tool);
  }

  return tools;
}

// ── Shell 校验 ──

std::string SkillRegistry::validate_shell_command(const std::string& cmd,
                                                   const std::string& os) const {
  // 1. 注入模式检测
  for (const auto& pat : whitelist_.forbid_patterns) {
    if (cmd.find(pat) != std::string::npos) {
      return "forbidden_pattern:" + pat;
    }
  }

  // 2. 提取命令名（第一个空格前或整串）
  std::string cmd_name = cmd;
  auto space = cmd.find(' ');
  if (space != std::string::npos) {
    cmd_name = cmd.substr(0, space);
  }

  // 3. 白名单匹配
  auto* os_entry = whitelist_.find_os(os);
  if (!os_entry) {
    return "no_whitelist_for_os:" + os;
  }

  bool found = false;
  for (const auto& allowed : os_entry->allowed) {
    if (cmd_name == allowed || cmd_name == allowed) {
      found = true;
      break;
    }
  }

  if (!found) {
    return "command_not_allowed:" + cmd_name;
  }

  return "";  // 通过
}

// ── C++ handler ──

void SkillRegistry::register_cpp_handler(const std::string& action_name,
                                         CppHandlerFn fn) {
  cpp_handlers_[action_name] = std::move(fn);
  // 同时注册到 action_map_，使 find_action/find_handler 可查找
  if (action_map_.find(action_name) == action_map_.end()) {
    ActionDef ad;
    ad.name = action_name;
    ad.desc = action_name;
    action_map_[action_name] = ad;
  }
}

bool SkillRegistry::has_cpp_handler(const std::string& action_name) const {
  return cpp_handlers_.find(action_name) != cpp_handlers_.end();
}

nlohmann::json SkillRegistry::dispatch_cpp(const std::string& action_name,
                                           const nlohmann::json& params) const {
  auto it = cpp_handlers_.find(action_name);
  if (it == cpp_handlers_.end()) {
    return {{"success", false}, {"error", "no_cpp_handler:" + action_name}};
  }

  // v0.53.90 技能异常隔离（单点收口）：dispatch_cpp 是**全仓所有技能/插件调用的唯一
  // 咽喉点**（AgentServiceUtil/Tools/Ws、McpServer 都经此）。此前 `return it->second()`
  // 让 handler 的异常原样穿出——内置 handler 的 nlohmann/std::filesystem 误用、以及
  // **跨 .so 边界的插件异常**（插件抛了异常，异常在宿主里继续展开；调用点无兜底时
  // std::terminate = 服务崩溃），且失败零可观测。
  // 语义：把"崩溃/整链失败"降级为**结构化失败 + 一条可观测日志**，成功路径零改动。
  try {
    return it->second(params);
  } catch (const std::exception& e) {
    log_event("skill", LogLevel::Error, "handler threw",
              {{"action", action_name}, {"what", e.what()}});
    return {{"success", false},
            {"error", std::string("handler_exception:") + e.what()},
            {"action", action_name}};
  } catch (...) {
    log_event("skill", LogLevel::Error, "handler threw unknown exception",
              {{"action", action_name}});
    return {{"success", false},
            {"error", "handler_exception:unknown"},
            {"action", action_name}};
  }
}

// ── 变量展开 ──

std::string SkillRegistry::expand_vars(const std::string& tmpl,
                                        const nlohmann::json& params) {
  std::string result = tmpl;
  for (auto it = params.begin(); it != params.end(); ++it) {
    std::string placeholder = "{" + it.key() + "}";
    std::string value;
    if (it.value().is_string()) {
      value = it.value().get<std::string>();
    } else {
      value = it.value().dump();
    }
    // 简单替换（不处理嵌套）
    size_t pos = 0;
    while ((pos = result.find(placeholder, pos)) != std::string::npos) {
      result.replace(pos, placeholder.length(), value);
      pos += value.length();
    }
  }
  return result;
}

std::string SkillRegistry::resolve_ctx_var(const std::string& expr,
                                            const nlohmann::json& ctx) {
  // 期望格式: $output_name[N].field  或  $output_name
  if (expr.empty() || expr[0] != '$') return expr;

  // 去掉 $ 前缀
  std::string path = expr.substr(1);

  // 解析: name[N].field
  std::string var_name = path;
  int idx = -1;
  std::string field;

  auto bracket = path.find('[');
  if (bracket != std::string::npos) {
    var_name = path.substr(0, bracket);
    auto close = path.find(']', bracket);
    if (close != std::string::npos) {
      try {
        idx = std::stoi(path.substr(bracket + 1, close - bracket - 1));
      } catch (...) {
        idx = -1;
      }
      // 看有没有 .field
      if (close + 1 < path.length() && path[close + 1] == '.') {
        field = path.substr(close + 2);
      }
    }
  }

  if (!ctx.contains(var_name)) return expr;

  const auto& val = ctx[var_name];

  // 如果指定了索引，从数组中取
  if (idx >= 0 && val.is_array() && idx < (int)val.size()) {
    const auto& item = val[idx];
    if (!field.empty() && item.is_object() && item.contains(field)) {
      return item[field].is_string() ? item[field].get<std::string>() : item[field].dump();
    }
    return item.is_string() ? item.get<std::string>() : item.dump();
  }

  return val.is_string() ? val.get<std::string>() : val.dump();
}

}  // namespace thin_agent
