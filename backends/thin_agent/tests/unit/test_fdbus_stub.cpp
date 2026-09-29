// unit_fdbus_stub：无 CAMERA 服务时 FdbusDeviceControl 应返回服务不可用或调用失败。

#include <iostream>

#include "thin_agent/fdbus/FdbusDeviceControl.h"

int main() {
#if THIN_AGENT_WITH_FDBUS != 1
  std::cerr << "THIN_AGENT_WITH_FDBUS is expected to be ON in this test\n";
  return 1;
#endif

  thin_agent::FdbusDeviceControl dc;

  auto unsupported = dc.act("shell_exec", {}, 1000);
  if (unsupported.code != 4001) {
    std::cerr << "expected unsupported action code 4001, got=" << unsupported.code << "\n";
    return 1;
  }

  auto capture = dc.act("capture_photo", {}, 1000);
  if (capture.code != 5001 && capture.code != 5002) {
    std::cerr << "expected capture_photo code 5001/5002 without camera, got=" << capture.code
              << " msg=" << capture.message << "\n";
    return 1;
  }

  std::cout << "unit:test_fdbus_stub PASS\n";
  return 0;
}
