#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "thin_agent/agent/ToolSchema.h"

namespace thin_agent {
namespace agent {

/// 工具注册中心：单例，管理所有可用工具的注册、发现、校验与执行。
///
/// 用法：
///   auto& reg = ToolRegistry::instance();
///   reg.register_tool({"weather", "查询天气", {{"city","string","城市名",true}}, my_handler});
///   reg.call("weather", {{"city","上海"}});
class ToolRegistry {
 public:
  /// 获取全局单例。
  static ToolRegistry& instance();

  /// 注册一个工具（同名覆盖，返回被覆盖的旧工具名列表）。
  /// @param on_conflict 冲突策略："replace"(默认) / "prefix" / "reject"
  /// @param prefix      冲突时添加的前缀（on_conflict="prefix" 时使用）
  std::vector<std::string> register_tool(ToolSchema tool,
                                          const std::string& on_conflict = "replace",
                                          const std::string& prefix = "mcp_");

  /// 按名称查找工具；未注册返回 nullptr。
  const ToolSchema* find(const std::string& name) const;

  /// 返回所有已注册工具的副本。
  std::vector<ToolSchema> all_tools() const;

  /// 序列化所有工具为 OpenAI function calling 格式。
  nlohmann::json to_openai_tools() const;

  /// 构建注入 LLM system prompt 的工具描述文本。
  std::string build_tools_prompt() const;

  /// 执行工具调用：参数校验 → 执行回调 → 超时保护 → 返回统一结果。
  /// @param name    工具名
  /// @param params  JSON 参数（已从 LLM 解析）
  /// @param timeout_ms 执行超时（毫秒），0 表示不限
  ToolCallResult call(const std::string& name,
                      const nlohmann::json& params,
                      int timeout_ms = 10000);

  /// 获取工具数量。
  size_t size() const;

  /// 清空所有工具（主要用于测试）。
  void clear();

 private:
  ToolRegistry() = default;
  ToolRegistry(const ToolRegistry&) = delete;
  ToolRegistry& operator=(const ToolRegistry&) = delete;

  std::unordered_map<std::string, ToolSchema> tools_;
  mutable std::mutex mu_;
};

}  // namespace agent
}  // namespace thin_agent
