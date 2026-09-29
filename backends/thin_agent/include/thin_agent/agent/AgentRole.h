#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace thin_agent {
namespace agent {

/// Agent 角色定义：每个子 Agent 拥有独立的 persona、system prompt、工具子集和可选模型。
struct AgentRole {
  std::string name;               ///< 角色名，如 "viewer", "tester", "researcher"
  std::string description;        ///< 人类可读描述
  std::string system_prompt;      ///< 角色专属 system prompt（覆盖默认）
  std::vector<std::string> tools; ///< 允许的工具名列表（空 = 全部可用）
  std::string model;              ///< 可选模型覆盖（空 = 使用主模型）

  /// 从 JSON 解析角色定义。
  static AgentRole from_json(const nlohmann::json& j);
  /// 序列化为 JSON。
  nlohmann::json to_json() const;
};

/// 角色管理器：管理预定义角色和动态角色。
class AgentRoleManager {
 public:
  AgentRoleManager() = default;

  /// 注册一个角色（同名覆盖）。
  void register_role(const AgentRole& role);
  /// 按名称查找角色。
  const AgentRole* find(const std::string& name) const;
  /// 列出所有已注册角色。
  std::vector<AgentRole> list() const;
  /// 删除角色。
  bool remove(const std::string& name);
  /// 角色数量。
  size_t size() const { return roles_.size(); }

  /// 注册内置角色（viewer, tester, researcher, summarizer, debugger，及 worker/developer/device/server）。
  void register_builtins();

 private:
  std::unordered_map<std::string, AgentRole> roles_;
  mutable std::mutex mu_;
};

}  // namespace agent
}  // namespace thin_agent
