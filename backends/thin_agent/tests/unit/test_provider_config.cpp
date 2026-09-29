// unit_provider_config：ProviderFactory 全部别名/大小写/边界 + detect_provider + normalize_provider + ModelConfig::valid。

#include <iostream>
#include <string>

#include "thin_agent/llm/ModelConfig.h"
#include "thin_agent/llm/ProviderFactory.h"

namespace {

int g_failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++g_failures;
  }
}

}  // namespace

int main() {
  using namespace thin_agent;

  // ═══════════════ detect_provider ═══════════════
  {
    expect(detect_provider("") == ProviderKind::kNone, "detect empty → kNone");
    expect(detect_provider("unknown-vendor-xyz") == ProviderKind::kNone, "detect unknown → kNone");
  }

  // GitHub Copilot aliases
  {
    expect(detect_provider("github-copilot") == ProviderKind::kGithubCopilot, "detect github-copilot");
    expect(detect_provider("copilot") == ProviderKind::kGithubCopilot, "detect copilot alias");
    expect(detect_provider("github") == ProviderKind::kGithubCopilot, "detect github alias");
    expect(detect_provider("GitHub-Copilot") == ProviderKind::kGithubCopilot, "detect case-insensitive");
    expect(detect_provider("COPILOT") == ProviderKind::kGithubCopilot, "detect COPILOT uppercase");
    expect(detect_provider("GitHub") == ProviderKind::kGithubCopilot, "detect GitHub alias case");
  }

  // DeepSeek
  {
    expect(detect_provider("deepseek") == ProviderKind::kDeepSeek, "detect deepseek");
    expect(detect_provider("DeepSeek") == ProviderKind::kDeepSeek, "detect DeepSeek case");
    expect(detect_provider("DEEPSEEK") == ProviderKind::kDeepSeek, "detect DEEPSEEK uppercase");
  }

  // LM Studio（v0.53.34 本地推理）
  {
    expect(detect_provider("lmstudio") == ProviderKind::kLmStudio, "detect lmstudio");
    expect(detect_provider("lm-studio") == ProviderKind::kLmStudio, "detect lm-studio alias");
    expect(detect_provider("local-lmstudio") == ProviderKind::kLmStudio, "detect local-lmstudio alias");
    expect(detect_provider("LMStudio") == ProviderKind::kLmStudio, "detect case-insensitive");
    expect(normalize_provider("lm-studio") == "lmstudio", "normalize lm-studio");
    expect(is_provider_supported("lmstudio"), "lmstudio supported");
  }

  // OpenAI-compatible aliases
  {
    expect(detect_provider("openai-compatible") == ProviderKind::kOpenAICompatible, "detect openai-compatible");
    expect(detect_provider("openai") == ProviderKind::kOpenAICompatible, "detect openai alias");
    expect(detect_provider("custom") == ProviderKind::kOpenAICompatible, "detect custom alias");
    expect(detect_provider("OpenAI") == ProviderKind::kOpenAICompatible, "detect OpenAI case");
    expect(detect_provider("OPENAI-COMPATIBLE") == ProviderKind::kOpenAICompatible, "detect OPENAI-COMPATIBLE case");
  }

  // ═══════════════ supported_providers ═══════════════
  {
    auto providers = supported_providers();
    expect(providers.size() == 4, "supported_providers returns 4 entries (v0.53.34 +lmstudio)");
    expect(providers[0] == "github-copilot", "supported[0]=github-copilot");
    expect(providers[1] == "deepseek", "supported[1]=deepseek");
    expect(providers[2] == "openai-compatible", "supported[2]=openai-compatible");
  }

  // ═══════════════ is_provider_supported ═══════════════
  {
    expect(is_provider_supported("copilot"), "copilot supported");
    expect(is_provider_supported("github-copilot"), "github-copilot supported");
    expect(is_provider_supported("deepseek"), "deepseek supported");
    expect(is_provider_supported("DeepSeek"), "DeepSeek supported (case)");
    expect(is_provider_supported("openai"), "openai supported");
    expect(is_provider_supported("openai-compatible"), "openai-compatible supported");
    expect(is_provider_supported("custom"), "custom supported");
    expect(!is_provider_supported("unknown"), "unknown NOT supported");
    expect(!is_provider_supported(""), "empty NOT supported");
    expect(!is_provider_supported("anthropic"), "anthropic NOT supported");
  }

  // ═══════════════ normalize_provider ═══════════════
  {
    expect(normalize_provider("copilot") == "github-copilot", "copilot → github-copilot");
    expect(normalize_provider("github") == "github-copilot", "github → github-copilot");
    expect(normalize_provider("GitHub-Copilot") == "github-copilot", "case → github-copilot");
    expect(normalize_provider("deepseek") == "deepseek", "deepseek → deepseek");
    expect(normalize_provider("DeepSeek") == "deepseek", "DeepSeek → deepseek");
    expect(normalize_provider("openai") == "openai-compatible", "openai → openai-compatible");
    expect(normalize_provider("custom") == "openai-compatible", "custom → openai-compatible");
    expect(normalize_provider("openai-compatible") == "openai-compatible", "openai-compatible → openai-compatible");
    expect(normalize_provider("") == "", "empty → empty");
    expect(normalize_provider("unknown") == "", "unknown → empty");
  }

  // ═══════════════ ModelConfig::valid ═══════════════
  // invalid mode
  {
    ModelConfig c;
    c.mode = "invalid_mode";
    expect(!c.valid(), "invalid mode → false");
  }

  // offline: valid without provider/model (no further checks)
  {
    ModelConfig c;
    c.mode = "offline";
    expect(c.valid(), "offline → valid");
  }

  // cloud: missing provider
  {
    ModelConfig c;
    c.mode = "cloud";
    c.model_name = "gpt-4";
    expect(!c.valid(), "cloud missing provider → false");
  }

  // cloud: missing model_name
  {
    ModelConfig c;
    c.mode = "cloud";
    c.provider = "github-copilot";
    expect(!c.valid(), "cloud missing model → false");
  }

  // cloud: both missing
  {
    ModelConfig c;
    c.mode = "cloud";
    expect(!c.valid(), "cloud both missing → false");
  }

  // cloud: valid
  {
    ModelConfig c;
    c.mode = "cloud";
    c.provider = "github-copilot";
    c.model_name = "gpt-4.1";
    c.api_key_env = "COPILOT_API_KEY";
    expect(c.valid(), "cloud github-copilot → valid");
  }

  // cloud: zero timeout
  {
    ModelConfig c;
    c.mode = "cloud";
    c.provider = "deepseek";
    c.model_name = "deepseek-chat";
    c.request_timeout_ms = 0;
    expect(!c.valid(), "cloud timeout=0 → false");
  }

  // cloud: negative timeout
  {
    ModelConfig c;
    c.mode = "cloud";
    c.provider = "deepseek";
    c.model_name = "deepseek-chat";
    c.request_timeout_ms = -500;
    expect(!c.valid(), "cloud timeout=-500 → false");
  }

  // cloud: invalid fallback
  {
    ModelConfig c;
    c.mode = "cloud";
    c.provider = "deepseek";
    c.model_name = "deepseek-chat";
    c.fallback = "none";
    expect(!c.valid(), "cloud fallback=none → false");
  }

  // cloud: fallback=offline (valid)
  {
    ModelConfig c;
    c.mode = "cloud";
    c.provider = "deepseek";
    c.model_name = "deepseek-chat";
    c.fallback = "offline";
    expect(c.valid(), "cloud fallback=offline → valid");
  }

  // cloud: fallback=cloud (valid)
  {
    ModelConfig c;
    c.mode = "cloud";
    c.provider = "deepseek";
    c.model_name = "deepseek-chat";
    c.fallback = "cloud";
    expect(c.valid(), "cloud fallback=cloud → valid");
  }

  // auto mode valid
  {
    ModelConfig c;
    c.mode = "auto";
    c.provider = "deepseek";
    c.model_name = "deepseek-chat";
    c.api_key_env = "DEEPSEEK_API_KEY";
    expect(c.valid(), "auto deepseek → valid");
  }

  // auto mode invalid (missing provider)
  {
    ModelConfig c;
    c.mode = "auto";
    c.model_name = "deepseek-chat";
    expect(!c.valid(), "auto missing provider → false");
  }

  // ── legacy checks from original test ──
  {
    ModelConfig c;
    c.mode = "offline";
    expect(c.valid(), "legacy: offline valid without provider/model");
  }
  {
    ModelConfig c;
    c.mode = "cloud";
    c.provider = "github-copilot";
    c.model_name = "gpt-4.1";
    c.api_key_env = "COPILOT_API_KEY";
    expect(c.valid(), "legacy: cloud github-copilot valid");
  }
  {
    ModelConfig c;
    c.mode = "auto";
    c.provider = "deepseek";
    c.model_name = "deepseek-chat";
    c.api_key_env = "DEEPSEEK_API_KEY";
    expect(c.valid(), "legacy: auto deepseek valid");
  }
  {
    ModelConfig c;
    c.mode = "cloud";
    c.provider = "";
    c.model_name = "deepseek-chat";
    expect(!c.valid(), "legacy: cloud missing provider invalid");
  }

  if (g_failures != 0) {
    std::cerr << g_failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "unit:test_provider_config PASS\n";
  return 0;
}
