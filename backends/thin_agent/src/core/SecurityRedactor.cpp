#include "thin_agent/core/SecurityRedactor.h"

// v0.54.16: 原为 `#include <regex.h>`（POSIX ERE）——**死包含**：本文件对 regcomp/regexec/
// regex_t/REG_* **零处使用**（实际走下方 `<regex>` 的 std::regex），却在 Windows 侧直接
// fatal error: regex.h: No such file or directory。删除即可（死代码零容忍）。
#include <cstring>
#include <regex>
#include <sstream>

namespace thin_agent {

namespace {

// ── 已知前缀快速匹配（长前缀优先，避免 sk- 误吞 sk-ant-） ──
struct PrefixPattern {
  const char* prefix;
  int min_total_length;  // 至少多长才算有效 secret
};

// 排序：长前缀优先
const PrefixPattern kKnownPrefixes[] = {
  {"sk-ant-api03-", 50},   // Anthropic
  {"sk-ant-", 40},
  {"sk-proj-", 40},         // OpenAI project key
  {"sk-svcacct-", 50},      // OpenAI service account
  {"sk-admin-", 40},        // OpenAI admin
  {"sk-", 28},              // OpenAI / generic
  {"deepseek-", 25},        // DeepSeek
  {"ghp_", 35},             // GitHub personal access token (classic)
  {"gho_", 35},             // GitHub OAuth
  {"ghu_", 35},             // GitHub user-to-server
  {"ghs_", 35},             // GitHub server-to-server
  {"ghr_", 35},             // GitHub refresh
  {"AKIA", 20},             // AWS access key
  {"ASIA", 20},             // AWS temporary access key
  {"xai-", 25},             // xAI / Grok
  {"hf_", 25},              // HuggingFace
  {"glm-", 25},             // Z.AI / GLM
  {"dashscope-", 25},       // Alibaba DashScope
};
constexpr int kKnownPrefixCount = sizeof(kKnownPrefixes) / sizeof(kKnownPrefixes[0]);

bool looks_like_jwt(const std::string& s) {
  // JWT: three base64url segments separated by dots
  if (s.size() < 20) return false;
  int dots = 0;
  for (char c : s) {
    if (c == '.') ++dots;
    else if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
      return false;
  }
  return dots == 2;
}

bool looks_like_base64_secret(const std::string& s) {
  // 基础 Base64 检测：长度 > 20、字符集受限、有足够熵
  if (s.size() < 20) return false;

  int upper = 0, lower = 0, digit = 0, b64special = 0;
  for (char c : s) {
    if (std::isupper(static_cast<unsigned char>(c))) ++upper;
    else if (std::islower(static_cast<unsigned char>(c))) ++lower;
    else if (std::isdigit(static_cast<unsigned char>(c))) ++digit;
    else if (c == '+' || c == '/' || c == '=') ++b64special;
    else return false;  // 非 Base64 字符
  }
  int total = upper + lower + digit + b64special;
  // 需要足够的字符多样性（至少 2 种 + 有大小写或数字）
  int categories = (upper > 0) + (lower > 0) + (digit > 0) + (b64special > 0);
  return total >= 20 && categories >= 2 && (upper > 0 || digit > 0);
}

// ── key=value 赋值检测 ──
bool is_secret_assignment(const std::string& line) {
  // 匹配 "KEY=value" 或 "KEY: value" 模式的敏感键名
  static const char* kSensitiveKeys[] = {
    "api_key", "apikey", "api-key", "api_secret", "apisecret",
    "secret", "token", "password", "passwd", "access_key",
    "access_token", "auth_token", "private_key", "client_secret",
    "app_secret", "bot_token", "webhook_secret", "signing_secret",
    "encryption_key", "master_key", "db_password", "database_url",
  };
  static const int kNumKeys = sizeof(kSensitiveKeys) / sizeof(kSensitiveKeys[0]);

  std::string lower(line);
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  for (int i = 0; i < kNumKeys; ++i) {
    const char* key = kSensitiveKeys[i];
    size_t key_len = strlen(key);
    // 查找 key= 或 key: 或 key =
    auto pos = lower.find(key);
    if (pos == std::string::npos) continue;

    // 键必须是一个完整单词（前面是边界，后面跟 = 或 : 或空格=）
    bool boundary_before = (pos == 0 ||
      !std::isalnum(static_cast<unsigned char>(lower[pos - 1])) && lower[pos - 1] != '_');
    if (!boundary_before) continue;

    size_t after = pos + key_len;
    while (after < lower.size() && lower[after] == ' ') ++after;
    if (after < lower.size() && (lower[after] == '=' || lower[after] == ':')) {
      return true;
    }
  }
  return false;
}

// ── pos = secret 起始位置，返回其结束位置（空白/引号/换行等）──
size_t secret_end(const std::string& s, size_t start) {
  size_t i = start;
  while (i < s.size()) {
    char c = s[i];
    // 允许字母、数字和常见 secret 字符
    if (std::isalnum(static_cast<unsigned char>(c)) ||
        c == '-' || c == '_' || c == '+' || c == '/' || c == '=' || c == '.') {
      ++i;
    } else {
      break;
    }
  }
  return i;
}

}  // namespace

// ═══════════════════════════════════════════════════
// redact_string
// ═══════════════════════════════════════════════════

std::string SecurityRedactor::redact_string(const std::string& input) {
  // Step 1: 前缀快速匹配（O(n) 单次扫描）
  std::string result = input;

  for (int p = 0; p < kKnownPrefixCount; ++p) {
    const auto& pp = kKnownPrefixes[p];
    size_t prefix_len = strlen(pp.prefix);

    size_t search_from = 0;
    while (true) {
      size_t pos = result.find(pp.prefix, search_from);
      if (pos == std::string::npos) break;

      size_t end = secret_end(result, pos);
      size_t match_len = end - pos;
      if (match_len >= static_cast<size_t>(pp.min_total_length)) {
        result.replace(pos, match_len, "[REDACTED]");
        search_from = pos + 10;  // "[REDACTED]" 长度
      } else {
        search_from = pos + prefix_len;
      }
    }
  }

  // Step 2: JWT 检测 (eyJ...)
  {
    size_t search_from = 0;
    while (true) {
      size_t pos = result.find("eyJ", search_from);
      if (pos == std::string::npos) break;

      size_t end = secret_end(result, pos);
      std::string candidate = result.substr(pos, end - pos);
      if (looks_like_jwt(candidate)) {
        result.replace(pos, end - pos, "[REDACTED_JWT]");
        search_from = pos + 14;
      } else {
        search_from = pos + 3;
      }
    }
  }

  // Step 3: key=value 敏感赋值（行级别）
  {
    std::istringstream iss(result);
    std::string line;
    std::ostringstream oss;
    while (std::getline(iss, line)) {
      if (is_secret_assignment(line)) {
        oss << "[REDACTED_ASSIGNMENT]\n";
      } else {
        oss << line << '\n';
      }
    }
    result = oss.str();
    // 去掉末尾多余的换行
    if (!result.empty() && result.back() == '\n') result.pop_back();
  }

  // Step 4: 通用 Base64 高熵长串（基本启发式）
  // 只在较长字符串中扫描，避免性能问题
  if (result.size() < 20) return result;

  {
    std::ostringstream oss;
    size_t i = 0;
    while (i < result.size()) {
      char c = result[i];
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' || c == '=') {
        // 收集连续候选字符
        size_t j = i;
        while (j < result.size() &&
               (std::isalnum(static_cast<unsigned char>(result[j])) ||
                result[j] == '+' || result[j] == '/' || result[j] == '=')) {
          ++j;
        }
        std::string candidate = result.substr(i, j - i);
        if (candidate.size() >= 32 && looks_like_base64_secret(candidate)) {
          oss << "[REDACTED_B64]";
        } else {
          oss << candidate;
        }
        i = j;
      } else {
        oss << c;
        ++i;
      }
    }
    result = oss.str();
  }

