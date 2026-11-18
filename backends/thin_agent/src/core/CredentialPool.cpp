#include "thin_agent/core/CredentialPool.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace thin_agent {

void CredentialPool::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    raw_.clear();
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  raw_ = ss.str();
}

std::string CredentialPool::get(const std::string& provider,
                                const std::string& field) const {
  if (raw_.empty() || provider.empty() || field.empty()) return "";
  try {
    const auto j = nlohmann::json::parse(raw_);
    if (!j.is_object() || !j.contains(provider)) return "";
    const auto& entry = j.at(provider);
    if (entry.is_object() && entry.contains(field) && entry.at(field).is_string())
      return entry.at(field).get<std::string>();
    if (entry.is_string() && field == "api_key") return entry.get<std::string>();
  } catch (...) {
  }
  return "";
}

}  // namespace thin_agent
