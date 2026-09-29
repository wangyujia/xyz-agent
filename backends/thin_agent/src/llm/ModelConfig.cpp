#include "thin_agent/llm/ModelConfig.h"

#include <fstream>

// ModelConfig：精简模型配置校验与 YAML profile 加载（DemoConfigLoader 基础版）。

namespace thin_agent {

namespace {

/// 去除 YAML 行首尾空白。
std::string trim_line(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
    s.pop_back();
  }
  return s;
}

/// 去除 YAML 值两侧引号。
std::string unquote(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
  return s;
}

/// 判断字符串是否以指定前缀开头。
bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }

}  // namespace

bool ModelConfig::valid() const {
  if (mode != "offline" && mode != "cloud" && mode != "auto") {
    return false;
  }

  if (mode == "offline") {
    return true;
  }

  if (provider.empty() || model_name.empty()) {
    return false;
  }

  if (request_timeout_ms <= 0) {
    return false;
  }

  if (fallback != "offline" && fallback != "cloud") {
    return false;
  }

  return true;
}

ModelConfig load_demo_profile(const std::string& yaml_path, const std::string& profile) {
  std::ifstream in(yaml_path);
  if (!in.is_open()) in.open("../" + yaml_path);

  ModelConfig cfg;
  if (!in.is_open()) return cfg;

  std::string line;
  bool in_profiles = false;
  bool in_target = false;

  while (std::getline(in, line)) {
    if (starts_with(line, "profiles:")) {
      in_profiles = true;
      continue;
    }
    if (!in_profiles) continue;

    const std::string ptag = "  " + profile + ":";
    if (line == ptag) {
      in_target = true;
      continue;
    }

    if (in_target) {
      if (starts_with(line, "  ") && line.find(':') != std::string::npos && line.size() > 2 && line[2] != ' ') {
        break;
      }

      auto pick = [&](const std::string& key) -> std::string {
        const std::string k = "    " + key + ":";
        if (!starts_with(line, k)) return "";
        return unquote(trim_line(line.substr(k.size())));
      };

      if (auto v = pick("mode"); !v.empty()) cfg.mode = v;
      if (auto v = pick("provider"); !v.empty()) cfg.provider = v;
      if (auto v = pick("name"); !v.empty()) cfg.model_name = v;
      if (auto v = pick("api_base"); !v.empty()) cfg.api_base = v;
      if (auto v = pick("api_key_env"); !v.empty()) cfg.api_key_env = v;
      if (auto v = pick("fallback"); !v.empty()) cfg.fallback = v;
      if (auto v = pick("request_timeout_ms"); !v.empty()) cfg.request_timeout_ms = std::stoi(v);
    }
  }

  return cfg;
}

}  // namespace thin_agent
