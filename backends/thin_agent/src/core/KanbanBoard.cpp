#include "thin_agent/core/KanbanBoard.h"

namespace thin_agent {

void KanbanBoard::push(const KanbanTask& task) {
  std::lock_guard<std::mutex> lk(mu_);
  // v0.53.43: 容量治理——done/failed 终态任务此前永不清理,push_batch
  /// 高频灌入下 tasks_ 无界缓涨;超限时先淘汰最老终态
  if (tasks_.size() >= 1000) {
    for (auto it = tasks_.begin(); it != tasks_.end() && tasks_.size() >= 800;) {
      if (it->status == "done" || it->status == "failed") it = tasks_.erase(it);
      else ++it;
    }
  }
  tasks_.push_back(task);
}

void KanbanBoard::push_batch(const std::vector<KanbanTask>& tasks) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& t : tasks) tasks_.push_back(t);
}

KanbanTask KanbanBoard::pull(const std::string& worker_id) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& t : tasks_) {
    if (t.status == "pending") {
      t.status = "in_progress";
      t.assigned_to = worker_id;
      return t;
    }
  }
  return {};  // task_id empty = no pending
}

void KanbanBoard::complete(const std::string& task_id, bool ok,
                            const nlohmann::json& result, const std::string& error) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& t : tasks_) {
    if (t.task_id == task_id) {
      t.status = ok ? "done" : "failed";
      t.result = result;
      if (!error.empty()) t.result["error"] = error;
      return;
    }
  }
}

nlohmann::json KanbanBoard::status() const {
  std::lock_guard<std::mutex> lk(mu_);
  int pending = 0, in_progress = 0, done = 0, failed = 0;
  for (auto& t : tasks_) {
    if (t.status == "pending") ++pending;
    else if (t.status == "in_progress") ++in_progress;
    else if (t.status == "done") ++done;
    else if (t.status == "failed") ++failed;
  }
  nlohmann::json tasks_arr = nlohmann::json::array();
  for (auto& t : tasks_) {
    tasks_arr.push_back({
      {"task_id", t.task_id},
      {"name", t.name},
      {"status", t.status},
      {"role", t.role},
      {"assigned_to", t.assigned_to}
    });
  }
  return {
    {"total", tasks_.size()},
    {"pending", pending},
    {"in_progress", in_progress},
    {"done", done},
    {"failed", failed},
    {"tasks", tasks_arr}
  };
}

size_t KanbanBoard::pending_count() const {
  std::lock_guard<std::mutex> lk(mu_);
  size_t count = 0;
  for (auto& t : tasks_) if (t.status == "pending") ++count;
  return count;
}

bool KanbanBoard::has_pending() const {
  return pending_count() > 0;
}

void KanbanBoard::clear() {
  std::lock_guard<std::mutex> lk(mu_);
  tasks_.clear();
}

}  // namespace thin_agent
