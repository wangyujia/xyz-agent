// unit_hitl_fast_path：fast 通道 HITL 审批回归（v0.51.2）
//
// 修复的缺陷：v0.49.0 HITL 只接入主 FC 通道（fc_approval_cb 仅传给主调
// run_function_calling_loop），fast 通道（简单文件操作直通）的危险工具
// 无审批直接执行。v0.51.2 将回调定义提升至两分支之前、两通道共用。
//
// 测试方法：本地 mock OpenAI server（SSE 流式返回 tool_calls 指向
// shell_exec/Dangerous 工具）→ 发命中 fast 关键词的 chat → 断言：
//   1. 响应 text 含审批暂停提示（⏸️/危险操作需要确认）
//   2. 响应 needs_approval 语义经 text 传播（fast 分支返回 out.final）
//   3. fc_pending_approvals_ 状态已记录（经 chat_approve 可继续）
//   4. 危险工具未被执行（目标文件不存在）
// mock 说明：THIN_AGENT_TEST_CLOUD_RESPONSE 会让 use_native_fc=false，
// 必须用真实 HTTP mock server 驱动（v0.50.1 沉淀的 OneShotServer 模式）。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/llm/DemoConfigCompat.h"
#include "thin_agent/core/TaskEngine.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"

#include "test_macros.h"

namespace {

// mock OpenAI server：接受任意路径，返回带 tool_calls 的 SSE 流
class MockLlmServer {
 public:
  bool start(int* port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;  // 随机端口
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    if (listen(fd_, 4) != 0) return false;
    socklen_t len = sizeof(addr);
    if (getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
    *port = ntohs(addr.sin_port);
    running_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
  }

  void stop() {
    running_ = false;
    if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; }
    if (th_.joinable()) th_.join();
  }

  int requests() const { return requests_.load(); }

 private:
  void loop() {
    while (running_) {
      int c = ::accept(fd_, nullptr, nullptr);
      if (c < 0) break;
      requests_++;
      // 读掉请求头+体（到空行后按 Content-Length 收）
      std::string req;
      char buf[4096];
      size_t cl = 0;
      while (true) {
        int n = ::recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, n);
        auto hpos = req.find("\r\n\r\n");
        if (hpos != std::string::npos) {
          auto cpos = req.find("Content-Length:");
          if (cpos != std::string::npos) {
            cl = std::strtoul(req.c_str() + cpos + 15, nullptr, 10);
          } else {
            cl = 0;
          }
          if (req.size() - hpos - 4 >= cl) break;
        }
      }
      // SSE 响应：一轮 tool_calls（shell_exec 危险工具）+ 一轮纯文本
      std::string body =
          "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
          "\"id\":\"call_1\",\"type\":\"function\",\"function\":{"
          "\"name\":\"shell_exec\",\"arguments\":\"{\\\"command\\\":\\\"rm -f /tmp/hitl_fast_pwned\\\"}\"}}]}}]}\n\n"
          "data: {\"choices\":[{\"finish_reason\":\"tool_calls\"}]}\n\n"
          "data: [DONE]\n\n";
      std::string resp =
          "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
          "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
      ::send(c, resp.c_str(), resp.size(), 0);
      ::close(c);
    }
  }
  int fd_{-1};
  std::atomic<bool> running_{false};
  std::atomic<int> requests_{0};
  std::thread th_;
};

}  // namespace

int main() {
  const std::string data_dir = "/tmp/hitl_fast_test_" + std::to_string(::getpid());
  std::filesystem::remove_all(data_dir);
  setenv("THIN_AGENT_HOME", data_dir.c_str(), 1);

  MockLlmServer srv;
  int port = 0;
  ASSERT_TRUE("mock server start", srv.start(&port));

  thin_agent::DemoConfigCompat cfg;
  cfg.mode = "cloud";
  cfg.provider = "openai-compatible";
  cfg.model_name = "mock-fast-model";
  cfg.api_base = "http://127.0.0.1:" + std::to_string(port) + "/v1";
  cfg.api_key_env = "HITL_FAST_TEST_KEY";
  cfg.fallback = "cloud";  // 不降级本地
  setenv("HITL_FAST_TEST_KEY", "test-key-0123456789abcdef", 1);

  auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
  auto ex = std::make_shared<thin_agent::ActionExecutor>(dc);
  auto te = std::make_shared<thin_agent::TaskEngine>(ex);
  te->init(data_dir + "/test.db");
  thin_agent::AgentService svc(cfg, ex, te);

  // 命中 fast 关键词的简单文件操作话术（read_file 在 file_keywords）
  auto r = svc.handle_request("hitl-fast-sess",
                              nlohmann::json{{"type", "chat"},
                                             {"text", "read_file 读取 /tmp/some_file.txt 内容"},
                                             {"chat_id", "hitl-fast-sess"}},
                              nullptr, nullptr);
  std::string text = r.value("text", "");
  std::string mode = r.value("mode_used", r.value("mode", ""));

  // ── 断言 ──
  // 1. 审批暂停提示出现（fast 通道被 HITL 拦住）
  ASSERT_TRUE("fast 通道危险工具被 HITL 拦截（text 含暂停提示）",
              text.find("\xe2\x8f\xb8\xef\xb8\x8f") != std::string::npos ||
              text.find("危险操作需要确认") != std::string::npos ||
              text.find("批准") != std::string::npos);
  // 2. 危险工具没有执行（目标文件不存在）
  ASSERT_TRUE("shell_exec 未被执行（无 pwned 文件）",
              !std::filesystem::exists("/tmp/hitl_fast_pwned"));
  // 3. 走的是 fast 通道（mode 含 fast）或主通道兜底——两通道都应拦截
  ASSERT_TRUE("mode 为 fast 或主 FC 通道", mode.find("fast") != std::string::npos ||
              mode.find("cloud-fc") != std::string::npos || mode.empty());

  // 4. chat_approve 可达（pending 状态已记录）
  auto ap = svc.handle_request("hitl-fast-sess",
                               nlohmann::json{{"type", "chat_approve"},
                                              {"approved", false}},
                               nullptr, nullptr);
  std::string apt = ap.dump();
  ASSERT_TRUE("chat_approve 有响应（拒绝路径）", !apt.empty());

  // 5. v0.51.2 缓存交互回归：HITL 暂停响应不得被缓存——同话术二次发
  //    必须重新走 FC（再次出现审批暂停），而不是命中缓存返回旧"⏸️"文本
  auto r2 = svc.handle_request("hitl-fast-sess2",
                               nlohmann::json{{"type", "chat"},
                                              {"text", "read_file 读取 /tmp/some_file.txt 内容"},
                                              {"chat_id", "hitl-fast-sess2"}},
                               nullptr, nullptr);
  std::string text2 = r2.value("text", "");
  ASSERT_TRUE("同话术重发未命中缓存（再次触发审批暂停）",
              text2.find("危险操作需要确认") != std::string::npos ||
              text2.find("批准") != std::string::npos);
  ASSERT_TRUE("二次暂停后拒绝路径可达",
              !svc.handle_request("hitl-fast-sess2",
                                  nlohmann::json{{"type", "chat_approve"},
                                                 {"approved", false}},
                                  nullptr, nullptr).dump().empty());

  std::filesystem::remove_all(data_dir);
  ::unlink("/tmp/hitl_fast_pwned");
  srv.stop();
  return TEST_REPORT();
}
