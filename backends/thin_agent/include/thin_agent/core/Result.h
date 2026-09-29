#pragma once

#include <string>
#include <unordered_map>

namespace thin_agent {

/// 统一动作/设备调用返回结构，贯穿 IDeviceControl → ActionExecutor → TaskEngine。
struct Result {
  int code{0};                                      ///< 0 成功；4xxx 客户端/参数；5xxx 服务端/设备
  std::string message;                              ///< 人类可读说明
  std::unordered_map<std::string, std::string> data;  ///< 结构化附加字段（如 file_path、mode）

  /// 是否成功（code == 0）。
  bool ok() const { return code == 0; }
};

}  // namespace thin_agent
