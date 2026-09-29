// v0.53.0: state 插件 —— 目标/检查点域工具（goal_add/list/update +
// checkpoint_save/rollback/list + checkpoint_fs_*）。
//
// 从 AgentService 迁出。GoalManager/CheckpointManager/FilesystemCheckpoint
// 类留核心（引擎注入点深耦合：goal 43 处引用、FC 循环自动快照），
// 插件经 PluginContext 的服务查询位获取实例指针；查不到（类未启用）
// 返回 not available（与原核心行为一致）。
//
// v2 服务查询协议：context.config("state") 返回
//   {"goal_manager": <int64 地址>, "checkpoint_manager": <地址>,
//    "fs_checkpoint": <地址>}
// 核心在 AgentService 构造后注入。地址传递为整型（避免跨 .so 的
// C++ 接口依赖），插件侧 reinterpret_cast 恢复。

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

#include "thin_agent/agent/CheckpointManager.h"
#include "thin_agent/agent/FilesystemCheckpoint.h"
#include "thin_agent/agent/GoalManager.h"
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/plugin/PluginContext.h"

namespace thin_agent {
namespace state {

static agent::GoalManager* goal_mgr = nullptr;
static agent::CheckpointManager* ckpt_mgr = nullptr;
static agent::FilesystemCheckpoint* fs_ckpt = nullptr;

nlohmann::json handle_goal_add(const nlohmann::json& params) {
  if (!goal_mgr)
    return {{"success", false}, {"error", "goal manager not available"}};
  std::string desc = params.value("description", "");
  if (desc.empty())
    return {{"success", false}, {"error", "description required"}};
  auto g = goal_mgr->create(desc);
  return {{"success", true}, {"id", g.id}, {"description", g.description}};
}

nlohmann::json handle_goal_list(const nlohmann::json& params) {
  if (!goal_mgr)
    return {{"success", false}, {"error", "goal manager not available"}};
  std::string filter = params.value("status", "");
  auto goals = goal_mgr->list(filter);
  nlohmann::json arr = nlohmann::json::array();
  for (auto& g : goals) {
    arr.push_back({{"id", g.id}, {"description", g.description},
                   {"status", g.status}, {"progress_pct", g.progress_pct}});
  }
  return {{"success", true}, {"goals", arr}, {"count", goals.size()}};
}

nlohmann::json handle_goal_update(const nlohmann::json& params) {
  if (!goal_mgr)
    return {{"success", false}, {"error", "goal manager not available"}};
  int id = params.value("id", 0);
  if (id <= 0) return {{"success", false}, {"error", "id required"}};
  nlohmann::json fields = params.value("fields", nlohmann::json::object());
  return {{"success", goal_mgr->update(id, fields)}, {"id", id}};
}

nlohmann::json handle_ckpt_save(PluginContext& ctx,
                                const nlohmann::json& params) {
  if (!ckpt_mgr)
    return {{"success", false}, {"error", "checkpoint not available"}};
  std::string sid = params.value("session_id", "__llm__");
  std::string label = params.value("label", "manual");
  // 会话消息经 context 快照（原核心直接读 session_chat_memory_）
  std::vector<std::string> messages;
  for (const auto& m : ctx.session_snapshot(sid)) {
    if (m.is_object())
      messages.push_back(m.value("role", "?") + ": " +
                         m.value("content", "").substr(0, 500));
  }
  std::string ckpt_id = ckpt_mgr->save(sid, messages, {}, label);
  return {{"success", true}, {"checkpoint_id", ckpt_id}, {"label", label}};
}

nlohmann::json handle_ckpt_rollback(const nlohmann::json& params) {
  if (!ckpt_mgr)
    return {{"success", false}, {"error", "checkpoint not available"}};
  std::string sid = params.value("session_id", "__llm__");
  auto restored = ckpt_mgr->restore(sid);
  return {{"success", !restored.empty()}, {"message_count", restored.size()},
          {"session_id", sid}};
}

nlohmann::json handle_ckpt_list(const nlohmann::json& params) {
  if (!ckpt_mgr)
    return {{"success", false}, {"error", "checkpoint not available"}};
  std::string sid = params.value("session_id", "__llm__");
  auto snaps = ckpt_mgr->list_snapshots(sid);
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& s : snaps) {
    arr.push_back({{"session_id", s.session_id}, {"label", s.label},
                   {"timestamp", s.timestamp},
                   {"message_count", s.chat_memory.size()}});
  }
  return {{"success", true}, {"snapshots", arr}, {"count", snaps.size()}};
}

nlohmann::json handle_fs_save(const nlohmann::json& params) {
  if (!fs_ckpt)
    return {{"success", false},
            {"error", "filesystem checkpoint not available"}};
  std::string workdir = params.value("workdir", params.value("dir", "/tmp"));
  std::string label = params.value("label", "manual");
  std::string ckpt_id = fs_ckpt->save(workdir, {}, label);
  if (ckpt_id.empty())
    return {{"success", false}, {"error", "save failed"}};
  return {{"success", true}, {"checkpoint_id", ckpt_id}, {"label", label},
          {"workdir", workdir}};
}

nlohmann::json handle_fs_rollback(const nlohmann::json& params) {
  if (!fs_ckpt)
    return {{"success", false},
            {"error", "filesystem checkpoint not available"}};
  std::string ckpt_id = params.value("checkpoint_id", "");
  if (ckpt_id.empty())
    return {{"success", false}, {"error", "checkpoint_id required"}};
  bool dry_run = params.value("dry_run", false);
  auto result = fs_ckpt->rollback(ckpt_id, dry_run);
  return {{"success", result.ok},
          {"dry_run", dry_run},
          {"restored", result.restored.size()},
          {"skipped", result.skipped.size()},
          {"added", result.added.size()},
          {"restored_files", result.restored},
          {"added_files", result.added}};
}

nlohmann::json handle_fs_diff(const nlohmann::json& params) {
  if (!fs_ckpt)
    return {{"success", false},
            {"error", "filesystem checkpoint not available"}};
  std::string ckpt_id = params.value("checkpoint_id", "");
  if (ckpt_id.empty())
    return {{"success", false}, {"error", "checkpoint_id required"}};
  return {{"success", true}, {"diff", fs_ckpt->diff(ckpt_id)}};
}

nlohmann::json handle_fs_list(const nlohmann::json&) {
  if (!fs_ckpt)
    return {{"success", false},
            {"error", "filesystem checkpoint not available"}};
  return {{"success", true}, {"snapshots", fs_ckpt->stats()}};
}

}  // namespace state
}  // namespace thin_agent

