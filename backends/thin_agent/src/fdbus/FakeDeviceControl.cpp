#include "thin_agent/fdbus/FakeDeviceControl.h"

// FakeDeviceControl：无真实硬件的 in-process 设备模拟。

namespace thin_agent {

namespace {

/// 构造带假文件路径的成功结果。
Result ok_with_path(int counter) {
  Result r;
  r.code = 0;
  r.message = "ok";
  r.data["file_path"] = "/tmp/fake_capture_" + std::to_string(counter) + ".jpg";
  return r;
}

}  // namespace

Result FakeDeviceControl::get(const std::string& key, const Kv&, int) {
  Result r;
  if (key == "recording") {
    r.code = 0;
    r.message = "ok";
    r.data["recording"] = recording_ ? "true" : "false";
    return r;
  }
  if (key == "mode") {
    r.code = 0;
    r.message = "ok";
    r.data["mode"] = mode_;
    return r;
  }
  r.code = 4004;
  r.message = "unknown key";
  return r;
}

Result FakeDeviceControl::set(const std::string& key, const Kv& value, int) {
  Result r;
  if (key == "mode") {
    auto it = value.find("mode");
    if (it == value.end()) {
      r.code = 4002;
      r.message = "missing mode";
      return r;
    }
    mode_ = it->second;
    r.code = 0;
    r.message = "ok";
    return r;
  }
  r.code = 4004;
  r.message = "unknown key";
  return r;
}

Result FakeDeviceControl::act(const std::string& action, const Kv& args, int) {
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

    Result last;
    for (int i = 0; i < count; ++i) {
      ++capture_counter_;
      last = ok_with_path(capture_counter_);
    }
    last.data["count"] = std::to_string(count);
    return last;
  }

  Result r;
  if (action == "start_recording") {
    if (recording_) {
      r.code = 5002;
      r.message = "already recording";
      return r;
    }
    recording_ = true;
    r.code = 0;
    r.message = "ok";
    if (const auto it = args.find("duration_sec"); it != args.end()) {
      r.data["duration_sec"] = it->second;
    }
    if (const auto it = args.find("mode"); it != args.end()) {
      r.data["mode"] = it->second;
    }
    return r;
  }
  if (action == "stop_recording") {
    recording_ = false;
    r.code = 0;
    r.message = "ok";
    return r;
  }
  if (action == "fetch_capture_results") {
    if (capture_counter_ <= 0) {
      r.code = 5004;
      r.message = "result not ready";
      return r;
    }
    auto out = ok_with_path(capture_counter_);
    out.data["count"] = std::to_string(capture_counter_);
    out.data["status"] = "completed";
    return out;
  }

  r.code = 4001;
  r.message = "unsupported action";
  return r;
}

Result FakeDeviceControl::evt(const std::string& op, const std::string&, const Kv&) {
  Result r;
  if (op == "sub" || op == "unsub" || op == "pub" || op == "poll") {
    r.code = 0;
    r.message = "ok";
    return r;
  }
  r.code = 4003;
  r.message = "invalid evt op";
  return r;
}

}  // namespace thin_agent
