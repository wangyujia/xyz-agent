#include "thin_agent/core/AgentOrchestrator.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_map>

namespace thin_agent {

void AgentOrchestrator::start(int num_workers) {
  if (started_.exchange(true)) return;  // 幂等

  num_workers_ = std::max(1, num_workers);
  running_ = true;

  for (int i = 0; i < num_workers_; ++i) {
    workers_.emplace_back(&AgentOrchestrator::worker_loop, this);
  }
}

void AgentOrchestrator::stop() {
  // v0.53.78: 三段停机——①worker 池闸+join ②在飞 detached 计数等待
  /// (此前无 stop:workers_ 悬+detached 捕 this 悬,AgentService 析构
  /// 后继续跑=UAF;与 cron/monitor 停机闸同族)
  if (!started_.exchange(false)) return;
  running_ = false;
  {
    std::lock_guard<std::mutex> lk(queue_mu_);
    queue_cv_.notify_all();
  }
  for (auto& w : workers_) {
    if (w.joinable()) w.join();
  }
  workers_.clear();
  std::unique_lock<std::mutex> lk(exit_mu_);
  exit_cv_.wait(lk, [this] { return in_flight_.load() == 0; });
}

AgentOrchestrator::OrchestrationResult AgentOrchestrator::execute(
    const OrchestrationRequest& request, int timeout_ms) {
  if (!started_) start(3);  // 惰性启动

  auto t0 = std::chrono::steady_clock::now();

  OrchestrationResult orch_result;
  orch_result.request_id = request.request_id;

  if (!executor_) {
    orch_result.all_ok = false;
    return orch_result;
  }

  // 提交任务
  std::vector<std::future<TaskResult>> futures;
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    for (const auto& task : request.tasks) {
      InternalTask it;
      it.task = task;
      futures.push_back(it.promise.get_future());
      task_queue_.push(std::move(it));
    }
  }
  queue_cv_.notify_all();

  // 等待全部完成
  for (auto& fut : futures) {
    auto status = fut.wait_for(std::chrono::milliseconds(timeout_ms));
    if (status == std::future_status::ready) {
      orch_result.results.push_back(fut.get());
    } else {
      TaskResult tr;
      tr.task_id = "unknown";
      tr.ok = false;
      tr.error = "timeout";
      orch_result.results.push_back(tr);
    }
  }

  orch_result.all_ok = true;
  for (const auto& r : orch_result.results) {
    if (!r.ok) { orch_result.all_ok = false; break; }
  }

  auto t1 = std::chrono::steady_clock::now();
  orch_result.total_latency_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

  return orch_result;
}

std::future<AgentOrchestrator::OrchestrationResult> AgentOrchestrator::execute_async(
    const OrchestrationRequest& request, int timeout_ms) {
  auto promise = std::make_shared<std::promise<OrchestrationResult>>();
  auto future = promise->get_future();

  in_flight_.fetch_add(1);  // v0.53.78: 在飞计数——stop() 等归零
  std::thread([this, request, timeout_ms, promise]() {
    promise->set_value(execute(request, timeout_ms));
    {
      // 交接锁内自减+通知:与 stop() 的等待互斥,防析构竞态
      std::lock_guard<std::mutex> lk(exit_mu_);
      in_flight_.fetch_sub(1);
    }
    exit_cv_.notify_one();
  }).detach();

  return future;
}

void AgentOrchestrator::worker_loop() {
  while (running_) {
    InternalTask task;
    {
      std::unique_lock<std::mutex> lock(queue_mu_);
      queue_cv_.wait(lock, [this]() {
        return !task_queue_.empty() || !running_;
      });
      if (!running_ && task_queue_.empty()) break;
      task = std::move(task_queue_.front());
      task_queue_.pop();
    }

    active_tasks_++;

    auto t0 = std::chrono::steady_clock::now();
    TaskResult result;
    try {
      result = executor_(task.task);
    } catch (const std::exception& e) {
      result.task_id = task.task.task_id;
      result.ok = false;
      result.error = e.what();
    } catch (...) {
      result.task_id = task.task.task_id;
      result.ok = false;
      result.error = "unknown error";
    }
    auto t1 = std::chrono::steady_clock::now();
    result.latency_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    task.promise.set_value(result);
    active_tasks_--;
    total_completed_++;
  }
}

AgentOrchestrator::PoolStatus AgentOrchestrator::status() const {
  PoolStatus s;
  s.num_workers = num_workers_;
  s.active_tasks = active_tasks_;
  {
    std::lock_guard<std::mutex> lock(queue_mu_);
    s.queued_tasks = static_cast<int>(task_queue_.size());
  }
  s.total_completed = total_completed_;
  return s;
}

