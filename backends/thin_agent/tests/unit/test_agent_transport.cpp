// unit_agent_transport：MCP 传输层覆盖 — HttpTransport（回环服务器 POST 回显）
// + StdioTransport（fork cat 子进程回显、JSON-RPC 往返、断连清理）。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "thin_agent/agent/HttpTransport.h"
#include "thin_agent/agent/StdioTransport.h"

namespace {

int g_failures = 0;

int expect(bool ok, const std::string& msg) {
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", msg.c_str());
  }
  return g_failures == 1 ? 1 : 0;
}

struct EchoServer {
  int port = 0;
  int srv_fd = -1;
  std::string last_body;

  bool start() {
    srv_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(srv_fd, (sockaddr*)&addr, sizeof(addr)) != 0) return false;
    if (::listen(srv_fd, 1) != 0) return false;
    socklen_t len = sizeof(addr);
    if (::getsockname(srv_fd, (sockaddr*)&addr, &len) != 0) return false;
    port = ntohs(addr.sin_port);
    return true;
  }

  /// 收一个请求，把 body 原样回显为响应体。
  void serve_once() {
    sockaddr_in cli{};
    socklen_t clen = sizeof(cli);
    int c = ::accept(srv_fd, (sockaddr*)&cli, &clen);
    if (c < 0) return;
    timeval tv{10, 0};
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string raw;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(c, buf, sizeof(buf), 0)) > 0) {
      raw.append(buf, n);
      size_t pos = raw.find("\r\n\r\n");
      if (pos == std::string::npos) continue;
      std::string lower;
      lower.reserve(raw.size());
      for (char ch : raw) lower += (char)::tolower((unsigned char)ch);
      size_t cl = 0;
      size_t hpos = lower.find("content-length:");
      if (hpos != std::string::npos) {
        cl = (size_t)strtoul(raw.c_str() + hpos + 15, nullptr, 10);
      }
      if (raw.size() - pos - 4 >= cl) break;
    }
    size_t pos = raw.find("\r\n\r\n");
    last_body = pos == std::string::npos ? "" : raw.substr(pos + 4);
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: " +
                       std::to_string(last_body.size()) +
                       "\r\nConnection: close\r\n\r\n" + last_body;
    ::send(c, resp.data(), resp.size(), 0);
    ::close(c);
  }

  void stop() {
    if (srv_fd >= 0) ::close(srv_fd);
    srv_fd = -1;
  }
};

}  // namespace

int main() {
  using thin_agent::agent::HttpTransport;
  using thin_agent::agent::StdioTransport;

  // ── HttpTransport ──
  {
    EchoServer srv;
    expect(srv.start(), "echo server start");

    HttpTransport tr;
    expect(!tr.send("x").empty() == false || true, "send before connect no-crash");
    expect(tr.connect("http://127.0.0.1:" + std::to_string(srv.port) + "/mcp"),
           "connect ok");

    std::thread t([&] { srv.serve_once(); });
    std::string rpc = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    std::string resp = tr.send(rpc);
    t.join();
    expect(resp == rpc, "http echo roundtrip (resp=" + resp.substr(0, 40) + ")");
    expect(srv.last_body == rpc, "server received exact body");
    srv.stop();
    tr.disconnect();
  }

  // ── HttpTransport 错误路径：不可达端口 → JSON-RPC error 响应 ──
  {
    HttpTransport tr;
    tr.connect("http://127.0.0.1:9/nope");
    std::string resp = tr.send("{}");
    expect(resp.find("\"error\"") != std::string::npos,
           "unreachable endpoint yields jsonrpc error (resp=" + resp.substr(0, 50) + ")");
    tr.disconnect();
  }

  // ── HttpTransport 未 connect 就 send → not connected 错误 ──
  {
    HttpTransport tr;
    std::string resp = tr.send("{}");
    expect(resp.find("not connected") != std::string::npos, "send before connect error");
  }

  // ── StdioTransport：cat 回显 ──
  {
    StdioTransport tr;
    expect(tr.connect("/bin/cat"), "stdio connect /bin/cat");
    std::string rpc = R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})";
    std::string resp = tr.send(rpc);
    expect(resp == rpc, "stdio echo roundtrip (resp=" + resp.substr(0, 40) + ")");
    tr.disconnect();
    // disconnect 后子进程应被回收（不悬挂僵尸）
    int status = 0;
    pid_t w = ::waitpid(-1, &status, WNOHANG);
    expect(w == 0 || w == -1, "no zombie child after disconnect");
  }

  // ── StdioTransport：不存在程序 ──
  {
    StdioTransport tr;
    expect(!tr.connect("/nonexistent/mcp-server"), "stdio connect missing binary fails");
    tr.disconnect();
  }

  if (g_failures == 0) {
    std::printf("unit:test_agent_transport PASS\n");
    return 0;
  }
  std::printf("unit:test_agent_transport FAIL (%d)\n", g_failures);
  return 1;
}
