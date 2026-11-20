#include "thin_agent/llm/DemoConfigCompat.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>

// DemoConfigCompat：解析 demo.model.yaml 中单个 profile 为 Agent 运行配置。

namespace thin_agent {

namespace {

/// 去除 YAML 行首尾空白。
std::string trim(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
  return s;
}

/// 去除 YAML 值行内注释（# 前有空格才算注释——URL 片段 #tag 不受影响）。
/// v0.53.36: 此前不剥——`api_base: ""  # 注释` 把注释连同引号读进值,
/// provider 能走通但 api_base 变垃圾串(实测 lmstudio 链路排障发现)。
std::string strip_inline_comment(const std::string& s) {
  bool in_squote = false, in_dquote = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '\'' && !in_dquote) in_squote = !in_squote;
    else if (c == '"' && !in_squote) in_dquote = !in_dquote;
    else if (c == '#' && !in_squote && !in_dquote &&
             (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))
      return s.substr(0, i);
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

std::string canonicalize_access_profile(std::string profile) {
  if (profile == "offline_demo") return "offline_mode";
  if (profile == "local_tools_demo") return "local_tools_mode";
  if (profile == "lmstudio_demo") return "lmstudio_mode";
  if (profile == "openai_compatible_demo") return "cloud_api_mode";
  return profile;
}

std::vector<std::string> access_profile_lookup_keys(const std::string& profile) {
  std::vector<std::string> keys;
  auto add = [&](const std::string& k) {
    if (k.empty()) return;
    for (const auto& e : keys) {
      if (e == k) return;
    }
    keys.push_back(k);
  };
  add(profile);
  add(canonicalize_access_profile(profile));
  // 新旧互逆：家目录 YAML 可能仍是 *_demo 键，或请求旧名而仓库已是 *_mode。
  static const char* pairs[][2] = {
      {"offline_mode", "offline_demo"},
      {"local_tools_mode", "local_tools_demo"},
      {"lmstudio_mode", "lmstudio_demo"},
      {"cloud_api_mode", "openai_compatible_demo"},
  };
  for (const auto& p : pairs) {
    if (profile == p[0] || profile == p[1] ||
        canonicalize_access_profile(profile) == p[0]) {
      add(p[0]);
      add(p[1]);
    }
  }
  return keys;
}

bool DemoConfigCompat::valid() const {
  if (mode != "offline" && mode != "cloud" && mode != "auto") return false;
  if (mode == "offline") return true;
  if (provider.empty() || model_name.empty()) return false;
  if (request_timeout_ms <= 0) return false;
  if (fallback != "offline" && fallback != "cloud") return false;
  if (budget_max_input_chars < 0) return false;
  if (budget_max_latency_ms < 0) return false;
  if (budget_max_cost_cents < 0.0) return false;
  return true;
}

DemoConfigCompat load_demo_profile_compat(const std::string& yaml_path, const std::string& profile) {
  const auto keys = access_profile_lookup_keys(profile);
  std::ifstream in(yaml_path);
  if (!in.is_open()) in.open("../" + yaml_path);
  if (!in.is_open()) throw std::runtime_error("cannot open config: " + yaml_path);

  DemoConfigCompat cfg;
  std::string line;
  bool in_profiles = false;
  bool in_target = false;
  std::string resolved;

  while (std::getline(in, line)) {
    if (starts_with(line, "profiles:")) { in_profiles = true; continue; }
    if (!in_profiles) continue;

    if (!in_target) {
      for (const auto& key : keys) {
        const std::string ptag = "  " + key + ":";
        if (line == ptag) {
          in_target = true;
          resolved = key;
          break;
        }
      }
      if (in_target) continue;
      continue;
    }

    if (in_target) {
      // 遇到下一个同级 profile 键则结束当前块解析。
      if (starts_with(line, "  ") && line.find(':') != std::string::npos && line.size() > 2 && line[2] != ' ') {
        break;
      }

      auto pick = [&](const std::string& key) -> std::string {
        const std::string k = "    " + key + ":";
        if (!starts_with(line, k)) return "";
        return unquote(trim(strip_inline_comment(trim(line.substr(k.size())))));
      };

      if (auto v = pick("mode"); !v.empty()) cfg.mode = v;
      if (auto v = pick("provider"); !v.empty()) cfg.provider = v;
      if (auto v = pick("name"); !v.empty()) cfg.model_name = v;
      if (auto v = pick("api_base"); !v.empty()) cfg.api_base = v;
      if (auto v = pick("api_key_env"); !v.empty()) cfg.api_key_env = v;
      if (auto v = pick("fallback"); !v.empty()) cfg.fallback = v;
      if (auto v = pick("api_mode"); !v.empty()) cfg.api_mode = v;
      if (auto v = pick("request_timeout_ms"); !v.empty()) cfg.request_timeout_ms = std::stoi(v);
      // v0.52.9: completion 预算可配（思考型模型 reasoning 与 content 共享，
      // 2048 下 reasoning 耗尽预算 content 空——真 e2e 第十二轮子代理连败）
      if (auto v = pick("max_completion_tokens"); !v.empty()) cfg.max_completion_tokens = std::stoi(v);
      if (auto v = pick("budget_max_input_chars"); !v.empty()) cfg.budget_max_input_chars = std::stoi(v);
      if (auto v = pick("budget_max_latency_ms"); !v.empty()) cfg.budget_max_latency_ms = std::stoi(v);
      if (auto v = pick("budget_max_cost_cents"); !v.empty()) cfg.budget_max_cost_cents = std::stod(v);
      if (auto v = pick("complex_intent_force_cloud"); !v.empty()) cfg.complex_intent_force_cloud = (v == "true" || v == "True" || v == "1");
      if (auto v = pick("pipeline_enable_rollback_hook"); !v.empty()) cfg.pipeline_enable_rollback_hook = (v == "true" || v == "True" || v == "1");
      if (auto v = pick("external_provider"); !v.empty()) cfg.external_provider = v;
      if (auto v = pick("external_weather_url"); !v.empty()) cfg.external_weather_url = v;
      if (auto v = pick("sidecar_enabled"); !v.empty())
        cfg.sidecar_enabled = (v == "true" || v == "True" || v == "1");
      if (auto v = pick("sidecar_server_path"); !v.empty()) cfg.sidecar_server_path = v;
      if (auto v = pick("sidecar_model_path"); !v.empty()) cfg.sidecar_model_path = v;
      if (auto v = pick("sidecar_host"); !v.empty()) cfg.sidecar_host = v;
      if (auto v = pick("sidecar_port"); !v.empty()) cfg.sidecar_port = std::stoi(v);
      if (auto v = pick("sidecar_n_ctx"); !v.empty()) cfg.sidecar_n_ctx = std::stoi(v);
      if (auto v = pick("sidecar_n_gpu_layers"); !v.empty())
        cfg.sidecar_n_gpu_layers = std::stoi(v);
      if (auto v = pick("sidecar_ready_timeout_ms"); !v.empty())
        cfg.sidecar_ready_timeout_ms = std::stoi(v);

      // cloud_providers 列表解析
      if (starts_with(line, "    cloud_providers:")) {
        CloudProviderConfig cur;
        while (std::getline(in, line)) {
          // 列表项开头 "- provider:" 或 next key / end of profile
          if (!starts_with(line, "      ") && !starts_with(line, "      -")) {
            if (cur.valid()) cfg.cloud_providers.push_back(cur);
            cur = CloudProviderConfig{};
            break;  // 返回外层 while 重新处理当前行
          }
          // 新列表项 "- provider: xxx"
          if (starts_with(trim(line), "- ")) {
            if (cur.valid()) cfg.cloud_providers.push_back(cur);
            cur = CloudProviderConfig{};
            // 提取 "- provider: xxx" 中的 provider
            auto dash_val = trim(line).substr(2);
            auto colon = dash_val.find(':');
            if (colon != std::string::npos) {
              auto k = trim(dash_val.substr(0, colon));
              auto v = unquote(trim(dash_val.substr(colon + 1)));
              if (k == "provider") cur.provider = v;
              else if (k == "name") cur.model_name = v;
              else if (k == "api_base") cur.api_base = v;
              else if (k == "api_key_env") cur.api_key_env = v;
              else if (k == "request_timeout_ms") cur.request_timeout_ms = std::stoi(v);
            }
            continue;
          }
          // 子字段 "      key: value"
          auto sub_pick = [&](const std::string& key) -> std::string {
            const std::string k = "        " + key + ":";
            if (!starts_with(line, k)) return "";
            return unquote(trim(line.substr(k.size())));
          };
          if (auto v = sub_pick("provider"); !v.empty()) cur.provider = v;
          if (auto v = sub_pick("name"); !v.empty()) cur.model_name = v;
          if (auto v = sub_pick("api_base"); !v.empty()) cur.api_base = v;
          if (auto v = sub_pick("api_key_env"); !v.empty()) cur.api_key_env = v;
          if (auto v = sub_pick("request_timeout_ms"); !v.empty()) cur.request_timeout_ms = std::stoi(v);
        }
        if (cur.valid()) cfg.cloud_providers.push_back(cur);
        continue;  // 回到外层 while，line 已被消费
      }
    }
  }

  if (!in_target) {
    std::string joined;
    for (size_t i = 0; i < keys.size(); ++i) {
      if (i) joined += "|";
      joined += keys[i];
    }
    throw std::runtime_error("profile not found: " + profile + " (tried " + joined + ")");
  }
  return cfg;
}

namespace {

std::string profile_label_for(const DemoProfileInfo& info) {
  if (info.name == "offline_mode" || info.name == "offline_demo") return "离线 / 进程内 GGUF";
  if (info.name == "local_tools_mode" || info.name == "local_tools_demo")
    return "本地 Tools（sidecar）";
  if (info.name == "lmstudio_mode" || info.name == "lmstudio_demo")
    return "外接 LM Studio";
  if (info.name == "cloud_api_mode" || info.name == "openai_compatible_demo")
    return "云 API（OpenAI 兼容）";
  if (info.provider == "lmstudio") return "本地 OpenAI 端点";
  if (info.name.find("zai_") == 0 || info.provider.find("openai-compatible") == 0) {
    if (info.model.find("glm") != std::string::npos || info.name.find("zai") != std::string::npos)
      return "智谱 " + (info.model.empty() ? info.name : info.model);
  }
  if (info.name.find("deepseek") != std::string::npos || info.provider == "deepseek")
    return "DeepSeek " + (info.model.empty() ? info.name : info.model);
  if (info.provider == "github-copilot") return "GitHub Copilot";
  if (!info.model.empty()) return info.name + " · " + info.model;
  return info.name;
}

bool profile_needs_key(const DemoProfileInfo& info) {
  if (info.mode == "offline") return false;
  if (info.provider == "lmstudio" || info.provider == "lm-studio" ||
      info.provider == "local-lmstudio")
    return false;
  return !info.api_key_env.empty();
}

}  // namespace

std::vector<DemoProfileInfo> list_demo_profiles(const std::string& yaml_path) {
  std::vector<DemoProfileInfo> out;
  std::ifstream in(yaml_path);
  if (!in.is_open()) in.open("../" + yaml_path);
  if (!in.is_open()) return out;

  std::string line;
  bool in_profiles = false;
  DemoProfileInfo cur;
  bool have = false;

  auto flush = [&]() {
    if (!have || cur.name.empty()) return;
    cur.needs_key = profile_needs_key(cur);
    if (cur.needs_key && !cur.api_key_env.empty()) {
      const char* v = std::getenv(cur.api_key_env.c_str());
      cur.has_key = (v && *v);
    } else {
      cur.has_key = !cur.needs_key;
    }
    cur.label = profile_label_for(cur);
    out.push_back(cur);
    cur = DemoProfileInfo{};
    have = false;
  };

  while (std::getline(in, line)) {
    if (starts_with(line, "profiles:")) {
      in_profiles = true;
      continue;
    }
    if (!in_profiles) continue;

    // 同级 profile 键：两位空格 + 名称 + 冒号，第三位不是空格
    if (starts_with(line, "  ") && line.size() > 2 && line[2] != ' ' &&
        line.find(':') != std::string::npos) {
      flush();
      auto colon = line.find(':');
      cur.name = trim(line.substr(2, colon - 2));
      have = true;
      continue;
    }

    if (!have) continue;

    auto pick = [&](const std::string& key) -> std::string {
      const std::string k = "    " + key + ":";
      if (!starts_with(line, k)) return "";
      return unquote(trim(strip_inline_comment(trim(line.substr(k.size())))));
    };
    if (auto v = pick("mode"); !v.empty()) cur.mode = v;
    if (auto v = pick("provider"); !v.empty()) cur.provider = v;
    if (auto v = pick("name"); !v.empty()) cur.model = v;
    if (auto v = pick("api_base"); !v.empty()) cur.api_base = v;
    if (auto v = pick("api_key_env"); !v.empty()) cur.api_key_env = v;
  }
  flush();
  return out;
}

}  // namespace thin_agent