extern "C" const char* thin_agent_plugin_init2(
    thin_agent::SkillRegistry& registry, thin_agent::PluginContext& ctx) {
  using namespace thin_agent;
  using namespace thin_agent::state;
  using thin_agent::agent::GoalManager;
  using thin_agent::agent::CheckpointManager;
  using thin_agent::agent::FilesystemCheckpoint;
  // 服务地址注入（核心构造后写入；未启用=0 → not available）
  auto svc = ctx.config("state");
  if (svc.is_object()) {
    auto p = svc.value("goal_manager", static_cast<int64_t>(0));
    if (p) goal_mgr = reinterpret_cast<GoalManager*>(p);
    p = svc.value("checkpoint_manager", static_cast<int64_t>(0));
    if (p) ckpt_mgr = reinterpret_cast<CheckpointManager*>(p);
    p = svc.value("fs_checkpoint", static_cast<int64_t>(0));
    if (p) fs_ckpt = reinterpret_cast<FilesystemCheckpoint*>(p);
  }

  registry.register_cpp_handler("goal_add", handle_goal_add);
  registry.register_cpp_handler("goal_list", handle_goal_list);
  registry.register_cpp_handler("goal_update", handle_goal_update);
  registry.register_cpp_handler(
      "checkpoint_save",
      [&ctx](const nlohmann::json& p) { return handle_ckpt_save(ctx, p); });
  registry.register_cpp_handler("checkpoint_rollback",
                                handle_ckpt_rollback);
  registry.register_cpp_handler("checkpoint_list", handle_ckpt_list);
  registry.register_cpp_handler("checkpoint_fs_save", handle_fs_save);
  registry.register_cpp_handler("checkpoint_fs_rollback",
                                handle_fs_rollback);
  registry.register_cpp_handler("checkpoint_fs_diff", handle_fs_diff);
  registry.register_cpp_handler("checkpoint_fs_list", handle_fs_list);
  return "state";
}
