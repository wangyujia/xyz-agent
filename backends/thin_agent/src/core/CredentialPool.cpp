#include "thin_agent/core/CredentialPool.h"

#include <fstream>
#include <utility>

namespace thin_agent {
namespace {

std::string json_string(const nlohmann::json& item, const char* name) {
  if (!item.is_object() || !item.contains(name) || !item[name].is_string()) {
    return "";
  }
  return item[name].get<std::string>();
}

std::string cursor_key(const std::string& service, const std::string& type) {
  return service + "\n" + type;
}

}  // namespace

bool CredentialPool::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) return false;
  nlohmann::json config;
  try {
    in >> config;
  } catch (const nlohmann::json::exception&) {
    return false;
  }
  return load_json(config);
}

bool CredentialPool::load_json(const nlohmann::json& config) {
  if (!config.is_array() || config.empty()) return false;
  std::vector<Entry> parsed;
  parsed.reserve(config.size());
  for (const auto& item : config) {
    const std::string service = json_string(item, "service");
    const std::string key = json_string(item, "key");
    if (service.empty() || key.empty()) continue;
    std::string type = json_string(item, "type");
    if (type.empty()) type = "api_key";
    parsed.push_back(Entry{service, key, std::move(type), 0, true});
  }
  if (parsed.empty()) return false;
  std::lock_guard<std::mutex> lock(mu_);
  entries_ = std::move(parsed);
  cursors_.clear();
  return true;
}

std::string CredentialPool::get(const std::string& service,
                                const std::string& type) {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<size_t> active;
  active.reserve(entries_.size());
  for (size_t i = 0; i < entries_.size(); ++i) {
    const Entry& entry = entries_[i];
    if (entry.active && entry.service == service && entry.type == type) {
      active.push_back(i);
    }
  }
  if (active.empty()) return "";
  size_t& cursor = cursors_[cursor_key(service, type)];
  const Entry& chosen = entries_[active[cursor % active.size()]];
  ++cursor;
  return chosen.key;
}

void CredentialPool::mark_failed(const std::string& service,
                                 const std::string& key) {
  std::lock_guard<std::mutex> lock(mu_);
  for (Entry& entry : entries_) {
    if (entry.service != service || entry.key != key) continue;
    ++entry.fail_count;
    if (entry.fail_count >= 3) entry.active = false;
    return;
  }
}

nlohmann::json CredentialPool::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  nlohmann::json out = nlohmann::json::object();
  int active = 0;
  std::unordered_map<std::string, int> seen;
  for (const Entry& entry : entries_) {
    if (entry.active) ++active;
    const std::string base = entry.service + ":" + entry.type;
    const int index = seen[base]++;
    const std::string name =
        index == 0 ? base : base + "#" + std::to_string(index + 1);
    out[name] = {{"fail_count", entry.fail_count}, {"active", entry.active}};
  }
  const int total = static_cast<int>(entries_.size());
  out["_summary"] = {{"total", total},
                      {"total_entries", total},
                      {"active", active},
                      {"failed", total - active}};
  return out;
}

}  // namespace thin_agent
