// unit_webhook_client：WebhookClient 覆盖 — 配置加载/列表脱敏/HMAC 签名投递/
// 直发 URL/错误路径。HTTP 投递用本地回环一次性服务器验证。
//
// 覆盖点：
//   1. load_json：合法配置/非数组拒绝/缺字段
//   2. list：输出不含 secret（脱敏）
//   3. send_direct → 本地服务器：收到 body + X-Signature 头（HMAC-SHA256 可复算）
//   4. send 未注册名 → not_found
//   5. send_direct 无 secret → 无签名头
//   6. test() 元信息

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "thin_agent/core/WebhookClient.h"

namespace {

int g_failures = 0;

int expect(bool ok, const std::string& msg) {
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", msg.c_str());
  }
  return g_failures == 1 ? 1 : 0;
}

/// 一次性本地 HTTP 服务器：收 1 个请求，返回最后收到的头+body。
struct OneShotServer {
  int port = 0;
  std::string last_headers;
  std::string last_body;
  int srv_fd = -1;

  bool start() {
    srv_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // 随机端口
    if (::bind(srv_fd, (sockaddr*)&addr, sizeof(addr)) != 0) return false;
    if (::listen(srv_fd, 1) != 0) return false;
    socklen_t len = sizeof(addr);
    if (::getsockname(srv_fd, (sockaddr*)&addr, &len) != 0) return false;
    port = ntohs(addr.sin_port);
    return true;
  }

  /// 阻塞收一个请求（带超时 10s）。
  bool accept_once() {
    sockaddr_in cli{};
    socklen_t clen = sizeof(cli);
    int c = ::accept(srv_fd, (sockaddr*)&cli, &clen);
    if (c < 0) return false;
    timeval tv{10, 0};
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string raw;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(c, buf, sizeof(buf), 0)) > 0) {
      raw.append(buf, n);
      size_t pos = raw.find("\r\n\r\n");
      if (pos == std::string::npos) continue;
      // 读 Content-Length
      size_t cl = 0;
      std::string lower;
      lower.reserve(raw.size());
      for (char ch : raw) lower += (char)::tolower((unsigned char)ch);
      size_t hpos = lower.find("content-length:");
      if (hpos != std::string::npos) {
        cl = (size_t)strtoul(raw.c_str() + hpos + 15, nullptr, 10);
      }
      if (raw.size() - pos - 4 >= cl) break;
    }
    // 发送最小 HTTP 响应（curl 需要完整响应才算成功）
    static const char kResp[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    ::send(c, kResp, sizeof(kResp) - 1, 0);
    ::close(c);
    size_t pos = raw.find("\r\n\r\n");
    if (pos == std::string::npos) return false;
    last_headers = raw.substr(0, pos);
    last_body = raw.substr(pos + 4);
    return true;
  }

  void stop() {
    if (srv_fd >= 0) ::close(srv_fd);
    srv_fd = -1;
  }
};

}  // namespace

int main() {
  using thin_agent::WebhookClient;

  // ── 1. load_json ──
  WebhookClient wc;
  nlohmann::json cfg = nlohmann::json::array({
      {{"name", "hook1"}, {"url", "http://127.0.0.1:1/hook"}, {"secret", "s3cret"}},
      {{"name", "plain"}, {"url", "http://127.0.0.1:1/plain"}},
  });
  expect(wc.load_json(cfg), "load_json valid");

  // ── 2. list 脱敏 ──
  nlohmann::json lst = wc.list();
  expect(lst.is_array() && lst.size() == 2, "list has 2 targets");
  expect(lst.dump().find("s3cret") == std::string::npos, "list hides secret");

  // ── 4. send 未注册名（不触网：find 失败先返回）──
  nlohmann::json nf = wc.send("no_such_hook", "{}");
  expect(!nf.value("success", true), "send unknown name fails");
  expect(nf.value("error", "") == "webhook_not_found" ||
             nf.contains("error"),
         "send unknown name error present");

  // ── 3. send_direct + 本地服务器：签名投递 ──
  {
    OneShotServer srv;
    if (!srv.start()) {
      expect(false, "oneshot server start");
    } else {
      std::string body = R"({"event":"test","v":1})";
      std::thread t([&] { srv.accept_once(); });
      nlohmann::json r = wc.send_direct(
          "http://127.0.0.1:" + std::to_string(srv.port) + "/cb", "hmackey", body);
      t.join();
      expect(r.value("success", false), "send_direct success");
      expect(r.value("status_code", 0) == 200, "send_direct status 200");
      expect(srv.last_body == body, "server received exact body");
      expect(srv.last_headers.find("X-Signature:") != std::string::npos,
             "signature header present");
      // HMAC 可复算性由 WebhookClient 内部 hmac_sha256_hex 保证，
      // 这里至少验证签名是 64 位 hex
      size_t spos = srv.last_headers.find("X-Signature: ") + 13;
      std::string sig = srv.last_headers.substr(spos, srv.last_headers.find("\r\n", spos) - spos);
      expect(sig.size() == 64, "signature is 64-hex chars (got " + std::to_string(sig.size()) + ")");
      // v0.53.67: HMAC 正确性对拍(python hmac 标准实现预计算)——
      /// 此前只查 64-hex 形状,签名值错也过(接收方会全部拒签)
      expect(sig == "184d3ed32ac2037b43e6fedba87cac55b2f6c8e5e24f0faacf481698eff3930d",
             "HMAC-SHA256 matches reference (got " + sig + ")");
      srv.stop();
    }
  }

  // ── 5. 无 secret → 无签名头 ──
  {
    OneShotServer srv;
    if (!srv.start()) {
      expect(false, "oneshot server start 2");
    } else {
      std::thread t([&] { srv.accept_once(); });
      nlohmann::json r = wc.send_direct(
          "http://127.0.0.1:" + std::to_string(srv.port) + "/cb", "", "{}");
      t.join();
      expect(r.value("success", false), "send_direct no-secret success");
      expect(srv.last_headers.find("X-Signature:") == std::string::npos,
             "no signature header when secret empty");
      srv.stop();
    }
  }

  // ── 6. test() 元信息 ──
  {
    nlohmann::json tr = wc.test("hook1", "{}");  // url 端口 1 不可达 → success=false 但有 meta
    expect(tr.contains("test_info") || tr.contains("target") || tr.contains("success"),
           "test() has meta/success");
  }

  // 非数组拒绝
  {
    WebhookClient w2;
    expect(!w2.load_json(nlohmann::json::object({{"a", 1}})), "object rejected");
  }

  if (g_failures == 0) {
    std::printf("unit:test_webhook_client PASS\n");
    return 0;
  }
  std::printf("unit:test_webhook_client FAIL (%d)\n", g_failures);
  return 1;
}
