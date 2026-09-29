#pragma once

#include <string>
#include <unordered_map>

#include "thin_agent/core/Result.h"

namespace thin_agent {

/// 设备控制键值参数，ActionExecutor 将 JSON args 序列化为此类型后下发。
using Kv = std::unordered_map<std::string, std::string>;

/// 设备控制抽象接口：读状态(get)、写配置(set)、执行动作(act)、事件通道(evt)。
/// 实现类：FakeDeviceControl（开发/测试）、FdbusDeviceControl（真机 aarch64，待集成）。
class IDeviceControl {
 public:
  virtual ~IDeviceControl() = default;

  /// 读取设备状态或属性，如 recording / mode。
  virtual Result get(const std::string& key, const Kv& args, int timeout_ms) = 0;

  /// 写入设备配置，如 switch_mode。
  virtual Result set(const std::string& key, const Kv& value, int timeout_ms) = 0;

  /// 执行一次性动作，如 capture_photo / start_recording。
  virtual Result act(const std::string& action, const Kv& args, int timeout_ms) = 0;

  /// 事件订阅/发布/轮询（sub / unsub / pub / poll）。
  virtual Result evt(const std::string& op, const std::string& event, const Kv& payload) = 0;
};

}  // namespace thin_agent
