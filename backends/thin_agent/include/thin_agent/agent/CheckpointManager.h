#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {
namespace agent {

/// 检查点管理器：AgentLoop 执行前自动快照会话状态，支持失败回滚。
class CheckpointManager {
 public:
  /// 单次快照。
  struct Snapshot {
    std::string session_id;
    int64_t timestamp;                              ///< epoch 毫秒
    std::string label;                              ///< auto / manual
    std::vector<std::string> chat_memory;           ///< 会话消息列表
    nlohmann::json extra;                           ///< 附加状态
  };

  CheckpointManager() = default;

  /// 保存当前会话快照，返回 checkpoint_id。
  std::string save(const std::string& session_id,
                   const std::vector<std::string>& chat_memory,
                   const nlohmann::json& extra = {},
                   const std::string& label = "auto",
                   int max_per_session = 10);

  /// 恢复到最近一次快照。返回被恢复的消息列表，失败返回空。
  std::vector<std::string> restore(const std::string& session_id);

  /// 恢复到指定 checkpoint_id 的快照。
  std::vector<std::string> restore_by_id(const std::string& checkpoint_id);

  /// 列出某 session 的所有快照。
  std::vector<Snapshot> list_snapshots(const std::string& session_id) const;

  /// 删除某 session 的所有快照。
  void clear(const std::string& session_id);

  /// 获取最近一次快照（不恢复）。
  Snapshot last_snapshot(const std::string& session_id) const;

 private:
  std::unordered_map<std::string, Snapshot> snapshots_;
  std::unordered_map<std::string, std::deque<std::string>> session_index_;
  mutable std::mutex mu_;
};

}  // namespace agent
}  // namespace thin_agent
