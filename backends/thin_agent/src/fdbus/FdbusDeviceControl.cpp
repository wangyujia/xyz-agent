#include "thin_agent/fdbus/FdbusDeviceControl.h"

// FdbusDeviceControl：真机 CAMERA 服务客户端（协议与 leaptic_app/sd-cli 对齐）。

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>

#include <fdbus/fdbus_clib.h>

#include "cabin_cmd_interface.h"
#include "ql_ipc_msg.h"
#include "uart_protocol.h"

namespace thin_agent {
namespace {

constexpr int kConnectWaitMs = 5000;
constexpr int kConnectPollMs = 100;

fdb_client_t* as_client(void* handle) { return static_cast<fdb_client_t*>(handle); }

void OnCameraOnline(fdb_client_t*, FdbSessionId_t, EFdbQOS) {}
void OnCameraOffline(fdb_client_t*, FdbSessionId_t, EFdbQOS) {}

const fdb_client_handles_t kCameraHandles = {
    OnCameraOnline,
    OnCameraOffline,
    nullptr,
    nullptr,
    nullptr,
};

Result make_result(int code, std::string message, Kv data = {}) {
  Result r;
  r.code = code;
  r.message = std::move(message);
  r.data = std::move(data);
  return r;
}

}  // namespace

FdbusDeviceControl::FdbusDeviceControl() {
  if (!fdb_started_) {
    fdb_start();
    fdb_started_ = true;
  }

  char cname[64];
  snprintf(cname, sizeof(cname), "thin_agent_%d", static_cast<int>(getpid()));
  client_ = fdb_client_autoconnect(QL_SVC_CAMERA_NAME, cname, nullptr, &kCameraHandles);
}

FdbusDeviceControl::~FdbusDeviceControl() {
  if (client_) {
    fdb_client_destroy(as_client(client_));
    client_ = nullptr;
  }
}

bool FdbusDeviceControl::ensure_online(int timeout_ms) {
  fdb_client_t* cam = as_client(client_);
  if (!cam) {
    return false;
  }
  if (cam->online) {
    return true;
  }
  const int budget_ms = timeout_ms > 0 ? timeout_ms : kConnectWaitMs;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (cam->online) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kConnectPollMs));
  }
  return cam->online;
}

Result FdbusDeviceControl::service_unavailable() const {
  return make_result(5001, "fdbus service unavailable", {{"service", QL_SVC_CAMERA_NAME}});
}

Result FdbusDeviceControl::invoke_failed(const std::string& detail) const {
  return make_result(5002, "fdbus invoke failed", {{"service", QL_SVC_CAMERA_NAME}, {"detail", detail}});
}

Result FdbusDeviceControl::bad_reply(const std::string& detail) const {
  return make_result(5003, "fdbus reply invalid", {{"service", QL_SVC_CAMERA_NAME}, {"detail", detail}});
}

int FdbusDeviceControl::send_dock_sync(int cmd_id, const void* payload, uint16_t plen,
                                       int timeout_ms, std::vector<uint8_t>* out_reply) {
  fdb_client_t* cam = as_client(client_);
  if (!cam) {
    return -1;
  }
  if (static_cast<size_t>(plen) + 2 > 1024) {
    return -1;
  }

  std::vector<uint8_t> buf(plen + 2, 0);
  buf[0] = static_cast<uint8_t>(CMD_TYPE_CAMERA);
  buf[1] = static_cast<uint8_t>(cmd_id);
  if (payload && plen > 0) {
    std::memcpy(&buf[2], payload, plen);
  }

  fdb_message_t reply;
  std::memset(&reply, 0, sizeof(reply));

  const fdb_bool_t ok = fdb_client_invoke_sync(
      cam,
      QL_MSG_REQ_FROM_DOCK,
      buf.data(),
      static_cast<int32_t>(plen + 2),
      timeout_ms > 0 ? timeout_ms : 5000,
      FDB_QOS_LOCAL,
      nullptr,
      &reply);

  if (!ok || reply.status != 0 || reply.data_size < 3 || reply.msg_data == nullptr) {
    fdb_client_release_return_msg(&reply);
    return -1;
  }

  if (out_reply) {
    out_reply->assign(reply.msg_data, reply.msg_data + reply.data_size);
  }

  const int result = static_cast<int>(reply.msg_data[2]);
  fdb_client_release_return_msg(&reply);
  return result;
}

