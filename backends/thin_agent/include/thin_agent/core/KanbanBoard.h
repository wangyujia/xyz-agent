#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace thin_agent {

/// Kanban 看板任务。
struct KanbanTask {
  std::string task_id;
  std::string name;
  std::string prompt;
  std::string role;                       ///< 建议角色
  std::string status;                     ///< pending / in_progress / done / failed
  nlohmann::json result;                  ///< 执行结果
  std::string assigned_to;                ///< 被哪个 agent 认领
};

/// Kanban 看板：任务池 + 状态管理 + 自主抢单。
class KanbanBoard {
 public:
  KanbanBoard() = default;

  /// 添加任务到待办池。
  void push(const KanbanTask& task);

  /// 批量添加任务。
  void push_batch(const std::vector<KanbanTask>& tasks);

  /// Worker 抢单：获取下一个 pending 任务并标记 in_progress。
  /// @param worker_id 认领的 worker ID。
  /// @return 未找到时 task_id 为空。
  KanbanTask pull(const std::string& worker_id);

  /// 报告任务完成（或失败）。
  void complete(const std::string& task_id, bool ok,
                const nlohmann::json& result, const std::string& error = "");

  /// 返回看板状态摘要。
  nlohmann::json status() const;

  /// 待办任务数。
  size_t pending_count() const;

  /// 总任务数。
  size_t total_count() const { return tasks_.size(); }

  /// 是否还有待处理任务。
  bool has_pending() const;

  /// 清空看板。
  void clear();

 private:
  mutable std::mutex mu_;
  std::vector<KanbanTask> tasks_;
  std::atomic<int> next_id_{1};
};

}  // namespace thin_agent
