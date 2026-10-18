#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {

/// API-key pool: load a JSON array, round-robin active keys, deactivate after
/// three failures. Thread-safe. Entries with an empty service or key are
/// skipped. The default key type is "api_key".
class CredentialPool {
 public:
  /// Load a JSON array from path. Missing or invalid files return false.
  bool load(const std::string& path);
  /// Replace the pool from a JSON array. Non-arrays and empty arrays fail.
  bool load_json(const nlohmann::json& config);

  /// Next active key for service. type defaults to "api_key".
  std::string get(const std::string& service,
                  const std::string& type = "api_key");

  /// Record a failure. The matching entry is deactivated on the third failure.
  void mark_failed(const std::string& service, const std::string& key);

  /// Summary plus one object per entry, keyed "service:type" and "#2", "#3".
  nlohmann::json stats() const;

 private:
  struct Entry {
    std::string service;
    std::string key;
    std::string type;
    int fail_count = 0;
    bool active = true;
  };

  mutable std::mutex mu_;
  std::vector<Entry> entries_;
  std::unordered_map<std::string, size_t> cursors_;
};

}  // namespace thin_agent
