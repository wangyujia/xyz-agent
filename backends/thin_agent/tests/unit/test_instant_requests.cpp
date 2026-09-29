// unit_instant_requests：请求线程归属判据 + 即时请求"必须返回"兜底（v0.53.82）
//
// 两个背景坑，本测试各有一道闸：
//   ① v0.53.77 在 ping/action/task_submit/task_cancel/task_replay 五处**已有
//      mu_ 临界区内**再补一把 mu_（std::mutex 非递归）→ 首次调用即自死锁、
//      整个 AgentService 永久冻结（gdb: mutex owner == 自身 TID）。
//      而当时 ctest 全绿——因为测试二进制滞后 13 个版本未重链（见 ② 的镜像：
//      陈旧二进制骗绿同样骗红）。故本测试用**看门狗**兜底：即时类型若 30s
//      不返回，进程直接 _Exit(1)（ctest --timeout 之外的第二道闸）。
//   ② 长任务白名单方向反了（v0.52.9 / v0.53.19 / v0.53.59 / v0.53.82 四次漏网）：
//      改为"默认入队 + 即时白名单"后，本测试把判据表锁成契约——历史漏网类型
//      必须全部在队列侧，未知类型必须默认入队（fail-safe）。
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/RequestDispatch.h"
#include "thin_agent/core/TaskEngine.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"
#include "thin_agent/llm/DemoConfigCompat.h"
#include "test_macros.h"

namespace {

std::atomic<bool> g_returned{false};

/// 看门狗：即时请求若卡住（自死锁/长阻塞），30s 后强杀——把"挂死"变成
/// 明确 FAIL，而不是等 ctest 超时（超时会被误读成"环境慢"）。
void start_watchdog() {
  std::thread([] {
    for (int i = 0; i < 300; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (g_returned.load()) return;
    }
    std::cerr << "FAIL: 即时请求 30s 未返回——疑似 mu_ 自死锁或事件循环内长阻塞" << std::endl;
    std::_Exit(1);
  }).detach();
}

const std::vector<std::string> kInstant = {
    "ping", "status", "chat_abort", "metrics", "cache_stats", "usage_stats", "event_recent"};

/// 历史漏网的重类型（每一次家族复发都要在此登记）——必须全部在队列侧。
const std::vector<std::string> kMustBeQueued = {
    "chat", "chat_approve", "chat_chunk",
    "agent_decompose", "agent_decompose_and_run", "agent_decompose_resume",
    "agent_synthesize", "spawn_agent",
    "agent_debate", "agent_dag", "orchestrate", "kanban_run", "goal_auto_reason",
    // v0.53.82 新查出六处：
    "cron_reply", "switch_model", "summarize", "task_submit", "task_replay"};

}  // namespace

