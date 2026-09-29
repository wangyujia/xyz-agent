#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "thin_agent/core/ActionExecutor.h"

namespace thin_agent {

/// 任务引擎：将 action 封装为可持久化、可重试、可审计的异步任务。
/// 状态保存在 SQLite（tasks + task_audits），支持幂等提交与 replay。
class TaskEngine {
 public:
  /// 重试与退避相关运行参数。
  struct Config {
    int max_retries{3};           ///< 失败后最大重试次数（总尝试 = max_retries + 1）
    int action_timeout_ms{1000};  ///< 单次 ActionExecutor 调用超时
    int backoff_base_ms{300};     ///< 指数退避基数
    int backoff_max_ms{3000};      ///< 退避上限
    int backoff_jitter_ms{120};   ///< 随机抖动幅度（±）
  };

  explicit TaskEngine(std::shared_ptr<ActionExecutor> executor);
  TaskEngine(std::shared_ptr<ActionExecutor> executor, Config cfg);
  ~TaskEngine();

  /// 打开/创建 SQLite 库并建表；默认路径 data/agent_tasks.db。
  bool init(const std::string& db_path = "data/agent_tasks.db");

  /// 提交任务并同步执行（含重试）；相同 idempotency_key 返回已有 task_id。
  std::string submit_task(const std::string& action,
                          const nlohmann::json& args,
                          const std::string& idempotency_key);

  /// 按 task_id 查询任务详情；不存在时 exists=false。
  nlohmann::json get_task(const std::string& task_id);

  /// 列出最近任务，可按 state / action 过滤。
  nlohmann::json list_tasks(int limit = 20,
                            const std::string& state_filter = "",
                            const std::string& action_filter = "",
                            int* limit_applied_out = nullptr);

  /// 取消非终态任务；已 success/failed/cancelled 时 cancelled=false。
  /// v0.53.89: 除落库改状态外，还置**协作式取消标志**，执行循环据此停止重试/退避，
  /// 且**取消优先于执行结果**（在跑动作结束后不会把 cancelled 覆盖回 success）。
  nlohmann::json cancel_task(const std::string& task_id, const std::string& reason);

  /// v0.53.91 启动收尾：把库中**非终态僵尸任务**（进程崩溃/重启后执行线程已消失、
  /// 状态却仍是 running/retrying/queued）统一收尾——running/retrying → failed(26011)，
  /// queued（从未启动）→ cancelled(26010)，message 标注 interrupted by restart。
  /// 幂等：只碰非终态行；返回收尾条数（>0 时写一条可观测 WARN）。init() 内自动调用。
  std::size_t reap_interrupted_tasks();

  /// 该任务是否已被请求取消（协作式标志，供执行循环/测试观测）。
  bool is_cancelled(const std::string& task_id);

  /// 仍挂着取消请求的任务数（观测用）：终态落库后即释放 → 不会无界增长。
  std::size_t cancelled_pending_count();

  /// 以原任务的 action/args 重新 submit，生成新 task_id。
  nlohmann::json replay_task(const std::string& task_id, const std::string& idempotency_key);

  /// 返回任务状态迁移审计记录（含最新态摘要字段，供记忆/证据渲染）。
  nlohmann::json list_task_audits(const std::string& task_id, int limit = 50);

 private:
  std::string now_iso() const;   ///< 当前 UTC 时间 ISO8601 字符串
  std::string new_task_id();     ///< 生成递增 task-<seq> ID

  void create_schema();          ///< 创建 tasks / task_audits 表
  void persist_task(const std::string& task_id,
                    const std::string& action,
                    const std::string& args_json,
                    const std::string& idem_key);
  std::optional<std::string> find_by_idem(const std::string& idem_key);
  void update_task_row(const std::string& task_id,
                       const std::string& state,
                       int attempts,
                       int code,
                       const std::string& message,
                       const std::string& result_json,
                       const std::string& error_json);
  std::string fetch_task_state(const std::string& task_id);
  bool task_exists(const std::string& task_id);
  void append_audit(const std::string& task_id,
                    const std::string& from_state,
                    const std::string& to_state,
                    int attempts,
                    int code,
                    const std::string& message);
  void sync_seq_from_db();  ///< 启动时从已有 task_id 恢复 seq_

  /// 带指数退避的执行循环，将 ActionExecutor 错误码映射为任务统一错误码。
  nlohmann::json execute_with_retry(const std::string& task_id,
                                    const std::string& action,
                                    const nlohmann::json& args);

  /// 可被取消打断的退避睡眠（100ms 切片轮询标志）；返回 false = 睡眠期间被取消。
  bool sleep_with_cancel(const std::string& task_id, int ms);
  /// 释放取消标志（终态落库后调用，防 set 无界增长）。
  void clear_cancelled(const std::string& task_id);

  std::shared_ptr<ActionExecutor> executor_;  ///< 动作执行器
  Config cfg_;                                 ///< 重试/退避配置
  std::string db_path_;                        ///< SQLite 文件路径
  void* db_{nullptr};                          ///< sqlite3*（void* 避免头文件依赖）
  uint64_t seq_{0};                            ///< task_id 序号计数器
  std::mutex mu_;                              ///< 保护 seq_ 与 DB 写操作
  /// v0.53.89: 取消标志独立互斥量——**刻意不与 mu_ 共用/嵌套**（补锁前置三问：
  /// 同一互斥量在临界区内二次获取=自死锁，见 v0.53.82/v0.53.84 实锤）。
  /// 锁序约定：任何路径都不在持有 mu_ 时获取 cancel_mu_，反之亦然（互不嵌套）。
  std::mutex cancel_mu_;
  std::set<std::string> cancelled_;             ///< 已请求取消的任务 id（终态后释放）
};

}  // namespace thin_agent