int FdbusDeviceControl::shoot_mode_from_text(const std::string& mode) const {
  if (mode == "photo") return PHOTO_MODE;
  if (mode == "video") return VIDEO_MODE;
  if (mode == "night" || mode == "night_video") return PURE_VIDEO_MODE;
  if (mode == "slowmo" || mode == "slow_motion") return SLOW_MOTION_MODE;
  if (mode == "timelapse" || mode == "time_lapse") return TIMELAPSE_MODE;
  if (mode == "loop") return LOOP_RECORDING_MODE;
  if (!mode.empty()) {
    bool digits = true;
    for (unsigned char ch : mode) {
      if (!std::isdigit(ch)) {
        digits = false;
        break;
      }
    }
    if (digits) {
      return std::atoi(mode.c_str());
    }
  }
  return 0;
}

std::string FdbusDeviceControl::shoot_mode_to_text(int mode) const {
  switch (mode) {
    case PHOTO_MODE:
      return "photo";
    case VIDEO_MODE:
      return "video";
    case PURE_VIDEO_MODE:
      return "night_video";
    case SLOW_MOTION_MODE:
      return "slowmo";
    case TIMELAPSE_MODE:
      return "timelapse";
    case LOOP_RECORDING_MODE:
      return "loop";
    default:
      return std::to_string(mode);
  }
}

void FdbusDeviceControl::ensure_shoot_mode(int mode, int timeout_ms) {
  std::vector<uint8_t> mode_out;
  const int mode_comm = send_dock_sync(GET_SHOOT_MODE, nullptr, 0, timeout_ms, &mode_out);
  if (mode_comm < 0 || mode_out.size() < 4 || mode_out[2] != 0) {
    return;
  }
  const int current = static_cast<int>(mode_out[3]);
  if (current == mode) {
    return;
  }
  set_camera_mode_req_t mode_req{};
  mode_req.shoot_mode = static_cast<SHOOT_MODE_E>(mode);
  mode_req.sub_mode = 0;
  (void)send_dock_sync(SET_SHOOT_MODE, &mode_req, sizeof(mode_req), timeout_ms, nullptr);
}

bool FdbusDeviceControl::wait_recording_idle(int timeout_ms, int poll_ms) {
  if (poll_ms < 20) poll_ms = 20;
  const int budget_ms = timeout_ms > 0 ? timeout_ms : 8000;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    std::vector<uint8_t> out;
    const int comm = send_dock_sync(RECORDING_STATE, nullptr, 0, poll_ms * 2, &out);
    if (comm >= 0 && out.size() >= 4 && out[2] == 0) {
      const int state = static_cast<int>(out[3]);
      if (state != 1) return true;  // 非「进行中」
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
  }
  return false;
}

Result FdbusDeviceControl::get(const std::string& key, const Kv&, int timeout_ms) {
  if (!ensure_online(timeout_ms)) {
    return service_unavailable();
  }

  if (key == "recording") {
    std::vector<uint8_t> out;
    const int comm = send_dock_sync(RECORDING_STATE, nullptr, 0, timeout_ms, &out);
    if (comm < 0) {
      return invoke_failed("RECORDING_STATE");
    }
    if (out.size() < 4) {
      return bad_reply("RECORDING_STATE short");
    }
    if (out[2] != 0) {
      return invoke_failed("RECORDING_STATE result");
    }
    const int state = static_cast<int>(out[3]);
    return make_result(0, "ok", {{"recording", state == 1 ? "true" : "false"}});
  }

  if (key == "mode") {
    std::vector<uint8_t> out;
    const int comm = send_dock_sync(GET_SHOOT_MODE, nullptr, 0, timeout_ms, &out);
    if (comm < 0) {
      return invoke_failed("GET_SHOOT_MODE");
    }
    if (out.size() < 4 || out[2] != 0) {
      return bad_reply("GET_SHOOT_MODE");
    }
    const int mode = static_cast<int>(out[3]);
    return make_result(0, "ok", {{"mode", shoot_mode_to_text(mode)}});
  }

  return make_result(4004, "unknown key");
}

