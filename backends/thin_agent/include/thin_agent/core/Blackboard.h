#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace thin_agent {

/// 子 Agent 共享黑板：键值存储，所有子 Agent 可读写。
/// AgentService 持有实例，生命周期等于 session。
class Blackboard {
 public:
  Blackboard() = default;

  /// 写入键值（覆盖）。
  void write(const std::string& key, const nlohmann::json& value);

  /// 读取键值。
  nlohmann::json read(const std::string& key) const;

  /// 检查键是否存在。
  bool has(const std::string& key) const;

  /// 删除键。
  void erase(const std::string& key);

  /// 列出所有键。
  std::vector<std::string> keys() const;

  /// 清空所有数据。
  void clear();

  /// 条目数量。
  size_t size() const;

  /// 将整个黑板序列化为 prompt 注入文本。
  std::string to_prompt_injection() const;

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, nlohmann::json> data_;
};

}  // namespace thin_agent
