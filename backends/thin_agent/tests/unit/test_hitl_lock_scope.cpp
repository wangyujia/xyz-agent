// unit_hitl_lock_scope：验证 HITL 审批不长时间持有全局锁。
//
// 场景：session A 触发 AgentLoop HITL 暂停后，session B 的 chat 应在
// session A 审批（含工具执行）期间依然可完成——此前 continue_after_approval
// 全程持 mu_，session B 被阻塞到 A 的整个 AgentLoop 跑完。
//
// 实现方式：无真实云 → AgentLoop 暂停依赖 mock 云。简化为直接验证
// 并发窗口：A 审批线程与 B chat 线程并发，B 的完成时间不应依赖 A。

#include <atomic>
#include <filesystem>
#include <future>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include <nlohmann/json.hpp>

#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/TaskEngine.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"

namespace {

int g_failures = 0;

int expect(bool ok, const std::string& msg) {
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", msg.c_str());
  }
  return g_failures == 1 ? 1 : 0;
}

}  // namespace

int main() {
  std::filesystem::remove_all("data_lock_scope");  // v0.50.2: 独立目录，避免与并发测试共享 data/

  setenv("THIN_AGENT_TEST_CLOUD_RESPONSE",
         R"({"strategy":"answer_direct","intent":"general_query","confidence":0.5})",
         1);

  thin_agent::DemoConfigCompat cfg;
  cfg.mode = "cloud";
  cfg.provider = "openai-compatible";
  cfg.model_name = "gpt-4.1-mini";
  cfg.api_base = "http://127.0.0.1:9/v1";
  cfg.api_key_env = "THIN_AGENT_TEST_API_KEY";
  setenv("THIN_AGENT_TEST_API_KEY", "test-key-0123456789abcdef0123", 1);
  cfg.fallback = "offline";

  auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
  auto ex = std::make_shared<thin_agent::ActionExecutor>(dc);
  auto te = std::make_shared<thin_agent::TaskEngine>(ex);
  te->init("data_lock_scope/test.db");
  thin_agent::AgentService svc(cfg, ex, te);
  svc.on_session_open("lockA");
  svc.on_session_open("lockB");

  using clock = std::chrono::steady_clock;

  // ── 1. 顺序基线：B chat 单独耗时 ──
  auto t0 = clock::now();
  auto rb = svc.handle_request("lockB", nlohmann::json{{"type", "chat"}, {"text", "B基线"}});
  auto base_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
  expect(rb["type"] == "chat_result", "B baseline chat ok");

  // ── 2. 并发：A 反复 chat（可能触发审批路径的 map 查询）+ B chat ──
  std::atomic<bool> a_running{true};
  std::atomic<long> a_count{0};
  std::thread a([&] {
    while (a_running.load()) {
      svc.handle_request("lockA", nlohmann::json{{"type", "chat_approve"}, {"approved", false}});
      ++a_count;
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  t0 = clock::now();
  auto rb2 = svc.handle_request("lockB", nlohmann::json{{"type", "chat"}, {"text", "B并发"}});
  auto conc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();

  a_running.store(false);
  a.join();

  expect(rb2["type"] == "chat_result", "B concurrent chat ok");
  expect(a_count.load() > 0, "A approve loop ran");
  // B 并发耗时不应超过基线 3 倍 + 500ms（approve 无 pending 时为纯 map 查询）
  expect(conc_ms < base_ms * 3 + 500,
         "B concurrent not blocked by A approves (base=" + std::to_string(base_ms) +
             "ms conc=" + std::to_string(conc_ms) + "ms)");

  // ── 3. chat 与 chat_approve 交错无死锁（最终可完成）──
  auto fut = std::async(std::launch::async, [&] {
    return svc.handle_request("lockB", nlohmann::json{{"type", "chat"}, {"text", "B交错"}});
  });
  svc.handle_request("lockA", nlohmann::json{{"type", "chat_approve"}, {"approved", true}});
  auto r3 = fut.get();
  expect(r3["type"] == "chat_result", "interleaved chat completes");

  std::filesystem::remove_all("data_lock_scope");  // v0.50.2: 独立目录，避免与并发测试共享 data/
  if (g_failures == 0) {
    std::printf("unit:test_hitl_lock_scope PASS\n");
    return 0;
  }
  std::printf("unit:test_hitl_lock_scope FAIL (%d)\n", g_failures);
  return 1;
}
