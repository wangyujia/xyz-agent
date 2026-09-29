#pragma once

#include <functional>
#include <map>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {

/// 单个 Action 的跨平台 handler
struct ActionHandler {
  std::string os;          // "linux" / "windows" / "macos" / "android" / "ios"
  std::string type;        // "shell" / "cpp" / "http"
  std::string cmd;         // shell 模板（含 {param} 占位符）
};

/// 单个 Action 定义
struct ActionDef {
  std::string name;
  std::string desc;                          // LLM prompt 用
  std::map<std::string, std::string> params; // param_name → 中文说明
  std::vector<std::string> optional_params;  // 可选参数名（不进 required 数组）
  std::vector<ActionHandler> handlers;       // 按 OS 分发

  /// 根据当前 OS 名查找 handler
  const ActionHandler* find_handler(const std::string& os) const {
    for (auto& h : handlers) {
      if (h.os == os) return &h;
    }
    return nullptr;
  }
};

/// 一个 Skill = 一组相关的 Action
struct SkillDef {
  std::string name;
  bool enabled = true;
  std::string desc;
  std::vector<ActionDef> actions;
};

/// Shell 白名单配置
struct ShellWhitelist {
  struct OsEntry {
    std::string os;
    std::vector<std::string> allowed;         // 允许的命令名列表
  };
  std::vector<OsEntry> entries;
  int max_output_bytes = 65536;
  int max_exec_sec = 30;
  std::vector<std::string> forbid_patterns;   // 禁止的 shell 模式（管道、重定向等）

  /// 查找指定 OS 的白名单
  const OsEntry* find_os(const std::string& os) const {
    for (auto& e : entries) {
      if (e.os == os) return &e;
    }
    return nullptr;
  }
};

/// C++ handler 签名：params → {success, output}
using CppHandlerFn = std::function<nlohmann::json(const nlohmann::json& params)>;

/// Skill 注册表：管理 Skill/Action 的加载、查询、校验
class SkillRegistry {
 public:
  /// 从 chat_policy.json 的 "skills" 段加载
  bool load_from_policy(const nlohmann::json& skills_json,
                        const nlohmann::json& whitelist_json);

  /// 注册一个 C++ handler（替代 shell）
  void register_cpp_handler(const std::string& action_name, CppHandlerFn fn);

  /// 分发 C++ handler
  nlohmann::json dispatch_cpp(const std::string& action_name,
                              const nlohmann::json& params) const;

  /// 是否已注册 C++ handler（pipeline 优先走 cpp，避免 JSON shell 模板绕过加固实现）
  bool has_cpp_handler(const std::string& action_name) const;

  /// 返回完整的 action_map（含 cpp 注册的 handler）
  const std::map<std::string, ActionDef>& action_map() const { return action_map_; }

  /// 获取所有已启用的 skill（扁平化为 action 列表 + skill 上下文）
  std::vector<const ActionDef*> get_enabled_actions() const;

  /// 由 action 名查找定义
  const ActionDef* find_action(const std::string& name) const;

  /// 由 action 名 + OS 返回 handler
  const ActionHandler* find_handler(const std::string& action_name,
                                    const std::string& os) const;

  /// 为 LLM prompt 生成可读的工具列表
  std::string build_skill_prompt_text() const;

  /// Shell 命令校验：解析命令名 → 白名单匹配 → 注入模式检测
  /// 返回 "" 表示通过；否则返回错误描述
  std::string validate_shell_command(const std::string& cmd,
                                     const std::string& os) const;

  /// 构建 OpenAI function calling tools schema（从已注册的 action 生成）。
  nlohmann::json build_tools_schema() const;

  /// 获取默认 OS 名（编译期确定）
  static std::string detect_os();

  /// OS 名对应的 shell 命令分隔符（Windows: &&, Unix: ; 但两者都在 forbid_patterns 中被禁）
  static std::string shell_cmd_separator(const std::string& os);

  /// 为 shell handler 展开 {param} 占位符
  static std::string expand_vars(const std::string& tmpl,
                                  const nlohmann::json& params);

  /// 解析 $vars 引用：$output_name[N].field → 实际值
  static std::string resolve_ctx_var(const std::string& expr,
                                      const nlohmann::json& ctx);

 private:
  std::vector<SkillDef> skills_;
  std::map<std::string, ActionDef> action_map_;            // name → def
  std::map<std::string, CppHandlerFn> cpp_handlers_;       // name → handler
  ShellWhitelist whitelist_;
  bool loaded_ = false;
};

}  // namespace thin_agent
