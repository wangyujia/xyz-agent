#pragma once

#include <atomic>
#include <condition_variable>  // v0.49.2: stop_cv_ 可中断等待
#include <functional>
#include <map>  // v0.54.6: task_exec_mu_
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

struct sqlite3;  // fwd decl for db_ pointer

namespace thin_agent {

/// 轻量 Cron 调度器：定时器线程 + SQLite 持久化任务表。
/// 支持一次性 / 周期性任务，提供 WS API 管理接口。
class CronScheduler {
 public:
  /// 任务执行回调：task_json 含 id/name/schedule/prompt 等字段。
  using TaskCallback = std::function<void(const nlohmann::json& task)>;

  CronScheduler();
  ~CronScheduler();

  /// 启动调度线程。
  /// @param db_path    SQLite 文件路径，空 = 内存
  /// @param callback   任务触发时的执行回调
  /// @param tick_ms    调度精度（毫秒，默认 5000）
  void start(const std::string& db_path = "",
             TaskCallback callback = nullptr,
             int tick_ms = 5000);

  /// 停止调度线程。
  void stop();

  /// v0.53.4: 注销回调（宿主析构时调用）——ticker 下轮起不再触发。
  /// 与 stop 的区别：调度继续（跨实例共享静态 sched），仅回调静默。
  void clear_callback();

  /// ── 任务 CRUD ───────────────────────────

  /// 添加任务。返回含 id 的完整任务 JSON。
  nlohmann::json add_task(const std::string& name,
                          const std::string& schedule,
                          const std::string& prompt,
                          bool enabled = true);

  // v0.42.0: 带全部元数据的添加任务
  nlohmann::json add_task_full(const std::string& name,
      const std::string& schedule, const std::string& prompt, bool enabled,
      int max_repeat, bool no_agent, const std::string& no_agent_script,
      const std::string& deliver_to, const std::string& workdir,
      const std::string& model = "", const std::string& provider = "",
      int context_from = 0, const std::string& skills = "",
      const std::string& toolsets = "");

  /// 列出所有任务。
  std::vector<nlohmann::json> list_tasks();

  /// 更新任务（按 id）。
  bool update_task(int task_id, const nlohmann::json& fields);

  /// 删除任务。
  bool remove_task(int task_id);

  /// 暂停/恢复任务。
  bool set_enabled(int task_id, bool enabled);

  /// 手动触发一次任务执行。
  bool trigger_now(int task_id);

  /// ── v0.42.1: context_from 输出存储 ──

  /// 保存任务输出（供下游 context_from 引用）。
  void save_output(const std::string& task_key, int64_t timestamp,
                   const std::string& output);

  /// 加载上游任务最近一次输出。
  std::string load_output(int task_id);

  /// ── 统计 ───────────────────────────────

  /// 任务总数 / 启用数 / 下次触发时间。
  nlohmann::json stats() const;

 private:
  void init_db();
  void ticker_loop();
  bool is_due(const std::string& schedule, int64_t last_run_ts) const;
  void execute_task(const nlohmann::json& task);
  /// 读取 SQLite schema 版本。
  int schema_version() const;
  /// 升级 SQLite schema。
  void upgrade_schema(int from_version);

  std::string db_path_;
  TaskCallback callback_;
  int tick_ms_{5000};

  std::unique_ptr<std::thread> ticker_;
  std::atomic<bool> running_{false};
  mutable std::recursive_mutex mu_;  // v0.53.4: execute_task 锁内回调（ticker 持锁扫描）需可重入
  // v0.54.6 (R91 拍板)：trigger_now 的**每任务串行执行锁**（map 由 mu_ 保护，执行期间只持任务锁）。
  // 语义 = 旧的"持 mu_ 串行"效果：同一任务的并发二次触发**排队**，等前一次执行完再执行一次
  // （即"排队重跑"；用户 2026-09-18 拍板，v0.54.3 的"在飞即拒绝返回 false"作废）。
  // 用 recursive_mutex：宿主 callback 内若经 cron skill 再触发同一任务，旧实现（持 recursive mu_）
  // 是可重入的——换成普通 mutex 会引入同线程自死锁（R70 家族）。
  // 与执行无关的 API（add/remove/list/stats/stop）只受 mu_ 短暂保护，不受任务执行时长影响。
  std::map<int, std::shared_ptr<std::recursive_mutex>> task_exec_mu_;
  // v0.49.2: 可中断等待——stop() 立即唤醒 ticker，无需耗满一个 tick。
  // 此前 sleep_for(tick_ms) 不可中断：多实例集中析构时每个最多等 5s，
  // 单测进程退出阶段可累积数十秒（27 个实例实测卡死）。
  std::condition_variable_any stop_cv_;  // v0.53.4: 配 recursive_mutex

  sqlite3* db_{nullptr};  ///< 持久 DB 句柄（:memory: 模式必需）
  void ensure_db();       ///< 幂等建表（使用持久的 db_）
};

}  // namespace thin_agent
