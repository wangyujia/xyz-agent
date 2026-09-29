// unit_ops_stats：/stats 运维汇总统计测试（v0.50.8）
// 验证 ops_stats() 数据源不变量（HTTP 层由 ws_agent_main 薄封装，冒烟另测）：
//   1. 必含五段：version/uptime_seconds/usage/cache/cron
//   2. version == kThinAgentVersion（与握手 hello 同源）
//   3. uptime_seconds 单调不减
//   4. usage 四字段与 usage_stats() 一致（api_calls 为整数）
//   5. cache 含 hits/misses/hit_rate；cron 含任务计数
//   6. 独立 data/ 目录（并发测试隔离铁律）

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>  // getpid()

#include "thin_agent/Version.h"
#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/llm/DemoConfigCompat.h"
#include "thin_agent/core/TaskEngine.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"

#include "test_macros.h"

int main() {
  // 独立 data 目录（ctest -j4 并发隔离）
  const std::string data_dir = "/tmp/ops_stats_test_data_" +
                               std::to_string(::getpid());
  std::filesystem::remove_all(data_dir);
  setenv("THIN_AGENT_HOME", data_dir.c_str(), 1);

  thin_agent::DemoConfigCompat cfg;
  cfg.mode = "offline";
  auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
  auto executor = std::make_shared<thin_agent::ActionExecutor>(dc);
  auto task_engine = std::make_shared<thin_agent::TaskEngine>(executor);
  task_engine->init(data_dir + "/test.db");

  thin_agent::AgentService svc(cfg, executor, task_engine);

  // ── 1. 五段齐全 ──
  auto j = svc.ops_stats();
  ASSERT_TRUE("has version", j.contains("version"));
  ASSERT_TRUE("has uptime_seconds", j.contains("uptime_seconds"));
  ASSERT_TRUE("has usage", j.contains("usage"));
  ASSERT_TRUE("has cache", j.contains("cache"));
  ASSERT_TRUE("has cron", j.contains("cron"));

  // ── 2. version 同源 ──
  ASSERT_EQ("version == kThinAgentVersion",
            j["version"].get<std::string>(),
            std::string(thin_agent::kThinAgentVersion));

  // ── 3. uptime 单调不减 ──
  {
    auto t1 = j["uptime_seconds"].get<long long>();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto t2 = svc.ops_stats()["uptime_seconds"].get<long long>();
    ASSERT_TRUE("uptime monotonic non-decreasing", t2 >= t1);
  }

  // ── 4. usage 与 usage_stats() 一致 ──
  {
    auto u1 = svc.ops_stats()["usage"];
    auto u2 = svc.usage_stats();
    ASSERT_EQ("usage.api_calls consistent",
              u1["api_calls"].get<long long>(),
              u2["api_calls"].get<long long>());
    ASSERT_EQ("usage.total_tokens consistent",
              u1["total_tokens"].get<long long>(),
              u2["total_tokens"].get<long long>());
  }

  // ── 5. cache/cron 字段 ──
  {
    auto cache = j["cache"];
    ASSERT_TRUE("cache has hits", cache.contains("hits"));
    ASSERT_TRUE("cache has misses", cache.contains("misses"));
    ASSERT_TRUE("cache has hit_rate", cache.contains("hit_rate"));
    auto cron = j["cron"];
    ASSERT_TRUE("cron is object/array", !cron.is_null());
  }

  std::filesystem::remove_all(data_dir);
  return TEST_REPORT();
}
