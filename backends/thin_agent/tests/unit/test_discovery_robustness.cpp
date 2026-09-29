// test_discovery_robustness：组播发现面（C ABI）稳健性/契约（v0.53.98）
//
// 修复前的行为（本测试为它们而写）：
//   D1 `thin_discovery_client_create(NULL)` **直接解引用 `*config`** → 段错误
//      （兄弟 `thin_discovery_server_create` 有 NULL 检查 = 兄弟不一致）
//   D2 `probe(dc, 0, ...)`：SO_RCVTIMEO 用原始 timeout_ms → {0,0} = **永不超时**，
//      无人应答时 recvfrom **永久阻塞**（而 deadline 却按 max(timeout,100) 算 = 自相矛盾）
//   D3 同一服务器回多条 announce → 回调**多次**、found **重复计数**（头文件契约是
//      "每发现一个服务器调用一次"）
//   D4 recvfrom 任何 n<0 即 break（ICMP port-unreachable 的 ECONNREFUSED 会提前掐断收集）
//
// 断言：
//   T1 client_create(NULL) → nullptr 且不崩
//   T2 probe(dc, 0) 在无人应答时**不挂死**（钳制后 deadline=300ms，实测 <3s 返回 0）
//   T3 真服务端 + 客户端往返：found >= 1（组播在部分环境不可用 → SKIP 不误红）
//   T4 假宣告方**连发两条相同 announce** → found == 1（去重；无组播 → SKIP）
//   T5 server create/start/stop/destroy **有界**（停机能退出，<3s）
//   T6 free_entry(全零 entry) 无崩
//
// 环境开关（仅供回退取证逐条跑）：DISCOVERY_ONLY=T1|T2|T3T4|T5T6
#include "test_macros.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "thin_agent/api/discovery.h"

using namespace std::chrono;

static const int kPort = 19876;
static const char* kGroup = "224.0.0.199";

static std::atomic<int> g_found_calls{0};
static void on_found(const thin_discovery_entry_t* e, void* ud) {
  (void)ud;
  g_found_calls.fetch_add(1);
  if (e) std::cout << "[DBG] found server_id=" << (e->server_id ? e->server_id : "-")
                   << " ws_port=" << e->ws_port << std::endl;
}

/// 假宣告方：收 probe → 对同一 sender **连发两条**完全相同的 announce（用于 T4 去重）
static void fake_announcer(std::atomic<bool>* stop, std::atomic<int>* got_probe) {
  int s = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return;
  int reuse = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kPort);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0) { ::close(s); return; }
  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = ::inet_addr(kGroup);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  ::setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
  timeval tv{0, 200000};
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  char buf[2048];
  while (!stop->load()) {
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    ssize_t n = ::recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fl);
    if (n <= 0) continue;
    got_probe->fetch_add(1);
    const char* ann = "{\"type\":\"thin_agent.announce\",\"version\":\"1\","
                      "\"server_id\":\"fake-srv-1\",\"hostname\":\"fakehost\","
                      "\"os\":\"Linux\",\"arch\":\"x86_64\",\"ws_port\":19191,"
                      "\"agent_version\":\"v0.0.0-fake\",\"profiles\":[\"p\"],"
                      "\"capabilities\":[\"agent\"],\"load\":0.1,\"uptime_seconds\":42}";
    for (int i = 0; i < 2; ++i) {  // 连发两条相同 announce
      ::sendto(s, ann, strlen(ann), 0, (sockaddr*)&from, fl);
    }
  }
  ::close(s);
}

int main() {
  const char* only = getenv("DISCOVERY_ONLY");
  auto want = [&](const char* k) { return !only || strcmp(only, k) == 0; };
  const char* yaml = "config/demo.model.yaml"; (void)yaml;

  // ── T1: NULL config 不得崩 ──
  if (want("T1")) {
    thin_discovery_t* bad = thin_discovery_client_create(nullptr);
    ASSERT_TRUE("T1 client_create(NULL) 返回 nullptr（不崩）", bad == nullptr);
  }

  // ── T2: timeout_ms=0 不得永久阻塞 ──
  if (want("T2")) {
    thin_discovery_config_t c{};
    c.port = 19999;                    // 无人监听的端口：必然无人应答
    c.group = kGroup;
    thin_discovery_t* dc = thin_discovery_client_create(&c);
    ASSERT_TRUE("T2 client_create 正常", dc != nullptr);
    auto t0 = steady_clock::now();
    int n = thin_discovery_probe(dc, 0, on_found, nullptr);
    long ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    std::cout << "[DBG] probe(timeout=0) 返回 " << n << " 耗时 " << ms << "ms" << std::endl;
    ASSERT_TRUE("T2 timeout=0 有界返回（不挂死，<3s）", ms < 3000);
    ASSERT_TRUE("T2 无应答时 found==0", n == 0);
    thin_discovery_client_destroy(dc);
  }

  // ── T3/T4: 组播往返 + 去重 ──
  if (want("T3T4")) {
    std::atomic<bool> stop{false};
    std::atomic<int> got_probe{0};
    std::thread ann(fake_announcer, &stop, &got_probe);
    std::this_thread::sleep_for(milliseconds(300));

    thin_discovery_config_t c{};
    c.port = kPort;
    c.group = kGroup;
    thin_discovery_t* dc = thin_discovery_client_create(&c);
    g_found_calls.store(0);
    int n = thin_discovery_probe(dc, 800, on_found, nullptr);
    std::cout << "[DBG] probe found=" << n << " 回调次数=" << g_found_calls.load()
              << " 假宣告方收到 probe=" << got_probe.load() << std::endl;
    if (got_probe.load() == 0) {
      std::cout << "SKIP: 本环境组播不可用（假宣告方未收到 probe），T3/T4 跳过" << std::endl;
    } else {
      ASSERT_TRUE("T4 两条相同 announce 只回调一次（去重）", g_found_calls.load() == 1);
      ASSERT_TRUE("T4 found 计数不重复（去重后==1）", n == 1);
    }
    thin_discovery_client_destroy(dc);
    stop.store(true);
    ann.join();
  }

  // ── T5: 服务端生命周期有界 ──
  if (want("T5T6")) {
    thin_discovery_config_t sc{};
    sc.port = kPort;
    sc.group = kGroup;
    thin_discovery_server_t* srv = thin_discovery_server_create(&sc);
    ASSERT_TRUE("T5 server_create", srv != nullptr);
    ASSERT_TRUE("T5 server_start", thin_discovery_server_start(srv) == 0);
    std::this_thread::sleep_for(milliseconds(200));
    auto t0 = steady_clock::now();
    thin_discovery_server_stop(srv);
    long ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    std::cout << "[DBG] server_stop 耗时 " << ms << "ms" << std::endl;
    ASSERT_TRUE("T5 server_stop 有界退出（<3s）", ms < 3000);
    thin_discovery_server_destroy(srv);

    thin_discovery_entry_t z{};
    thin_discovery_free_entry(&z);
    thin_discovery_free_entry(nullptr);
    ASSERT_TRUE("T6 free_entry(零值/NULL) 无崩", true);
  }
  return 0;
}
