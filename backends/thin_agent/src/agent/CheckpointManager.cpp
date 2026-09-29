#include "thin_agent/agent/CheckpointManager.h"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace thin_agent {
namespace agent {

std::string CheckpointManager::save(const std::string& session_id,
                                    const std::vector<std::string>& chat_memory,
                                    const nlohmann::json& extra,
                                    const std::string& label,
                                    int max_per_session) {
  std::lock_guard<std::mutex> lock(mu_);

  auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();

  // 生成 checkpoint_id
  static int seq = 0;
  std::ostringstream id_ss;
  id_ss << "ckpt_" << session_id << "_" << now << "_" << (++seq);
  std::string ckpt_id = id_ss.str();

  Snapshot snap;
  snap.session_id = session_id;
  snap.timestamp = now;
  snap.label = label;
  snap.chat_memory = chat_memory;
  snap.extra = extra;

  snapshots_[ckpt_id] = snap;
  session_index_[session_id].push_back(ckpt_id);

  // 超过上限则删除最老的快照
  while (static_cast<int>(session_index_[session_id].size()) > max_per_session) {
    std::string old_id = session_index_[session_id].front();
    session_index_[session_id].pop_front();
    snapshots_.erase(old_id);
  }

  return ckpt_id;
}

std::vector<std::string> CheckpointManager::restore(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = session_index_.find(session_id);
  if (it == session_index_.end() || it->second.empty()) return {};

  const std::string& ckpt_id = it->second.back();
  auto snap_it = snapshots_.find(ckpt_id);
  if (snap_it == snapshots_.end()) return {};
  return snap_it->second.chat_memory;
}

std::vector<std::string> CheckpointManager::restore_by_id(
    const std::string& checkpoint_id) {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = snapshots_.find(checkpoint_id);
  if (it == snapshots_.end()) return {};

  return it->second.chat_memory;
}

std::vector<CheckpointManager::Snapshot> CheckpointManager::list_snapshots(
    const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mu_);

  std::vector<Snapshot> result;
  auto it = session_index_.find(session_id);
  if (it == session_index_.end()) return result;

  for (const auto& ckpt_id : it->second) {
    auto snap_it = snapshots_.find(ckpt_id);
    if (snap_it != snapshots_.end()) {
      result.push_back(snap_it->second);
    }
  }
  return result;
}

void CheckpointManager::clear(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = session_index_.find(session_id);
  if (it != session_index_.end()) {
    for (const auto& ckpt_id : it->second) {
      snapshots_.erase(ckpt_id);
    }
    session_index_.erase(it);
  }
}

CheckpointManager::Snapshot CheckpointManager::last_snapshot(
    const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = session_index_.find(session_id);
  if (it == session_index_.end() || it->second.empty()) return {};

  auto snap_it = snapshots_.find(it->second.back());
  if (snap_it != snapshots_.end()) return snap_it->second;

  return {};
}

}  // namespace agent
}  // namespace thin_agent
