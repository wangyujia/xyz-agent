#include "thin_agent/core/SubAgentBus.h"

#include <algorithm>

namespace thin_agent {

void SubAgentBus::send(const std::string& from, const std::string& to,
                        const std::string& message) {
  if (to.empty() || message.empty()) return;
  std::lock_guard<std::mutex> lk(mu_);
  queues_[to].push_back("[" + from + "] " + message);
}

void SubAgentBus::broadcast(const std::string& from, const std::string& message) {
  if (message.empty()) return;
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& [agent_id, queue] : queues_) {
    if (agent_id != from) {
      queue.push_back("[broadcast from " + from + "] " + message);
    }
  }
}

std::vector<std::string> SubAgentBus::drain(const std::string& agent_id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = queues_.find(agent_id);
  if (it == queues_.end()) return {};
  std::vector<std::string> result = std::move(it->second);
  it->second.clear();
  return result;
}

std::vector<std::string> SubAgentBus::peek(const std::string& agent_id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = queues_.find(agent_id);
  return (it != queues_.end()) ? it->second : std::vector<std::string>{};
}

void SubAgentBus::clear(const std::string& agent_id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = queues_.find(agent_id);
  if (it != queues_.end()) it->second.clear();
}

size_t SubAgentBus::pending_count(const std::string& agent_id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = queues_.find(agent_id);
  return (it != queues_.end()) ? it->second.size() : 0;
}

void SubAgentBus::clear_all() {
  std::lock_guard<std::mutex> lk(mu_);
  queues_.clear();
}

}  // namespace thin_agent
