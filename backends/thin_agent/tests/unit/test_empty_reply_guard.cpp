// unit_empty_reply_guard：v0.51.4 空回复守卫回归
//
// 修复的缺陷：上游 4xx 错误体（坏 key 的 401 JSON）被 parse_response
// 解析为 ok=true + text="" 时，主 FC 分支无空文本守卫直接当成功返回
// ——mode=cloud-fc、http_status=0、text 空，fallback=offline 永不触发。
// fast 分支早有 !text.empty() 守卫，主分支漏了（同类不一致）。
//
// 测试：mock LLM server 返回 401 错误体（模拟 GLM 鉴权失败响应）→
// 断言 chat 走 offline fallback（而非空文本伪成功）。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"
#include "thin_agent/llm/DemoConfigCompat.h"
#include "thin_agent/core/TaskEngine.h"

#include "test_macros.h"

namespace {

class MockErrLlm {
 public:
  bool start(int* port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = 0;
    if (bind(fd_, (sockaddr*)&a, sizeof(a)) || listen(fd_, 4)) return false;
    socklen_t l = sizeof(a);
    getsockname(fd_, (sockaddr*)&a, &l);
    *port = ntohs(a.sin_port);
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
  }
  void stop() {
    run_ = false;
    if (fd_ >= 0) { ::shutdown(fd_, 2); ::close(fd_); fd_ = -1; }
    if (th_.joinable()) th_.join();
  }

 private:
  void loop() {
    while (run_) {
      int c = ::accept(fd_, nullptr, nullptr);
      if (c < 0) break;
      std::string req;
      char buf[8192];
      while (true) {
        int n = ::recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, n);
        auto h = req.find("\r\n\r\n");
        if (h != std::string::npos) {
          size_t cl = 0;
          auto cp = req.find("Content-Length:");
          if (cp != std::string::npos) cl = strtoul(req.c_str() + cp + 15, nullptr, 10);
          if (req.size() - h - 4 >= cl) break;
        }
      }
      // 401 错误体（GLM 风格）——无 choices 字段
      std::string body = "{\"error\":{\"code\":\"1002\",\"message\":\"invalid api key\"}}";
      std::string resp = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n"
                         "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
      ::send(c, resp.c_str(), resp.size(), 0);
      ::close(c);
    }
  }
  int fd_{-1};
  std::atomic<bool> run_{false};
  std::thread th_;
};

}  // namespace

int main() {
  const std::string data_dir = "/tmp/empty_guard_test_" + std::to_string(::getpid());
  std::filesystem::remove_all(data_dir);
  std::filesystem::create_directories(data_dir);
  setenv("THIN_AGENT_HOME", data_dir.c_str(), 1);

  MockErrLlm srv;
  int port = 0;
  ASSERT_TRUE("mock 401 server 启动", srv.start(&port));

  thin_agent::DemoConfigCompat cfg;
  cfg.mode = "cloud";
  cfg.provider = "openai-compatible";
  cfg.model_name = "mock-err";
  cfg.api_base = "http://127.0.0.1:" + std::to_string(port) + "/v1";
  cfg.api_key_env = "EMPTY_GUARD_KEY";
  cfg.fallback = "offline";  // 与生产配置一致
  setenv("EMPTY_GUARD_KEY", "sk-bad-key-unit-test", 1);

  auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
  auto ex = std::make_shared<thin_agent::ActionExecutor>(dc);
  auto te = std::make_shared<thin_agent::TaskEngine>(ex);
  te->init(data_dir + "/t.db");
  thin_agent::AgentService svc(cfg, ex, te);

  auto r = svc.handle_request("empty-guard-sess",
                              nlohmann::json{{"type", "chat"},
                                             {"text", "你好"},
                                             {"chat_id", "empty-guard-sess"}},
                              nullptr, nullptr);
  std::string text = r.value("text", "");
  std::string mode = r.value("mode_used", r.value("mode", ""));
  std::string reason = r.value("decision", nlohmann::json::object()).value("reason", "");

  // 断言：不再返回空文本伪成功；走了 offline fallback（或至少给出错误说明）
  ASSERT_TRUE("空回复不再伪装成功（text 非空）", !text.empty());
  ASSERT_TRUE("mode 不再是纯 cloud-fc 空响应",
              !(mode == "cloud-fc" && text.empty()));
  bool fell_back = mode.find("offline") != std::string::npos ||
                   mode.find("local") != std::string::npos ||
                   reason.find("offline") != std::string::npos ||
                   reason.find("fallback") != std::string::npos ||
                   text.find("离线") != std::string::npos ||
                   text.find("offline") != std::string::npos;
  ASSERT_TRUE("offline fallback 触发", fell_back);

  srv.stop();
  std::filesystem::remove_all(data_dir);
  return TEST_REPORT();
}
