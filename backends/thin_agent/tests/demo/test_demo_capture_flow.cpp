// demo_capture_flow：端到端冒烟——switch_mode → capture_photo → fetch_capture_results。

#include <iostream>
#include <memory>

#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"

int main() {
  auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
  thin_agent::ActionExecutor ex(dc);

  thin_agent::Kv mode_args;
  mode_args["mode"] = "night";
  auto mode_ret = ex.execute("switch_mode", mode_args, 1000);
  if (!mode_ret.ok()) {
    std::cerr << "demo FAIL: switch_mode\n";
    return 1;
  }

  auto cap_ret = ex.execute("capture_photo", {}, 1000);
  if (!cap_ret.ok()) {
    std::cerr << "demo FAIL: capture_photo\n";
    return 1;
  }

  auto fetch_ret = ex.execute("fetch_capture_results", {}, 1000);
  if (!fetch_ret.ok() || fetch_ret.data.count("file_path") == 0) {
    std::cerr << "demo FAIL: fetch_capture_results\n";
    return 1;
  }

  std::cout << "demo:capture_flow PASS file=" << fetch_ret.data["file_path"] << "\n";
  return 0;
}
