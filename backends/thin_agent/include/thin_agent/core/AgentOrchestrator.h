#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {

/// 多 Agent 编排器：线程池 + 任务队列 + 结果聚合。
/// 构造后不自动启动线程；首次 execute() 或显式 start() 时惰性初始化。
class AgentOrchestrator {
 public:
  struct OrchestrationTask {
    std::string task_id;
    std::string name;
    std::string prompt;
    std::string role;     ///< 角色名（空=默认），executor 按角色创建 AgentLoop
    nlohmann::json context;
  };

  struct TaskResult {
    std::string task_id;
    bool ok{false};
    nlohmann::json result;
    std::string error;
    double latency_ms{0.0};
  };

  struct OrchestrationRequest {
    std::string request_id;
    std::string session_id;
    std::vector<OrchestrationTask> tasks;
  };

  struct OrchestrationResult {
    std::string request_id;
    bool all_ok{false};
    std::vector<TaskResult> results;
    double total_latency_ms{0.0};
  };

  using TaskExecutor = std::function<TaskResult(const OrchestrationTask&)>;

  // v0.11.1: TaskDAG 依赖编排
  struct DAGNode {
    std::string task_id;
    std::string name;
    std::string prompt;
    std::string role;  ///< 可选角色名（空 = 默认）
    std::vector<std::string> depends_on;  ///< 依赖的 task_id 列表
    nlohmann::json context;
  };

  struct DAGRequest {
    std::string request_id;
    std::string session_id;
    std::vector<DAGNode> nodes;
  };

  /// DAG 编排：自动拓扑排序 → 逐波并行执行 → 失败传播。
  /// 依赖关系通过 depends_on 声明，无需手动分 wave。
  OrchestrationResult execute_dag(const DAGRequest& request,
                                   int timeout_ms = 60000);

  AgentOrchestrator() = default;
  explicit AgentOrchestrator(TaskExecutor executor) : executor_(std::move(executor)) {}
  ~AgentOrchestrator() { shutdown(); }

  void set_executor(TaskExecutor executor) { executor_ = std::move(executor); }

  /// 启动工作线程池（幂等）。
  void start(int num_workers = 3);

  /// v0.53.78: 停机——join worker 池+等在飞 detached 线程归零
  /// (此前无 stop:execute_async detach 捕 this,析构后继续跑=UAF)
  void stop();

  /// 同步执行编排请求（自动惰性启动）。
  OrchestrationResult execute(const OrchestrationRequest& request,
                              int timeout_ms = 60000);

  /// 异步执行。
  std::future<OrchestrationResult> execute_async(const OrchestrationRequest& request,
                                                  int timeout_ms = 60000);

  struct PoolStatus {
    int num_workers;
    int active_tasks;
    int queued_tasks;
    uint64_t total_completed;
  };
  PoolStatus status() const;

  void shutdown();

 private:
  void worker_loop();

  struct InternalTask {
    OrchestrationTask task;
    std::promise<TaskResult> promise;
  };

  TaskExecutor executor_;
  int num_workers_{0};
  std::vector<std::thread> workers_;
  std::queue<InternalTask> task_queue_;
  mutable std::mutex queue_mu_;
  std::condition_variable queue_cv_;
  std::atomic<bool> started_{false};
  std::atomic<bool> running_{false};
  // v0.53.78: detached request 线程在飞计数——析构等待防 UAF
  /// (execute_async per-request detach 捕 this,AgentService 析构后
  /// 继续跑=悬垂;计数归零才允许析构完成)
  std::atomic<int> in_flight_{0};
  std::mutex exit_mu_;               ///< 析构与在飞线程的交接锁
  std::condition_variable exit_cv_;  ///< in_flight_ 归零通知


  std::atomic<int> active_tasks_{0};
  std::atomic<uint64_t> total_completed_{0};
};

}  // namespace thin_agent
