// unit_task_cancel：TaskEngine 协作式取消（v0.53.89）
//
// 修复的行为缺陷（本轮实锤）：
//   cancel_task 此前是"记账式取消"——只把 tasks 行改成 cancelled，**全仓没有任何
//   取消检查点**：① 在跑动作不中断（副作用照常发生）② 重试与退避 sleep 照跑
//   ③ 最伤的一条：submit_task 在动作结束后**无条件写终态** → 取消被 success 静默
//   覆盖（用户看到"取消成功"，任务实际照常完成）。
//
// 本测试用**阻塞式假设备**把动作卡在 act() 里，从另一线程取消，逐条锁住修复契约：
//   T1 取消在跑任务 → 终态必须是 cancelled（不能被 success 覆盖）
//   T2 取消时动作仍返失败 → 不再重试（仅 1 次调用）且退避 sleep 被打断（<2s）
//   T3 未取消的任务照常 success（防"把所有任务都标成 cancelled"的反向回归）
//   T4 取消终态任务 → cancelled=false（幂等）
//   T5 终态落库后取消标志被释放（防 set 无界增长）
#include <chrono>
#include <filesystem>
#include <system_error>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "test_macros.h"
#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/TaskEngine.h"

using namespace thin_agent;

/// 阻塞式假设备：act() 进入后等测试线程 release()，返回值可控。
class BlockingDc : public IDeviceControl {
 public:
  Result get(const std::string&, const Kv&, int) override { return Result{0, "ok", {}}; }
  Result set(const std::string&, const Kv&, int) override { return Result{0, "ok", {}}; }
  Result evt(const std::string&, const std::string&, const Kv&) override {
    return Result{0, "ok", {}};
  }
  Result act(const std::string&, const Kv&, int) override {
    {
      std::lock_guard<std::mutex> lk(m_);
      entered_ = true;
      ++calls_;
    }
    cv_enter_.notify_all();
    std::unique_lock<std::mutex> lk(m_);
    cv_release_.wait(lk, [this] { return released_; });
    Result r;
    r.code = ret_code_;
    r.message = (ret_code_ == 0) ? "ok" : "boom";
    return r;
  }
  void wait_entered() {
    std::unique_lock<std::mutex> lk(m_);
    cv_enter_.wait(lk, [this] { return entered_; });
  }
  void release(int code) {
    {
      std::lock_guard<std::mutex> lk(m_);
      released_ = true;
      ret_code_ = code;
    }
    cv_release_.notify_all();
  }
  int calls() {
    std::lock_guard<std::mutex> lk(m_);
    return calls_;
  }

 private:
  std::mutex m_;
  std::condition_variable cv_enter_, cv_release_;
  bool entered_{false};
  bool released_{false};
  int calls_{0};
  int ret_code_{0};
};

/// 立即返回的假设备（用于"未取消照常成功"的正向基线）。
class ImmediateDc : public IDeviceControl {
 public:
  Result get(const std::string&, const Kv&, int) override { return Result{0, "ok", {}}; }
  Result set(const std::string&, const Kv&, int) override { return Result{0, "ok", {}}; }
  Result evt(const std::string&, const std::string&, const Kv&) override {
    return Result{0, "ok", {}};
  }
  Result act(const std::string&, const Kv&, int) override { return Result{0, "ok", {}}; }
};

/// TaskEngine 内含 std::mutex（不可拷贝/移动）→ 只给配置，就地构造。
static TaskEngine::Config test_cfg() {
  TaskEngine::Config cfg;
  cfg.max_retries = 3;
  cfg.action_timeout_ms = 5000;
  cfg.backoff_base_ms = 3000;   // 退避打满 3s —— 若取消没打断 sleep 就会超时露馅
  cfg.backoff_max_ms = 3000;
  cfg.backoff_jitter_ms = 0;
  return cfg;
}

