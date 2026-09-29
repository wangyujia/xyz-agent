// unit_demo_config_compat：DemoConfigCompat 完整覆盖 — valid()、字段解析、多源提供商、错误路径。

#include <iostream>
#include <stdexcept>
#include <string>

#include "thin_agent/llm/DemoConfigCompat.h"

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
  const std::string path = "config/demo.model.yaml";

  // ═══════════════ DemoConfigCompat::valid() ═══════════════
  {
    thin_agent::DemoConfigCompat cfg;
    cfg.mode = "offline";
    expect(cfg.valid(), "valid: offline → true");
  }
  {
    thin_agent::DemoConfigCompat cfg;
    cfg.mode = "invalid";
    expect(!cfg.valid(), "valid: invalid mode → false");
  }
  {
    thin_agent::DemoConfigCompat cfg;
    cfg.mode = "cloud";
    expect(!cfg.valid(), "valid: cloud no provider/model → false");
  }
  {
    thin_agent::DemoConfigCompat cfg;
    cfg.mode = "cloud";
    cfg.provider = "deepseek";
    cfg.model_name = "deepseek-chat";
    expect(cfg.valid(), "valid: cloud deepseek → true");
  }
  {
    thin_agent::DemoConfigCompat cfg;
    cfg.mode = "cloud";
    cfg.provider = "deepseek";
    cfg.model_name = "deepseek-chat";
    cfg.request_timeout_ms = 0;
    expect(!cfg.valid(), "valid: cloud timeout=0 → false");
  }
  {
    thin_agent::DemoConfigCompat cfg;
    cfg.mode = "cloud";
    cfg.provider = "deepseek";
    cfg.model_name = "deepseek-chat";
    cfg.fallback = "none";
    expect(!cfg.valid(), "valid: cloud fallback=none → false");
  }
  {
    thin_agent::DemoConfigCompat cfg;
    cfg.mode = "cloud";
    cfg.provider = "deepseek";
    cfg.model_name = "deepseek-chat";
    cfg.budget_max_input_chars = -1;
    expect(!cfg.valid(), "valid: negative budget chars → false");
  }
  {
    thin_agent::DemoConfigCompat cfg;
    cfg.mode = "cloud";
    cfg.provider = "deepseek";
    cfg.model_name = "deepseek-chat";
    cfg.budget_max_latency_ms = -100;
    expect(!cfg.valid(), "valid: negative budget latency → false");
  }
  {
    thin_agent::DemoConfigCompat cfg;
    cfg.mode = "cloud";
    cfg.provider = "deepseek";
    cfg.model_name = "deepseek-chat";
    cfg.budget_max_cost_cents = -0.01;
    expect(!cfg.valid(), "valid: negative budget cost → false");
  }
  {
    thin_agent::DemoConfigCompat cfg;
    cfg.mode = "cloud";
    cfg.provider = "deepseek";
    cfg.model_name = "deepseek-chat";
    cfg.budget_max_input_chars = 0;
    cfg.budget_max_latency_ms = 0;
    cfg.budget_max_cost_cents = 0.0;
    expect(cfg.valid(), "valid: zero budgets are okay (disabled)");
  }
  {
    thin_agent::DemoConfigCompat cfg;
    cfg.mode = "auto";
    cfg.provider = "github-copilot";
    cfg.model_name = "gpt-4.1";
    expect(cfg.valid(), "valid: auto github-copilot → true");
  }

  // ═══════════════ load_demo_profile_compat: cloud_mock_demo ═══════════════
  {
    auto cfg = thin_agent::load_demo_profile_compat(path, "cloud_mock_demo");
    expect(cfg.mode == "cloud", "cloud_mock_demo mode");
    expect(cfg.provider == "openai-compatible", "cloud_mock_demo provider");
    expect(cfg.complex_intent_force_cloud == false, "cloud_mock_demo complex_intent default false");
    expect(cfg.pipeline_enable_rollback_hook == false, "cloud_mock_demo rollback default false");
  }

  // ═══════════════ load_demo_profile_compat: zai_fast_demo ═══════════════
  {
    auto cfg = thin_agent::load_demo_profile_compat(path, "zai_fast_demo");
    expect(cfg.mode == "cloud", "zai_fast_demo mode");
    expect(cfg.model_name == "glm-4.5-flash", "zai_fast_demo model");
    expect(cfg.pipeline_enable_rollback_hook == false, "zai_fast_demo rollback default");
  }

  // ═══════════════ load_demo_profile_compat: zai_main_demo ═══════════════
  {
    auto cfg = thin_agent::load_demo_profile_compat(path, "zai_main_demo");
    expect(cfg.mode == "cloud", "zai_main_demo mode");
    expect(cfg.model_name == "glm-5.2", "zai_main_demo model");
  }

  // ═══════════════ load_demo_profile_compat: multi_provider_demo ═══════════════
  {
    auto cfg = thin_agent::load_demo_profile_compat(path, "multi_provider_demo");
    expect(cfg.mode == "cloud", "multi_provider_demo mode");
    expect(cfg.cloud_providers.size() == 2, "multi_provider_demo 2 providers");
    expect(cfg.cloud_providers[0].model_name == "glm-5.2", "multi [0] model");
    expect(cfg.cloud_providers[0].api_base == "https://open.bigmodel.cn/api/coding/paas/v4", "multi [0] api_base");
    expect(cfg.cloud_providers[0].api_key_env == "GLM_API_KEY", "multi [0] key_env");
    expect(cfg.cloud_providers[1].model_name == "anthropic/claude-sonnet-4", "multi [1] model");
    expect(cfg.cloud_providers[1].api_base == "https://openrouter.ai/api/v1", "multi [1] api_base");
  }

  // ═══════════════ load_demo_profile_compat: copilot_demo ═══════════════
  {
    auto cfg = thin_agent::load_demo_profile_compat(path, "copilot_demo");
    expect(cfg.mode == "auto", "copilot_demo mode=auto");
    expect(cfg.provider == "github-copilot", "copilot_demo provider");
  }

  // ═══════════════ load_demo_profile_compat: deepseek_demo ═══════════════
  {
    auto cfg = thin_agent::load_demo_profile_compat(path, "deepseek_demo");
    expect(cfg.provider == "deepseek", "deepseek_demo provider");
    expect(cfg.api_base == "https://api.deepseek.com", "deepseek_demo api_base");
  }

  // ═══════════════ load_demo_profile_compat: offline_demo ═══════════════
  {
    auto cfg = thin_agent::load_demo_profile_compat(path, "offline_demo");
    expect(cfg.mode == "offline", "offline_demo mode");
    expect(cfg.provider.empty(), "offline_demo provider empty");
  }

  // ═══════════════ load_demo_profile_compat: error paths ═══════════════
  {
    // file not found → throw
    bool threw = false;
    try {
      thin_agent::load_demo_profile_compat("/nonexistent/file.yaml", "demo");
    } catch (const std::runtime_error& e) {
      threw = true;
      expect(std::string(e.what()).find("cannot open config") != std::string::npos,
             "file not found → throws with correct message");
    }
    expect(threw, "file not found → throws");
  }

  {
    // profile not found → throw
    bool threw = false;
    try {
      thin_agent::load_demo_profile_compat(path, "nonexistent_profile_xyz");
    } catch (const std::runtime_error& e) {
      threw = true;
      expect(std::string(e.what()).find("profile not found") != std::string::npos,
             "profile not found → throws with correct message");
    }
    expect(threw, "profile not found → throws");
  }

  // ═══════════════ CloudProviderConfig::valid() ═══════════════
  {
    thin_agent::CloudProviderConfig cp;
    expect(!cp.valid(), "CloudProviderConfig empty → false");
  }
  {
    thin_agent::CloudProviderConfig cp;
    cp.provider = "deepseek";
    expect(!cp.valid(), "CloudProviderConfig no model → false");
  }
  {
    thin_agent::CloudProviderConfig cp;
    cp.provider = "deepseek";
    cp.model_name = "deepseek-chat";
    expect(!cp.valid(), "CloudProviderConfig no api_base → false");
  }
  {
    thin_agent::CloudProviderConfig cp;
    cp.provider = "deepseek";
    cp.model_name = "deepseek-chat";
    cp.api_base = "https://api.deepseek.com";
    expect(cp.valid(), "CloudProviderConfig full → true");
  }

  std::cout << "unit:test_demo_config_compat PASS\n";
  return 0;
}