Result FdbusDeviceControl::set(const std::string& key, const Kv& value, int timeout_ms) {
  if (!ensure_online(timeout_ms)) {
    return service_unavailable();
  }

  if (key != "mode") {
    return make_result(4004, "unknown key");
  }

  auto it = value.find("mode");
  if (it == value.end()) {
    return make_result(4002, "missing mode");
  }

  const int mode_val = shoot_mode_from_text(it->second);
  if (mode_val <= 0) {
    return make_result(4008, "invalid mode");
  }

  set_camera_mode_req_t req{};
  req.shoot_mode = static_cast<SHOOT_MODE_E>(mode_val);
  req.sub_mode = 0;
  const int ret = send_dock_sync(SET_SHOOT_MODE, &req, sizeof(req), timeout_ms, nullptr);
  if (ret < 0) {
    return invoke_failed("SET_SHOOT_MODE");
  }
  if (ret != 0) {
    return invoke_failed("SET_SHOOT_MODE result=" + std::to_string(ret));
  }
  return make_result(0, "ok");
}

Result FdbusDeviceControl::act(const std::string& action, const Kv& args, int timeout_ms) {
  if (!ensure_online(timeout_ms)) {
    return service_unavailable();
  }

  if (action == "capture_photo") {
    int count = 1;
    if (const auto it = args.find("count"); it != args.end()) {
      try {
        count = std::stoi(it->second);
      } catch (...) {
        count = 1;
      }
    }
    if (count < 1) count = 1;
    if (count > 9) count = 9;

    ensure_shoot_mode(PHOTO_MODE, timeout_ms);
    // 连拍：每张之间等待相机离开「进行中」，避免连发 START_RECORD 只成一张
    const int per_shot_wait_ms = timeout_ms > 0 ? std::max(timeout_ms, 5000) : 8000;
    for (int i = 0; i < count; ++i) {
      if (i > 0) {
        (void)wait_recording_idle(per_shot_wait_ms);
        // 状态轮询失败时仍给硬件一点间隔
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      recording_ctrl_req_t req{};
      req.ctrl = START_RECORD;
      req.delay_time = 0;
      const int ret = send_dock_sync(RECORDING_CTRL, &req, sizeof(req), timeout_ms, nullptr);
      if (ret < 0) {
        return invoke_failed("RECORDING_CTRL photo");
      }
      if (ret != 0) {
        return invoke_failed("RECORDING_CTRL photo result=" + std::to_string(ret));
      }
    }
    (void)wait_recording_idle(per_shot_wait_ms);
    last_capture_ack_ = count;
    return make_result(0, "ok", {{"async", "false"}, {"count", std::to_string(count)}});
  }

  if (action == "start_recording") {
    ensure_shoot_mode(VIDEO_MODE, timeout_ms);

    recording_ctrl_req_t req{};
    req.ctrl = START_RECORD;
    req.delay_time = 0;
    const int ret = send_dock_sync(RECORDING_CTRL, &req, sizeof(req), timeout_ms, nullptr);
    if (ret < 0) {
      return invoke_failed("RECORDING_CTRL start");
    }
    if (ret != 0) {
      return make_result(5002, "already recording or rejected");
    }
    Kv data;
    if (const auto it = args.find("duration_sec"); it != args.end()) {
      data["duration_sec"] = it->second;
    }
    data["async"] = "true";
    return make_result(0, "ok", data);
  }

  if (action == "stop_recording") {
    recording_ctrl_req_t req{};
    req.ctrl = STOP_RECORD;
    req.delay_time = 0;
    const int ret = send_dock_sync(RECORDING_CTRL, &req, sizeof(req), timeout_ms, nullptr);
    if (ret < 0) {
      return invoke_failed("RECORDING_CTRL stop");
    }
    return make_result(0, "ok");
  }

  if (action == "fetch_capture_results") {
    if (last_capture_ack_ <= 0) {
      return make_result(5004, "result not ready");
    }
    std::vector<uint8_t> out;
    const int comm = send_dock_sync(RECORDING_STATE, nullptr, 0, timeout_ms, &out);
    if (comm < 0 || out.size() < 4 || out[2] != 0) {
      return make_result(5004, "result not ready");
    }
    const int state = static_cast<int>(out[3]);
    if (state == 1) {
      return make_result(5004, "capture in progress");
    }
    return make_result(0, "ok", {{"status", "completed"}});
  }

  return make_result(4001, "unsupported action");
}

Result FdbusDeviceControl::evt(const std::string& op, const std::string&, const Kv&) {
  if (op == "sub" || op == "unsub" || op == "pub" || op == "poll") {
    return make_result(0, "ok");
  }
  return make_result(4003, "invalid evt op");
}

}  // namespace thin_agent
