#pragma once

#include <memory>
#include <string>
#include <unordered_set>

#include "thin_agent/fdbus/IDeviceControl.h"

namespace thin_agent {

/// 本地设备动作执行器：对白名单内的 action 做参数校验后转发到 IDeviceControl。
/// AgentService 的即时 action 与 TaskEngine 的异步任务均通过本类落地。
class ActionExecutor {
 public:
  /// @param device_control 底层设备控制接口；为空时 execute 返回 5001。
  explicit ActionExecutor(std::shared_ptr<IDeviceControl> device_control);

  /// 执行单个动作。
  /// @param action 动作名，须在 allowlist_ 内（如 capture_photo / start_recording）。
  /// @param args   键值参数，由调用方序列化自 JSON。
  /// @param timeout_ms 下发到底层控制的超时（毫秒）。
  /// @return Result.code==0 表示成功；4001 不支持动作；5001 设备控制不可用。
  Result execute(const std::string& action, const Kv& args, int timeout_ms = 1000) const;

 private:
  std::shared_ptr<IDeviceControl> dc_;              ///< 设备控制实现（可为 mock）
  std::unordered_set<std::string> allowlist_;       ///< 允许执行的 action 白名单
};

}  // namespace thin_agent
