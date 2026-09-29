// unit_action_executor：ActionExecutor 全部分支覆盖 — 白名单、本地短路、设备转发、空指针、超时。

#include <cstdlib>
#include <iostream>
#include <memory>

#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"

namespace {

int g_failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++g_failures;
  }
}

}  // namespace

int main() {
  auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
  thin_agent::ActionExecutor ex(dc);

  // ── null device_control → code 5001 ──
  {
    thin_agent::ActionExecutor null_ex(nullptr);
    auto r = null_ex.execute("capture_photo", {}, 1000);
    expect(r.code == 5001, "null dc → code 5001");
    expect(r.message == "device control unavailable", "null dc → message");
  }

  // ── switch_mode → dc_->set("mode", ...) ──
  {
    thin_agent::Kv args;
    args["mode"] = "sport";
    auto r = ex.execute("switch_mode", args, 500);
    expect(r.ok(), "switch_mode sport → ok");
    expect(r.code == 0, "switch_mode code 0");
  }

  // ── switch_mode → dc_->set("mode", ...) with night mode ──
  {
    thin_agent::Kv args;
    args["mode"] = "night";
    auto r = ex.execute("switch_mode", args, 2000);
    expect(r.ok(), "switch_mode night → ok");
  }

  // ── health_report → local shortcut (no device) ──
  {
    auto r = ex.execute("health_report", {}, 1000);
    expect(r.ok(), "health_report → ok");
    expect(r.code == 0, "health_report code 0");
    expect(r.data["agent"] == "healthy", "health_report agent=healthy");
  }

  // ── collect_logs → local shortcut ──
  {
    auto r = ex.execute("collect_logs", {}, 1000);
    expect(r.ok(), "collect_logs → ok");
    expect(r.code == 0, "collect_logs code 0");
    expect(r.data["hint"] == "fake-log-bundle", "collect_logs hint");
  }

  // ── capture_photo → dc_->act ──
  {
    auto r = ex.execute("capture_photo", {}, 1000);
    expect(r.ok(), "capture_photo no args → ok");
    expect(r.data.count("file_path") == 1, "capture_photo returns file_path");
  }

  {
    thin_agent::Kv args;
    args["count"] = "3";
    auto r = ex.execute("capture_photo", args, 500);
    expect(r.ok(), "capture_photo count=3 → ok");
    expect(r.data.count("count") == 1 && r.data.at("count") == "3", "capture_photo echoes count=3");
  }

  // ── capture_photo via null dc ──
  {
    thin_agent::ActionExecutor null_ex(nullptr);
    auto r = null_ex.execute("capture_photo", {}, 1000);
    expect(r.code == 5001, "null dc capture_photo → 5001");
  }

  // ── start_recording / stop_recording ──
  {
    thin_agent::Kv args;
    args["duration_sec"] = "20";
    args["mode"] = "video";
    auto start = ex.execute("start_recording", args, 1000);
    expect(start.ok(), "start_recording video 20s → ok");
    expect(start.data.count("duration_sec") == 1 && start.data.at("duration_sec") == "20",
           "start_recording echoes duration_sec");

    auto stop = ex.execute("stop_recording", {}, 1000);
    expect(stop.ok(), "stop_recording → ok");

    // audio mode
    thin_agent::Kv audio_args;
    audio_args["duration_sec"] = "10";
    audio_args["mode"] = "audio";
    auto audio_start = ex.execute("start_recording", audio_args, 500);
    expect(audio_start.ok(), "start_recording audio 10s → ok");
    expect(audio_start.data.at("duration_sec") == "10", "audio echoes duration_sec");
  }

  // ── fetch_capture_results → dc_->act ──
  {
    auto r = ex.execute("fetch_capture_results", {}, 2000);
    expect(r.ok(), "fetch_capture_results → ok");
  }

  // ── unsupported action → code 4001 ──
  {
    auto r = ex.execute("bad_action", {}, 1000);
    expect(r.code == 4001, "bad_action → code 4001 rejected by allowlist");
  }

  {
    auto r = ex.execute("rm -rf /", {}, 1000);
    expect(r.code == 4001, "dangerous action → code 4001");
  }

  {
    auto r = ex.execute("unknown_command", {}, 1000);
    expect(r.code == 4001, "unknown_command → code 4001");
  }

  // ── timeout passthrough ──
  {
    thin_agent::Kv args;
    args["count"] = "1";
    auto r = ex.execute("capture_photo", args, 7777);
    expect(r.ok(), "capture_photo timeout=7777 → ok");
  }

  if (g_failures != 0) {
    std::cerr << g_failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "unit:test_action_executor PASS\n";
  return 0;
}
