// unit_cron_unattended_hitl：v0.52.1 cron 无人值守 HITL 语义回归
//
// 修复的问题：定时任务触发危险工具时无人在场，原行为=FC 卡在暂停态
// 干等 5 分钟被 TTL 清扫，任务静默烂掉。
//
// 新语义：cron 线程（g_cron_filter 非空）审批回调自动拒绝 +
// g_fc_dangerous_denied 置位 → FC 从本轮 tool_calls 剔除该工具 →
// 以"被自动拒绝"错误结果（带 tool_call_id 配对）喂回模型 → 模型
// 感知后换安全路径收尾。
//
// 测试：mock LLM server（SSE）驱动真实 FC 路径。
// 1) 交互会话：危险工具 → 暂停（不变式回归）
// 2) cron 上下文：危险工具被拒 → 模型收到 DENIED → 收尾文本返回

#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <filesystem>
#include <thread>

#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/AgentServiceUtil.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"
#include "thin_agent/llm/CloudLlmClient.h"

#include "test_macros.h"

using namespace thin_agent;

// v0.53.3: 拆分后 g_cron_filter 移入 svc_util（AgentServiceUtil.cpp）
using thin_agent::svc_util::g_cron_filter;

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
    // accept 用非阻塞+轮询（析构置 stop_ 后能退出，防挂起）
    int flags = fcntl(fd_, F_SETFL, O_NONBLOCK);
    (void)flags;
    while (!stop_) {
      int c = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK);
      if (c < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      // 读掉请求头+体（按 Content-Length 收齐，防分包截断）
      std::string req;
      char buf[4096];
      size_t cl = 0;
      while (true) {
        ssize_t n = recv(c, buf, sizeof(buf), 0);
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
      // 按路径分发：/embeddings 是记忆召回（回固定 JSON，不耗脚本）
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

std::string sse_rm_then_text(size_t round) {
  if (round == 0) {
    return "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
           "\"id\":\"call_d1\",\"type\":\"function\",\"function\":{"
           "\"name\":\"shell_exec\",\"arguments\":"
           "\"{\\\"command\\\":\\\"rm -rf /tmp/cron_unattended_probe\\\"}\"}}]}}]}\n\n"
           "data: {\"choices\":[{\"finish_reason\":\"tool_calls\"}]}\n\n"
           "data: [DONE]\n\n";
  }
  return "data: {\"choices\":[{\"delta\":{\"content\":"
         "\"收尾：危险操作已被系统自动拒绝，改用说明性输出。CRON_UNATTENDED_OK\"},"
         "\"finish_reason\":\"stop\"}]}\n\n"
         "data: [DONE]\n\n";
}

nlohmann::json drive_chat(AgentService& svc, const std::string& text,
                          const std::string& cid) {
  auto r = svc.handle_request(cid,
                              nlohmann::json{{"type", "chat"},
                                             {"text", text},
                                             {"chat_id", cid}},
                              nullptr, nullptr);
  return r;
}

}  // namespace

int main() {
  setenv("CRON_HITL_TEST_KEY", "test-key-0123456789abcdef", 1);
  setenv("THIN_AGENT_HOME", "/tmp/cron_hitl_test_home", 1);
  std::filesystem::remove_all("/tmp/cron_hitl_test_home");

  // 场景 A：交互会话（g_cron_filter 空）→ 暂停不变式
  {
    MockLlm mock({sse_rm_then_text(0), sse_rm_then_text(1)});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    DemoConfigCompat cfg;
    cfg.provider = "openai-compatible";
    cfg.model_name = "mock";
    cfg.api_base = "http://127.0.0.1:" + std::to_string(mock.port()) + "/v1";
    cfg.api_key_env = "CRON_HITL_TEST_KEY";
    cfg.fallback = "cloud";
    cfg.mode = "cloud";
    auto dc = std::make_shared<FakeDeviceControl>();
    auto executor = std::make_shared<ActionExecutor>(dc);
    auto task_engine = std::make_shared<TaskEngine>(executor);
    AgentService svc(cfg, executor, task_engine);

    auto j = drive_chat(svc, "删除 /tmp/cron_unattended_probe", "cu_interactive");
    std::string text = j.value("text", "");
    ASSERT_TRUE("交互会话危险工具仍暂停",
                text.find("需要确认") != std::string::npos);
  }

  // 场景 B：cron 上下文（g_cron_filter 置位）→ 自动拒绝+模型收尾
  {
    MockLlm mock({sse_rm_then_text(0), sse_rm_then_text(1)});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    DemoConfigCompat cfg;
    cfg.provider = "openai-compatible";
    cfg.model_name = "mock";
    cfg.api_base = "http://127.0.0.1:" + std::to_string(mock.port()) + "/v1";
    cfg.api_key_env = "CRON_HITL_TEST_KEY";
    cfg.fallback = "cloud";
    cfg.mode = "cloud";
    auto dc = std::make_shared<FakeDeviceControl>();
    auto executor = std::make_shared<ActionExecutor>(dc);
    auto task_engine = std::make_shared<TaskEngine>(executor);
    AgentService svc(cfg, executor, task_engine);

    g_cron_filter = nlohmann::json::object();  // 模拟 cron 线程标记
    auto j = drive_chat(svc, "删除 /tmp/cron_unattended_probe", "cu_cron");
    g_cron_filter = nullptr;  // 还原

    std::string text = j.value("text", "");
    ASSERT_TRUE("cron 危险工具不暂停（无 needs_approval 卡死）",
                text.find("需要确认") == std::string::npos);
    ASSERT_TRUE("模型收到拒绝后收尾（CRON_UNATTENDED_OK）",
                text.find("CRON_UNATTENDED_OK") != std::string::npos);
    // 探针文件未被删（工具确实没执行）
    ASSERT_TRUE("危险工具未执行",
                !std::filesystem::exists("/tmp/cron_unattended_probe_deleted"));
  }

  return TEST_REPORT();
}
