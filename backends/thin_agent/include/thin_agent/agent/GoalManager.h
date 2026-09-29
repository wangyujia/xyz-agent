#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace thin_agent {
namespace agent {

/// 长期目标：跨会话持久化跟踪。
/// v0.52.3: 树化——parent_id/subtask_ids 表达分解任务树
/// （复杂任务 = 父目标 + N 子任务；子任务完成自动推进父进度）。
struct Goal {
  int id{0};
  std::string description;
  std::string status{"pending"};  ///< pending / active / done / failed
  int progress_pct{0};
  std::string created_at;
  std::string updated_at;
  int parent_id{0};                    ///< v0.52.3: 父目标 id（0=根任务）
  std::vector<int> subtask_ids;        ///< v0.52.3: 子任务 id 列表
  nlohmann::json meta;                 ///< v0.52.4: 通用元数据（分解 payload/断点等）
};

/// 目标管理器：SQLite 持久化 + CRUD + system prompt 注入。
class GoalManager {
 public:
  GoalManager() = default;
  explicit GoalManager(const std::string& db_path);
  ~GoalManager();

  GoalManager(const GoalManager&) = delete;
  GoalManager& operator=(const GoalManager&) = delete;

  /// 创建目标。
  Goal create(const std::string& description);

  /// 列出所有目标（可按状态过滤）。
  std::vector<Goal> list(const std::string& status_filter = "") const;

  /// 更新目标字段。
  bool update(int id, const nlohmann::json& fields);

  /// 删除目标。
  bool remove(int id);

  /// 获取活跃目标数目。
  int active_count() const;

  /// 将活跃目标拼接为 system prompt 注入文本。
  std::string to_prompt_injection() const;

  /// 返回超过 N 小时未更新的活跃目标。
  std::vector<Goal> stalled_goals(int hours = 24) const;

  /// 快速更新进度百分比。
  bool update_progress(int id, int progress_pct);

  /// 快速更新状态+进度。
  bool update_status(int id, const std::string& status, int progress_pct = -1);

  /// v0.52.3: 创建子任务（挂到 parent 下，双向维护树关系）。
  Goal create_subtask(int parent_id, const std::string& description);

  /// v0.52.3: 取整个目标（含树字段）。
  Goal get(int id) const;

  /// v0.52.3: 子任务状态变更后重算父进度（done=100）并级联更新。
  /// 返回父目标新进度（-1=失败/无父）。
  int refresh_parent_progress(int child_id);

  /// v0.52.3: 任务树 JSON（goal_tree 命令用）。
  nlohmann::json tree_json(int root_id) const;

  /// v0.52.4: 通用元数据读写（分解 payload、断点进度等）。
  nlohmann::json get_meta(int id) const;
  bool set_meta(int id, const nlohmann::json& meta);

 private:
  void ensure_db();
  std::string now_iso() const;

  std::string db_path_;
  sqlite3* db_{nullptr};
  mutable std::mutex mu_;
};

}  // namespace agent
}  // namespace thin_agent