void AgentOrchestrator::shutdown() {
  running_ = false;
  queue_cv_.notify_all();
  for (auto& w : workers_) {
    if (w.joinable()) w.join();
  }
  workers_.clear();
  started_ = false;
}

// ── v0.11.1: DAG 依赖编排 ──────────────────────────────────────────

AgentOrchestrator::OrchestrationResult AgentOrchestrator::execute_dag(
    const DAGRequest& request, int timeout_ms) {
  if (!started_) start(3);

  auto t0 = std::chrono::steady_clock::now();
  OrchestrationResult orch_result;
  orch_result.request_id = request.request_id;

  const auto& nodes = request.nodes;
  if (nodes.empty() || !executor_) {
    orch_result.all_ok = (nodes.empty());
    return orch_result;
  }

  // 构建 task_id → index 映射
  std::unordered_map<std::string, size_t> idx_map;
  for (size_t i = 0; i < nodes.size(); ++i) idx_map[nodes[i].task_id] = i;

  // 拓扑排序：计算入度
  std::vector<int> in_degree(nodes.size(), 0);
  std::vector<std::vector<size_t>> dependents(nodes.size());  // 谁依赖我
  for (size_t i = 0; i < nodes.size(); ++i) {
    for (const auto& dep : nodes[i].depends_on) {
      auto it = idx_map.find(dep);
      if (it != idx_map.end()) {
        ++in_degree[i];
        dependents[it->second].push_back(i);
      }
    }
  }

  // 结果存储 + 状态追踪
  std::vector<TaskResult> results(nodes.size());
  std::vector<bool> completed(nodes.size(), false);
  std::vector<bool> failed(nodes.size(), false);
  int remaining = static_cast<int>(nodes.size());

  // Kahn 算法：逐波执行
  while (remaining > 0) {
    // 找到当前可执行的节点（入度=0 且未完成）
    std::vector<size_t> wave;
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (!completed[i] && in_degree[i] == 0 && !failed[i]) {
        wave.push_back(i);
      }
    }

    if (wave.empty()) {
      // 无可执行节点但有剩余 → 循环依赖或全部失败
      for (size_t i = 0; i < nodes.size(); ++i) {
        if (!completed[i] && !failed[i]) {
          results[i].task_id = nodes[i].task_id;
          results[i].ok = false;
          results[i].error = "circular dependency or blocked";
          completed[i] = true;
          failed[i] = true;
          --remaining;
        }
      }
      break;
    }

    // 并行执行当前 wave
    {
      std::vector<std::future<TaskResult>> futures;
      {
        std::lock_guard<std::mutex> lock(queue_mu_);
        for (auto i : wave) {
          InternalTask it;
          it.task = {nodes[i].task_id, nodes[i].name, nodes[i].prompt, nodes[i].role, nodes[i].context};
          futures.push_back(it.promise.get_future());
          task_queue_.push(std::move(it));
        }
      }
      queue_cv_.notify_all();

      for (size_t fi = 0; fi < futures.size(); ++fi) {
        auto status = futures[fi].wait_for(std::chrono::milliseconds(timeout_ms));
        size_t node_idx = wave[fi];
        if (status == std::future_status::ready) {
          results[node_idx] = futures[fi].get();
        } else {
          results[node_idx].task_id = nodes[node_idx].task_id;
          results[node_idx].ok = false;
          results[node_idx].error = "timeout";
        }
        completed[node_idx] = true;
        if (!results[node_idx].ok) failed[node_idx] = true;
        --remaining;
      }
    }

    // 失败传播：将失败节点的所有后继标记为 failed（级联失败）
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (completed[i] && failed[i]) {
        for (auto dep_idx : dependents[i]) {
          if (!completed[dep_idx] && !failed[dep_idx]) {
            results[dep_idx].task_id = nodes[dep_idx].task_id;
            results[dep_idx].ok = false;
            results[dep_idx].error = "skipped: upstream task '" + nodes[i].task_id + "' failed";
            completed[dep_idx] = true;
            failed[dep_idx] = true;
            --remaining;
            // 递归传播
            for (auto sub : dependents[dep_idx]) {
              if (!completed[sub] && !failed[sub]) {
                --in_degree[sub];  // 减少入度，避免 wave 循环
              }
            }
          }
        }
      }
    }

    // 更新入度：已完成节点对其后继的入度减一
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (completed[i]) {
        for (auto dep_idx : dependents[i]) {
          if (in_degree[dep_idx] > 0) --in_degree[dep_idx];
        }
      }
    }
  }

  orch_result.all_ok = true;
  for (auto& r : results) {
    orch_result.results.push_back(r);
    if (!r.ok) orch_result.all_ok = false;
  }

  auto t1 = std::chrono::steady_clock::now();
  orch_result.total_latency_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

  return orch_result;
}

}  // namespace thin_agent
