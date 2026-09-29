// test_agent_api_shutdown：C ABI 停机契约 + 契约面（v0.53.97）
//
// 修复前的行为（本测试为它而写）：
//   ① `thin_agent_stop` **先 join 事件线程**（投递通道先死），再让 worker **排空队列**
//      （`if (chat_queue.empty()) return; // 停机且排空` + `idx = 0; // 停机排空`）→
//      停机期间算出的结果全部丢弃（embedder 只看到断连），且 stop 被 1024 上限的队列
//      拖成分钟~小时级（UI 线程卡死）；
//   ② 排队请求**零通告**；
//   ③ `last_error` 返回内部 std::string 的 c_str()（锁已释放，他线程 set_error 即撕裂）；
//   ④ `set_log_callback` 与 `log()` 无同步（头文件却承诺"线程安全"）→ (cb,ud) 错配窗口。
//
// 契约（本测试锁定）：
//   T1 create/start 成功
//   T2 同一连接**连发 8 条同 chat 请求**（同 session 串行 → 1 在途 + ≥1 排队）
//   T3 stop **快速返回**（不排空队列；<20s）
//   T4 排队请求收到 **code 29001 停机通告**（而非静默丢弃）
//   T5 is_running == 0
//   T6 last_error(NULL) == "null agent handle"（契约）
//   T7 日志回调拿到**正确的 user_data**（快照配对）
//   T8 destroy 无崩
#include "test_macros.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "thin_agent/api/agent_api.h"

using namespace std::chrono;

static const int kPort = 19101;

/// 构造掩码文本帧（客户端→服务端必须掩码）
static std::vector<unsigned char> ws_frame(const std::string& payload) {
  std::vector<unsigned char> f;
  f.push_back(0x81);
  const size_t n = payload.size();
  if (n < 126) {
    f.push_back(static_cast<unsigned char>(0x80 | n));
  } else {
    f.push_back(0x80 | 126);
    f.push_back(static_cast<unsigned char>((n >> 8) & 0xff));
    f.push_back(static_cast<unsigned char>(n & 0xff));
  }
  const unsigned char mask[4] = {7, 8, 9, 10};
  f.insert(f.end(), mask, mask + 4);
  for (size_t i = 0; i < n; ++i) f.push_back(static_cast<unsigned char>(payload[i] ^ mask[i % 4]));
  return f;
}

static std::atomic<int> g_log_calls{0};
static std::atomic<bool> g_log_ud_ok{true};
static void on_log(int, const char*, void* ud) {
  g_log_calls.fetch_add(1);
  if (ud != reinterpret_cast<void*>(0x5A5A)) g_log_ud_ok.store(false);  // 快照必须配对
}

int main() {
  thin_agent_config_t cfg{};
  cfg.config_yaml = "config/demo.model.yaml";
  cfg.profile = "lmstudio_demo";
  cfg.ws_port = kPort;
  cfg.multicast_port = 0;
  cfg.discovery_enabled = 0;

  thin_agent_t* ag = thin_agent_create(&cfg);
  ASSERT_TRUE("T1 create", ag != nullptr);
#ifdef _WIN32
  _putenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE={\"choices\":[{\"message\":{\"content\":\"ok\"}}]}");
#else
  setenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE",
         "{\"choices\":[{\"message\":{\"content\":\"mock ok\"}}]}", 1);
  // T7: 先装回调再 start，start 内部的 log 必须回调且 user_data 配对
  thin_agent_set_log_callback(ag, on_log, reinterpret_cast<void*>(0x5A5A));
#endif
  ASSERT_TRUE("T1 start", thin_agent_start(ag) == 0);
  std::this_thread::sleep_for(milliseconds(300));
  ASSERT_TRUE("T7 日志回调触发且 user_data 配对", g_log_calls.load() > 0 && g_log_ud_ok.load());

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kPort);
  ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  ASSERT_TRUE("WS connect", ::connect(fd, (sockaddr*)&a, sizeof(a)) == 0);
  const char* up =
      "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
      "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  ::send(fd, up, static_cast<int>(strlen(up)), 0);
  char ub[2048];
  int un = static_cast<int>(::recv(fd, ub, sizeof(ub) - 1, 0));
  ASSERT_TRUE("upgrade 101", un > 0 && strstr(ub, "101") != nullptr);

  // T2: 同一连接连发 8 条（同 session → 串行 → 必然有排队项）
  int sent = 0;
  for (int i = 0; i < 8; ++i) {
    auto f = ws_frame("{\"type\":\"chat\",\"text\":\"hi\",\"cmd_id\":\"t" +
                      std::to_string(i) + "\"}");
    if (::send(fd, f.data(), static_cast<int>(f.size()), 0) > 0) ++sent;
  }
  ASSERT_TRUE("T2 连发 8 条已送出", sent == 8);

  // T3: 立刻停机——落点应"快速返回"，不因排队项被拖住
  auto t0 = steady_clock::now();
  std::atomic<int> stop_rc{-99};
  std::thread stopper([&] { stop_rc.store(thin_agent_stop(ag)); });
  stopper.join();
  const long stop_ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
  std::cout << "[DBG] stop rc=" << stop_rc.load() << " 耗时=" << stop_ms << "ms" << std::endl;
  ASSERT_TRUE("T3 stop 返回 0", stop_rc.load() == 0);
  ASSERT_TRUE("T3 stop 快速返回（不排空队列，<20s）", stop_ms < 20000);

  // T4: 读残余帧，找停机通告 code 29001
  int notices = 0, frames = 0;
  struct timeval tv{2, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  char buf[8192];
  for (int i = 0; i < 8; ++i) {
    int n = static_cast<int>(::recv(fd, buf, sizeof(buf) - 1, 0));
    if (n <= 0) break;
    buf[n] = 0;
    frames++;
    if (strstr(buf, "29001") != nullptr) notices++;
  }
  std::cout << "[DBG] 收到 " << frames << " 个读块，" << notices << " 个含 29001 通告" << std::endl;
  ASSERT_TRUE("T4 排队请求收到停机通告(29001)", notices > 0);
  ::close(fd);

  ASSERT_TRUE("T5 is_running == 0", thin_agent_is_running(ag) == 0);
  ASSERT_TRUE("T6 last_error(NULL) 契约",
              strcmp(thin_agent_last_error(nullptr), "null agent handle") == 0);
  thin_agent_destroy(ag);
  ASSERT_TRUE("T8 destroy 无崩", true);
  return 0;
}
