#include "thin_agent/core/Blackboard.h"

#include <sstream>

namespace thin_agent {

void Blackboard::write(const std::string& key, const nlohmann::json& value) {
  std::lock_guard<std::mutex> lk(mu_);
  data_[key] = value;
}

nlohmann::json Blackboard::read(const std::string& key) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = data_.find(key);
  return (it != data_.end()) ? it->second : nlohmann::json();
}

bool Blackboard::has(const std::string& key) const {
  std::lock_guard<std::mutex> lk(mu_);
  return data_.count(key) > 0;
}

void Blackboard::erase(const std::string& key) {
  std::lock_guard<std::mutex> lk(mu_);
  data_.erase(key);
}

std::vector<std::string> Blackboard::keys() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<std::string> result;
  result.reserve(data_.size());
  for (const auto& [k, _] : data_) result.push_back(k);
  return result;
}

void Blackboard::clear() {
  std::lock_guard<std::mutex> lk(mu_);
  data_.clear();
}

size_t Blackboard::size() const {
  std::lock_guard<std::mutex> lk(mu_);
  return data_.size();
}

std::string Blackboard::to_prompt_injection() const {
  std::lock_guard<std::mutex> lk(mu_);
  if (data_.empty()) return "";
  std::ostringstream oss;
  oss << "\n## Shared Context (Blackboard)\n";
  for (const auto& [k, v] : data_) {
    oss << "- " << k << ": ";
    if (v.is_string()) {
      oss << v.get<std::string>();
    } else {
      oss << v.dump();
    }
    oss << "\n";
  }
  return oss.str();
}

}  // namespace thin_agent