int main() {
  // 独立数据目录：避免与其它测试共享 data/ 时 -j4 并行互删（v0.53.82 假红实锤）
  std::error_code ec;
  std::filesystem::remove_all("data_taskcancel", ec);
  std::filesystem::create_directories("data_taskcancel", ec);
  const std::string db = "data_taskcancel/agent_tasks.db";

  // ── T1：取消在跑任务 → 终态 cancelled（结果后写保护）──────────────
  {
    auto dc = std::make_shared<BlockingDc>();
    TaskEngine eng(std::make_shared<ActionExecutor>(dc), test_cfg());
    eng.init(db);
    std::string tid;
    std::thread runner([&] {
      tid = eng.submit_task("capture_photo", nlohmann::json{{"n", 1}}, "idem-t1");
    });
    dc->wait_entered();
    auto newest = eng.list_tasks(1);
    ASSERT_TRUE("T1 有新任务在跑", newest.is_array() && !newest.empty());
    const std::string id = newest[0].value("task_id", "");
    auto c = eng.cancel_task(id, "user cancelled");
    ASSERT_TRUE("T1 取消受理(cancelled=true)", c.value("cancelled", false));
    ASSERT_EQ("T1 库中状态先落 cancelled", eng.get_task(id).value("state", ""),
              std::string("cancelled"));
    dc->release(0);   // 动作照常"成功"返回 —— 这是本条缺陷的核心场景
    runner.join();
    auto after = eng.get_task(id);
    ASSERT_EQ("T1 结果后写保护：终态仍为 cancelled（未被 success 覆盖）",
              after.value("state", ""), std::string("cancelled"));
    ASSERT_EQ("T1 取消错误码 26010", after.value("code", 0), 26010);
    ASSERT_EQ("T1 动作只执行 1 次", dc->calls(), 1);
    ASSERT_EQ("T1 取消标志已释放（防无界增长）",
              static_cast<long long>(eng.cancelled_pending_count()), 0LL);
  }

  // ── T2：取消时动作失败 → 不再重试 + 退避 sleep 被打断 ──────────────
  {
    auto dc = std::make_shared<BlockingDc>();
    TaskEngine eng(std::make_shared<ActionExecutor>(dc), test_cfg());
    eng.init(db);
    const auto t0 = std::chrono::steady_clock::now();
    std::thread runner([&] {
      eng.submit_task("start_recording", nlohmann::json::object(), "idem-t2");
    });
    dc->wait_entered();
    auto newest = eng.list_tasks(1);
    const std::string id = newest[0].value("task_id", "");
    eng.cancel_task(id, "user cancelled");
    dc->release(5001);   // 设备失败 → 未取消时本会退避 3s 后重试
    runner.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    ASSERT_EQ("T2 取消后不再重试（仅 1 次动作调用）", dc->calls(), 1);
    ASSERT_TRUE("T2 退避 sleep 被打断（<2s，配置退避基数为 3s）", ms < 2000);
    auto after = eng.get_task(id);
    ASSERT_EQ("T2 终态 cancelled", after.value("state", ""), std::string("cancelled"));
    ASSERT_EQ("T2 标志已释放", static_cast<long long>(eng.cancelled_pending_count()), 0LL);
  }

  // ── T3：未取消任务照常成功（反向回归守卫）─────────────────────────
  {
    auto dc = std::make_shared<ImmediateDc>();
    TaskEngine eng(std::make_shared<ActionExecutor>(dc), test_cfg());
    eng.init(db);
    const std::string id = eng.submit_task("capture_photo", nlohmann::json::object(), "idem-t3");
    auto t = eng.get_task(id);
    ASSERT_EQ("T3 未取消 → success", t.value("state", ""), std::string("success"));
    ASSERT_EQ("T3 成功码 0", t.value("code", -1), 0);
    ASSERT_EQ("T3 无残留取消标志", static_cast<long long>(eng.cancelled_pending_count()), 0LL);

    // ── T4：取消终态任务 → cancelled=false（幂等）───────────────────
    auto c = eng.cancel_task(id, "too late");
    ASSERT_TRUE("T4 终态任务取消返回 cancelled=false", !c.value("cancelled", true));
    ASSERT_EQ("T4 原因=terminal state", c.value("reason", ""), std::string("terminal state"));
    ASSERT_EQ("T4 状态未被改动", eng.get_task(id).value("state", ""), std::string("success"));
  }

  return TEST_REPORT();
}
