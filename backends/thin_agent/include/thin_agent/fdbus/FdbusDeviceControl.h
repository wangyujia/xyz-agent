#pragma once

#include <string>
#include <vector>

#include "thin_agent/fdbus/IDeviceControl.h"

namespace thin_agent {

/// 真机 FDBus 设备适配器（aarch64）：通过 fdbus_clib 直连 CAMERA 服务（参考 sd-cli）。
class FdbusDeviceControl final : public IDeviceControl {
 public:
  FdbusDeviceControl();
  ~FdbusDeviceControl() override;

  FdbusDeviceControl(const FdbusDeviceControl&) = delete;
  FdbusDeviceControl& operator=(const FdbusDeviceControl&) = delete;

  Result get(const std::string& key, const Kv& args, int timeout_ms) override;
  Result set(const std::string& key, const Kv& value, int timeout_ms) override;
  Result act(const std::string& action, const Kv& args, int timeout_ms) override;
  Result evt(const std::string& op, const std::string& event, const Kv& payload) override;

 private:
  bool ensure_online(int timeout_ms);
  Result service_unavailable() const;
  Result invoke_failed(const std::string& detail) const;
  Result bad_reply(const std::string& detail) const;

  int send_dock_sync(int cmd_id, const void* payload, uint16_t plen, int timeout_ms,
                     std::vector<uint8_t>* out_reply);
  int shoot_mode_from_text(const std::string& mode) const;
  std::string shoot_mode_to_text(int mode) const;
  void ensure_shoot_mode(int mode, int timeout_ms);
  /// 等待 RECORDING_STATE 离开「进行中」(state==1)；超时返回 false。
  bool wait_recording_idle(int timeout_ms, int poll_ms = 100);

  void* client_{nullptr};  // fdb_client_t*（仅在 .cpp 中包含 fdbus 头）
  bool fdb_started_{false};
  int last_capture_ack_{0};
};

}  // namespace thin_agent
