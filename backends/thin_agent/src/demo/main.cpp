#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"
#include "thin_agent/llm/ModelConfig.h"
#include "thin_agent/llm/ProviderFactory.h"
#include "thin_agent/RuntimePaths.h"

// thin_agent_demo：命令行演示程序。
// 加载 YAML profile → 校验 provider → 跑一轮 FakeDeviceControl 动作冒烟（switch_mode / 拍照 / 取结果）。

int main(int argc, char** argv) {
  std::string profile = "offline_demo";
  if (argc >= 2) profile = argv[1];

  const auto cfg = thin_agent::load_demo_profile(thin_agent::default_demo_model_config_path(), profile);
  const auto normalized = thin_agent::normalize_provider(cfg.provider);

  std::cout << "[thin_agent_demo] profile=" << profile << "\n";
  std::cout << "  mode=" << cfg.mode << "\n";
  std::cout << "  provider=" << (normalized.empty() ? cfg.provider : normalized) << "\n";
  std::cout << "  model=" << cfg.model_name << "\n";
  std::cout << "  api_base=" << cfg.api_base << "\n";
  std::cout << "  api_key_env=" << cfg.api_key_env << "\n";
  std::cout << "  fallback=" << cfg.fallback << "\n";

  if (!cfg.provider.empty() && !thin_agent::is_provider_supported(cfg.provider)) {
    std::cerr << "unsupported provider: " << cfg.provider << "\n";
    return 2;
  }

  if (!cfg.api_key_env.empty()) {
    const char* val = std::getenv(cfg.api_key_env.c_str());
    std::cout << "  key_present=" << ((val && *val) ? "yes" : "no") << "\n";
  }

  auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
  thin_agent::ActionExecutor ex(dc);

  thin_agent::Kv mode_args;
  mode_args["mode"] = "night";
  auto r1 = ex.execute("switch_mode", mode_args, 1000);
  auto r2 = ex.execute("capture_photo", {}, 1000);
  auto r3 = ex.execute("fetch_capture_results", {}, 1000);

  if (!r1.ok() || !r2.ok() || !r3.ok()) {
    std::cerr << "demo action flow failed\n";
    return 3;
  }

  std::cout << "[thin_agent_demo] fake action flow PASS file=" << r3.data["file_path"] << "\n";
  return 0;
}
