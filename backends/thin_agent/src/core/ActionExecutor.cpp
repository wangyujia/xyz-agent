#include "thin_agent/core/ActionExecutor.h"

// ActionExecutor：动作白名单网关，部分动作在本地短路（health_report/collect_logs），
// 其余转发 IDeviceControl::act / set。

namespace thin_agent {

ActionExecutor::ActionExecutor(std::shared_ptr<IDeviceControl> device_control)
    : dc_(std::move(device_control)),
      allowlist_({"health_report", "collect_logs", "switch_mode", "capture_photo",
                  "start_recording", "stop_recording", "fetch_capture_results"}) {}

Result ActionExecutor::execute(const std::string& action, const Kv& args, int timeout_ms) const {
  if (!dc_) {
    return Result{5001, "device control unavailable", {}};
  }

  if (allowlist_.find(action) == allowlist_.end()) {
    return Result{4001, "unsupported action", {}};
  }

  // switch_mode 走 set 通道，其余设备动作走 act。
  if (action == "switch_mode") {
    return dc_->set("mode", args, timeout_ms);
  }

  // 本地探活/日志收集，不触达真实设备。
  if (action == "health_report") {
    Result r;
    r.code = 0;
    r.message = "ok";
    r.data["agent"] = "healthy";
    return r;
  }

  if (action == "collect_logs") {
    Result r;
    r.code = 0;
    r.message = "ok";
    r.data["hint"] = "fake-log-bundle";
    return r;
  }

  return dc_->act(action, args, timeout_ms);
}

}  // namespace thin_agent
