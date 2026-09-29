#pragma once

#include <string>
#include <vector>

#include "thin_agent/llm/ModelConfig.h"

namespace thin_agent {

/// 支持的云提供商枚举。
enum class ProviderKind {
  kNone = 0,
  kGithubCopilot,
  kDeepSeek,
  kOpenAICompatible,
  kLmStudio,  // v0.53.34: 本地推理（LM Studio 等 OpenAI 兼容本地端点）
};

/// 将配置字符串映射为 ProviderKind（大小写不敏感，支持别名）。
ProviderKind detect_provider(const std::string& provider);

/// 返回当前构建支持的 provider 名称列表。
std::vector<std::string> supported_providers();

/// 判断 provider 是否在支持列表内。
bool is_provider_supported(const std::string& provider);

/// 将别名规范化为 canonical 名称（如 copilot → github-copilot）。
std::string normalize_provider(const std::string& provider);

}  // namespace thin_agent
