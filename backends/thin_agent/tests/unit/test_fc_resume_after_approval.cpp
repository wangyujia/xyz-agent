// unit_fc_resume_after_approval：v0.52.2 方案 C 回归
//
// 新语义：批准危险工具 → 执行 → 带工具结果续跑 FC 循环 → 模型收尾。
// 旧语义（只执行不续跑，用户须再发"继续"）作为无快照时的回退保留。
//
// 测试链路（mock LLM server，SSE）：
//   chat(write_file 危险) → iter0 tool_call → 暂停（快照入 pa）
//   approve → 执行工具 → 续跑 iter1 → 最终文本含 RESUME_DONE
// 断言：
//   1) 暂停响应正常（需要确认）
//   2) 批准后单次响应即含工具结果 AND 续跑收尾文本（不再需要"继续"驱动）
//   3) 拒绝路径不变（已取消）

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"

#include "test_macros.h"

using namespace thin_agent;

namespace {

class MockLlm {
 public:
  explicit MockLlm(std::vector<std::string> sse_scripts)
      : scripts_(std::move(sse_scripts)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;
    bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(fd_, 4);
    socklen_t len = sizeof(addr);
    getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    srv_ = std::thread([this] { serve(); });
  }
  ~MockLlm() {
    stop_ = true;
    close(fd_);
    if (srv_.joinable()) srv_.join();
  }
  int port() const { return port_; }

 private:
  void serve() {
    // 非阻塞 accept（析构可退出）
    int flags = fcntl(fd_, F_SETFL, O_NONBLOCK);
    (void)flags;
    while (!stop_) {
      int c = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK);
      if (c < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      // 按路径分流：/embeddings 是记忆召回（固定 JSON，不耗脚本）
      std::string req;
      char buf[16384];
      size_t cl = 0;
      while (true) {
        ssize_t n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, n);
        auto hpos = req.find("\r\n\r\n");
        if (hpos != std::string::npos) {
          auto cpos = req.find("Content-Length:");
          cl = cpos != std::string::npos
                   ? std::strtoul(req.c_str() + cpos + 15, nullptr, 10) : 0;
          if (req.size() - hpos - 4 >= cl) break;
        }
      }
      std::string body;
      if (req.rfind("POST /v1/embeddings") == 0) {
        body = "{\"data\":[{\"embedding\":[0.1,0.2,0.3],\"index\":0}]}";
      } else {
        body = idx_ < scripts_.size() ? scripts_[idx_++] : scripts_.back();
      }
      std::string http = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                         "Connection: close\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\n\r\n" + body;
      ssize_t off = 0;
      while (off < (ssize_t)http.size()) {
        ssize_t n = send(c, http.data() + off, http.size() - off, MSG_NOSIGNAL);
        if (n <= 0) break;
        off += n;
      }
      close(c);
    }
  }
  int fd_ = -1;
  int port_ = 0;
  std::vector<std::string> scripts_;
  size_t idx_ = 0;
  std::thread srv_;
  std::atomic<bool> stop_{false};
};

std::string sse_write_then_done(size_t round) {
  if (round == 0) {
    return "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
           "\"id\":\"call_r1\",\"type\":\"function\",\"function\":{"
           "\"name\":\"write_file\",\"arguments\":"
           "\"{\\\"path\\\":\\\"/tmp/fc_resume_probe.txt\\\","
           "\\\"content\\\":\\\"resume-ok\\\"}\"}}]}}]}\n\n"
           "data: {\"choices\":[{\"finish_reason\":\"tool_calls\"}]}\n\n"
           "data: [DONE]\n\n";
  }
  return "data: {\"choices\":[{\"delta\":{\"content\":"
         "\"任务收尾：文件已写入，全部完成。RESUME_DONE\"},"
         "\"finish_reason\":\"stop\"}]}\n\n"
         "data: [DONE]\n\n";
}

nlohmann::json drive(AgentService& svc, const nlohmann::json& req,
                     const std::string& sid) {
  return svc.handle_request(sid, req, nullptr, nullptr);
}

}  // namespace

int main() {
  setenv("FC_RESUME_TEST_KEY", "test-key-0123456789abcdef", 1);
  setenv("THIN_AGENT_HOME", "/tmp/fc_resume_test_home", 1);
  std::filesystem::remove_all("/tmp/fc_resume_test_home");
  std::filesystem::remove("/tmp/fc_resume_probe.txt");

  MockLlm mock({sse_write_then_done(0), sse_write_then_done(1)});
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  DemoConfigCompat cfg;
  cfg.mode = "cloud";
  cfg.provider = "openai-compatible";
  cfg.model_name = "mock-resume";
  cfg.api_base = "http://127.0.0.1:" + std::to_string(mock.port()) + "/v1";
  cfg.api_key_env = "FC_RESUME_TEST_KEY";
  cfg.fallback = "cloud";
  cfg.request_timeout_ms = 5000;

  auto dc = std::make_shared<FakeDeviceControl>();
  auto ex = std::make_shared<ActionExecutor>(dc);
  auto te = std::make_shared<TaskEngine>(ex);
  AgentService svc(cfg, ex, te);

  // 1) 危险工具 → 暂停
  auto j1 = drive(svc, nlohmann::json{{"type", "chat"},
                                      {"text", "创建 /tmp/fc_resume_probe.txt 写入 resume-ok"},
                                      {"chat_id", "fr-1"}}, "fr-1");
  ASSERT_TRUE("危险工具暂停",
              j1.value("text", "").find("需要确认") != std::string::npos);

  // 2) 批准 → 执行 + 续跑收尾（一次响应包含两段）
  auto j2 = drive(svc, nlohmann::json{{"type", "chat_approve"},
                                      {"approved", true},
                                      {"chat_id", "fr-1"}}, "fr-1");
  std::string t2 = j2.value("text", "");
  ASSERT_TRUE("批准后含工具执行结果", t2.find("已执行") != std::string::npos);
  ASSERT_TRUE("批准后续跑收尾（RESUME_DONE）——不再需要继续驱动",
              t2.find("RESUME_DONE") != std::string::npos);
  ASSERT_TRUE("文件真实落盘",
              std::filesystem::exists("/tmp/fc_resume_probe.txt"));

  // 3) 拒绝路径不变式
  auto j3 = drive(svc, nlohmann::json{{"type", "chat"},
                                      {"text", "再创建一次 /tmp/fc_resume_probe2.txt"},
                                      {"chat_id", "fr-2"}}, "fr-2");
  if (j3.value("text", "").find("需要确认") != std::string::npos) {
    auto j4 = drive(svc, nlohmann::json{{"type", "chat_approve"},
                                        {"approved", false},
                                        {"chat_id", "fr-2"}}, "fr-2");
    ASSERT_TRUE("拒绝后已取消", j4.value("text", "").find("已取消") != std::string::npos);
    ASSERT_TRUE("拒绝后未落盘", !std::filesystem::exists("/tmp/fc_resume_probe2.txt"));
  }

  std::filesystem::remove_all("/tmp/fc_resume_test_home");
  std::filesystem::remove("/tmp/fc_resume_probe.txt");
  return TEST_REPORT();
}
