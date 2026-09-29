#include "thin_agent/llm/ProviderFactory.h"

#include <algorithm>

// ProviderFactory：云提供商名称规范化与能力探测。

namespace thin_agent {

namespace {

/// provider 名称转小写。
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

}  // namespace

ProviderKind detect_provider(const std::string& provider) {
  const auto p = lower(provider);
  if (p.empty()) return ProviderKind::kNone;

  if (p == "github-copilot" || p == "copilot" || p == "github") {
    return ProviderKind::kGithubCopilot;
  }
  if (p == "deepseek") {
    return ProviderKind::kDeepSeek;
  }
  if (p == "openai-compatible" || p == "openai" || p == "custom") {
    return ProviderKind::kOpenAICompatible;
  }
  if (p == "lmstudio" || p == "lm-studio" || p == "local-lmstudio" ||
      p == "lm_studio") {
    return ProviderKind::kLmStudio;  // v0.53.34
  }

  return ProviderKind::kNone;
}

std::vector<std::string> supported_providers() {
  return {
      "github-copilot",
      "deepseek",
      "openai-compatible",
      "lmstudio",  // v0.53.34
  };
}

bool is_provider_supported(const std::string& provider) {
  return detect_provider(provider) != ProviderKind::kNone;
}

std::string normalize_provider(const std::string& provider) {
  switch (detect_provider(provider)) {
    case ProviderKind::kGithubCopilot:
      return "github-copilot";
    case ProviderKind::kDeepSeek:
      return "deepseek";
    case ProviderKind::kOpenAICompatible:
      return "openai-compatible";
    case ProviderKind::kLmStudio:
      return "lmstudio";  // v0.53.34
    case ProviderKind::kNone:
    default:
      return "";
  }
}

}  // namespace thin_agent