  return result;
}

// ═══════════════════════════════════════════════════
// redact_json
// ═══════════════════════════════════════════════════

nlohmann::json SecurityRedactor::redact_json(const nlohmann::json& input) {
  if (input.is_string()) {
    return nlohmann::json(redact_string(input.get<std::string>()));
  }
  if (input.is_array()) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& item : input) {
      out.push_back(redact_json(item));
    }
    return out;
  }
  if (input.is_object()) {
    nlohmann::json out = nlohmann::json::object();
    for (auto it = input.begin(); it != input.end(); ++it) {
      // 对敏感键名进行值红标
      std::string key_lower(it.key());
      for (char& c : key_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

      bool is_sensitive_key = false;
      if (key_lower.find("token") != std::string::npos ||
          key_lower.find("key") != std::string::npos ||
          key_lower.find("secret") != std::string::npos ||
          key_lower.find("password") != std::string::npos ||
          key_lower.find("auth") != std::string::npos ||
          key_lower.find("credential") != std::string::npos) {
        is_sensitive_key = true;
      }

      if (is_sensitive_key && it.value().is_string()) {
        out[it.key()] = "[REDACTED]";
      } else {
        out[it.key()] = redact_json(it.value());
      }
    }
    return out;
  }
  return input;  // number, bool, null → 原样返回
}

// ═══════════════════════════════════════════════════
// redact_tool_result
// ═══════════════════════════════════════════════════

nlohmann::json SecurityRedactor::redact_tool_result(const nlohmann::json& result) {
  if (!result.is_object()) return result;

  nlohmann::json out = nlohmann::json::object();
  for (auto it = result.begin(); it != result.end(); ++it) {
    const std::string& key = it.key();
    if (key == "result" || key == "output" || key == "data" || key == "error" || key == "trace") {
      out[key] = redact_json(it.value());
    } else {
      out[key] = it.value();
    }
  }
  return out;
}

const std::vector<SecurityRedactor::SecretPattern>& SecurityRedactor::patterns() {
  static const std::vector<SecretPattern> p = {
    {"Anthropic", "sk-ant-api[0-9]{2}-[A-Za-z0-9_\\-]{40,}"},
    {"OpenAI", "sk-(proj|svcacct|admin)-[A-Za-z0-9_\\-]{20,}"},
    {"Generic SK", "sk-[A-Za-z0-9_\\-]{20,}"},
    {"GitHub PAT Classic", "ghp_[A-Za-z0-9]{36,}"},
    {"GitHub OAuth", "gho_[A-Za-z0-9]{36,}"},
    {"GitHub User", "ghu_[A-Za-z0-9]{36,}"},
    {"GitHub Server", "ghs_[A-Za-z0-9]{36,}"},
    {"GitHub Refresh", "ghr_[A-Za-z0-9]{36,}"},
    {"AWS Access Key", "AKIA[0-9A-Z]{16}"},
    {"AWS Temporary", "ASIA[0-9A-Z]{16}"},
    {"JWT", "eyJ[A-Za-z0-9_\\-]+\\.[A-Za-z0-9_\\-]+\\.[A-Za-z0-9_\\-]+"},
  };
  return p;
}

}  // namespace thin_agent
