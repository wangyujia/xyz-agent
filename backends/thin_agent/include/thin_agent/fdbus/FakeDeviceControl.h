#pragma once

#include <string>
#include <unordered_map>

#include "thin_agent/fdbus/IDeviceControl.h"

namespace thin_agent {

/// 内存模拟设备：拍照返回假路径、录制切换布尔状态，供 demo/单测/WS 服务使用。
class FakeDeviceControl final : public IDeviceControl {
 public:
  FakeDeviceControl() = default;

  /// 读取 recording / mode 等模拟状态。
  Result get(const std::string& key, const Kv& args, int timeout_ms) override;
  /// 写入 mode 等配置。
  Result set(const std::string& key, const Kv& value, int timeout_ms) override;
  /// 执行拍照、录制等动作并返回假文件路径。
  Result act(const std::string& action, const Kv& args, int timeout_ms) override;
  /// 事件通道占位：sub/unsub/pub/poll 均返回成功。
  Result evt(const std::string& op, const std::string& event, const Kv& payload) override;

 private:
  bool recording_{false};     ///< 是否处于录制中
  int capture_counter_{0};  ///< 拍照计数，用于生成假文件路径
  std::string mode_{"normal"};  ///< 当前运行模式
};

}  // namespace thin_agent