int main() {
  // ── 1. 判据表契约（静态，不依赖服务实例）──
  size_t n = 0;
  const char* const* tbl = thin_agent::instant_ws_request_types(&n);
  std::set<std::string> tbl_set;
  for (size_t i = 0; i < n; ++i) tbl_set.insert(tbl[i]);
  for (const auto& t : kInstant) {
    ASSERT_TRUE(("即时白名单含 " + t).c_str(), thin_agent::is_instant_ws_request(t));
  }
  ASSERT_EQ("即时白名单条目数与清单一致", tbl_set.size(), kInstant.size());
  for (const auto& t : kInstant) {
    ASSERT_TRUE(("表内条目 " + t + " 在契约清单中").c_str(), tbl_set.count(t) == 1);
  }
  for (const auto& t : kMustBeQueued) {
    ASSERT_TRUE(("历史漏网类型必须入队: " + t).c_str(),
                thin_agent::ws_request_needs_worker(t));
  }
  ASSERT_TRUE("未知类型默认入队（fail-safe）",
              thin_agent::ws_request_needs_worker("no_such_type_v99"));
  ASSERT_TRUE("空类型默认入队", thin_agent::ws_request_needs_worker(""));

  // 白名单不得出现"服务端并不存在的类型"（防笔误/死条目）
  {
    std::ifstream f("../src/core/AgentServiceWs.cpp");
    if (!f.is_open()) {
      std::cout << "PASS: 源扫描跳过（未找到 AgentServiceWs.cpp，非 ctest 运行目录）\n";
    } else {
      std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      for (const auto& t : kInstant) {
        ASSERT_TRUE(("白名单类型在服务端有 handler: " + t).c_str(),
                    body.find("type == \"" + t + "\"") != std::string::npos);
      }
      for (const auto& t : kMustBeQueued) {
        if (t == "chat_chunk") continue;  // 出站帧型，非入站 handler
        ASSERT_TRUE(("重类型在服务端有 handler: " + t).c_str(),
                    body.find("type == \"" + t + "\"") != std::string::npos);
      }
    }
  }

  // ── 1b. 同 session 串行调度判据（v0.53.82: 两服务端共用一份）──
  {
    std::map<std::string, int> inflight;
    std::vector<std::string> empty;
    ASSERT_TRUE("空队列无可选任务",
                thin_agent::pick_unblocked_job(empty, inflight) == thin_agent::kNoPickableJob);
    std::vector<std::string> q{"s1", "s1", "s2"};
    ASSERT_EQ("空在飞表→取队首", thin_agent::pick_unblocked_job(q, inflight),
              static_cast<std::size_t>(0));
    inflight["s1"] = 1;  // s1 在飞 → 必须跳过两条 s1 选 s2
    ASSERT_EQ("跳过在飞 session 选后续", thin_agent::pick_unblocked_job(q, inflight),
              static_cast<std::size_t>(2));
    inflight["s2"] = 1;  // 全部在飞 → 不可选（调用方等待）
    ASSERT_TRUE("全部 session 在飞→不可选",
                thin_agent::pick_unblocked_job(q, inflight) == thin_agent::kNoPickableJob);
    inflight["s1"] = 0;  // 计数归零视作不在飞（释放后的残留键）
    ASSERT_EQ("计数归零不算在飞", thin_agent::pick_unblocked_job(q, inflight),
              static_cast<std::size_t>(0));
  }

  // ── 2. 行为兜底：解析/执行路径已就绪的 7 个即时类型必须返回 ──
  setenv("THIN_AGENT_FAST_MEDIA", "1", 1);
  // v0.53.82: 独立数据目录——ctest -j4 并行时不得删/用他人目录
  // （test_agent_service 也用 build/data/，双方 remove_all 会互删→并发假红）
  const std::string kDataDir = "data_instant_req";
  std::filesystem::remove_all(kDataDir);
  start_watchdog();

  thin_agent::DemoConfigCompat cfg;
  cfg.mode = "offline";
  cfg.fallback = "offline";
  auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
  auto ex = std::make_shared<thin_agent::ActionExecutor>(dc);
  auto te = std::make_shared<thin_agent::TaskEngine>(ex);
  ASSERT_TRUE("task engine init", te->init(kDataDir + "/tasks.db"));
  thin_agent::AgentService svc(cfg, ex, te);
  svc.on_session_open("s1");

  for (const auto& t : kInstant) {
    std::cout << "  ... call type=" << t << std::endl;
    auto resp = svc.handle_request("s1", nlohmann::json{{"type", t}, {"cmd_id", "c-" + t}});
    ASSERT_TRUE((t + " 返回 JSON").c_str(), resp.is_object() && resp.contains("type"));
    ASSERT_TRUE((t + " 原样回带 cmd_id（无丢失）").c_str(), resp.value("cmd_id", "") == "c-" + t);
    if (t == "ping") ASSERT_EQ("ping → pong", resp.value("type", ""), std::string("pong"));
  }

  // 同一实例上再打一遍：第二次调用同样不得死锁（自死锁的第二形态=持有未释放）
  for (const auto& t : kInstant) {
    auto resp = svc.handle_request("s1", nlohmann::json{{"type", t}});
    ASSERT_TRUE((t + " 二次调用返回").c_str(), resp.is_object());
  }
  g_returned.store(true);

  return TEST_REPORT();
}
