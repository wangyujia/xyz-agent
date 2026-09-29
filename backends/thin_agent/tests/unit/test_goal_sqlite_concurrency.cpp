// test_goal_sqlite_concurrency：v0.52.17 GoalManager 并发压测
//
// 背景：未测领域②——sqlite 并发写。GoalManager 内部 8 处
// lock_guard 串行化进程内写，但：
// 1. 进程内多线程（波次并行 update_status+refresh_parent_progress
//    +set_meta）并发混合读写
// 2. 外部进程直连同一 db（取证脚本/哨兵常态操作）
// 未设 busy_timeout——外部读撞写锁立即 SQLITE_BUSY 丢弃。
//
// 验证：
// 1. 8 线程 × 50 操作混合（create/update_status/set_meta/get）
//    全部成功，无异常无丢数据
// 2. 总量守恒：创建数=查询到的行数
// 3. 外部进程并发读（subprocess sqlite3 CLI）在写入压力下不报
//    database is locked（busy_timeout 修复后）
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "test_macros.h"

#include "thin_agent/agent/GoalManager.h"

using namespace thin_agent::agent;

int main() {
  const std::string db = "/tmp/test_goal_conc.db";
  ::remove(db.c_str());

  // 1) 多线程混合读写
  {
    GoalManager gm(db);
    std::atomic<int> created{0}, failed{0};
    std::vector<std::thread> ths;
    for (int t = 0; t < 8; ++t) {
      ths.emplace_back([&, t]() {
        for (int i = 0; i < 50; ++i) {
          try {
            auto g = gm.create("conc-" + std::to_string(t) + "-" +
                               std::to_string(i));
            if (g.id > 0) {
              ++created;
              gm.update_status(g.id, (i % 2) ? "done" : "active");
              nlohmann::json meta;
              meta["wave"] = t;
              meta["i"] = i;
              gm.set_meta(g.id, meta);
            } else {
              ++failed;
            }
          } catch (...) {
            ++failed;
          }
        }
      });
    }
    for (auto& th : ths) th.join();

    ASSERT_TRUE("并发零失败", failed.load() == 0);
    ASSERT_TRUE("创建 400 条", created.load() == 400);

    // 2) 总量守恒
    auto actives = gm.list("active");
    auto dones = gm.list("done");
    int total = 0;
    for (const auto& g : actives) (void)g, ++total;
    for (const auto& g : dones) (void)g, ++total;
    ASSERT_TRUE("总量守恒 400=200+200", total == 400);
  }

  // 3) 外部进程并发读（写压力进行中）
  {
    GoalManager gm(db);
    std::atomic<bool> stop{false};
    std::atomic<int> ext_fail{0}, ext_ok{0};
    std::thread writer([&]() {
      int i = 0;
      while (!stop.load()) {
        auto g = gm.create("ext-" + std::to_string(i++));
        gm.update_status(g.id, "done");
      }
    });
    std::thread ext_reader([&]() {
      while (!stop.load()) {
        FILE* f = ::popen(
            "sqlite3 /tmp/test_goal_conc.db 'SELECT COUNT(*) FROM goals' "
            "2>&1", "r");
        if (f) {
          char buf[128] = {0};
          fgets(buf, sizeof(buf), f);
          int rc = pclose(f);
          if (std::string(buf).find("locked") != std::string::npos ||
              std::string(buf).find("busy") != std::string::npos)
            ++ext_fail;
          else
            ++ext_ok;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
    std::this_thread::sleep_for(std::chrono::seconds(3));
    stop = true;
    writer.join();
    ext_reader.join();
    // busy_timeout 修复后外部读应全通；修复前 sqlite3 CLI 默认
    // busy_timeout=0 撞锁即"database is locked"
    printf("外部读: ok=%d fail=%d\n", ext_ok.load(), ext_fail.load());
    ASSERT_TRUE("外部并发读不报 locked", ext_fail.load() == 0);
  }

  ::remove(db.c_str());
  return TEST_REPORT();
}
