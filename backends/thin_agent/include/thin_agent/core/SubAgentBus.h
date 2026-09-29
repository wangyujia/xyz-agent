#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace thin_agent {

/// 子 Agent 间消息总线：支持点对点发送、广播、接收。
/// AgentService 持有实例，所有子 Agent 通过它通信。
class SubAgentBus {
 public:
  SubAgentBus() = default;

  /// 点对点发送消息。
  void send(const std::string& from, const std::string& to,
            const std::string& message);

  /// 广播给所有已注册 Agent（除发送者）。
  void broadcast(const std::string& from, const std::string& message);

  /// 获取并清空指定 Agent 的待收消息。
  /// @return 所有待收消息，按时间顺序排列。
  std::vector<std::string> drain(const std::string& agent_id);

  /// 获取待收消息但不删除（用于预览，不影响 drain）。
  std::vector<std::string> peek(const std::string& agent_id) const;

  /// 清空指定 Agent 的消息队列。
  void clear(const std::string& agent_id);

  /// 返回待收消息数量。
  size_t pending_count(const std::string& agent_id) const;

  /// 清空所有消息。
  void clear_all();

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::vector<std::string>> queues_;
};

}  // namespace thin_agent
