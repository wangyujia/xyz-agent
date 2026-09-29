// v0.53.0: kanban 插件 —— 看板工具（kanban_push/status + WS 域 push_batch/run/clear）。
//
// 从 AgentService 迁出。KanbanBoard 类（零依赖）随迁；kanban_run 的
// "拉任务→spawn_agent→完成"循环经 PluginContext.chat 驱动（原实现
// handle_request(spawn_agent)，插件侧改为 context.chat 同语义）。
// 核心 AgentService 的 kanban_ 成员与 WS 分发点同步删除（见
// AgentService 断链提交）。

#include <nlohmann/json.hpp>

#include <atomic>
#include <future>
#include <string>
#include <vector>

#include "thin_agent/core/KanbanBoard.h"
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/plugin/PluginContext.h"

namespace thin_agent {
namespace kanban {

/// 核心看板实例（经 ctx.config("kanban").board 注入——WS 层 kanban_run
/// 等核心直用成员的路径共享同一实例；未注入时退化自建）。
static KanbanBoard* g_board = nullptr;
/// v0.53.50: 注入方存活锚(与 collab/cron/meta 同款,防多实例悬垂)
static thin_agent::PluginContext* g_kanban_ctx = nullptr;

static KanbanBoard& board() {
  static KanbanBoard fallback;
  if (g_board && !thin_agent::PluginContext::is_alive(g_kanban_ctx)) {
    g_board = nullptr;  // v0.53.50: 注入方已亡回落 fallback
  }
  return g_board ? *g_board : fallback;
}

static std::atomic<uint64_t> g_next_id{1};

nlohmann::json handle_push(const nlohmann::json& params) {
  std::string desc = params.value("description", params.value("desc", ""));
  if (desc.empty())
    return {{"success", false}, {"error", "description required"}};
  KanbanTask t;
  t.task_id = "kb_" + std::to_string(g_next_id.fetch_add(1));
  t.name = params.value("name", t.task_id);
  t.prompt = desc;
  t.role = params.value("role", "");
  t.status = "pending";  // v0.53.0: 修存量 bug——push 不设 status 致 pull/status 全盲
  board().push(t);
  return {{"success", true}, {"task_id", t.task_id},
          {"pending", board().pending_count()}};
}

nlohmann::json handle_status(const nlohmann::json&) {
  return {{"success", true}, {"board", board().status()}};
}

nlohmann::json handle_push_batch(const nlohmann::json& params) {
  auto tasks_json = params.value("tasks", nlohmann::json::array());
  std::vector<KanbanTask> batch;
  for (auto& tj : tasks_json) {
    KanbanTask t;
    t.task_id = "kb_" + std::to_string(g_next_id.fetch_add(1));
    t.name = tj.value("name", t.task_id);
    t.prompt = tj.value("prompt", "");
    t.role = tj.value("role", "researcher");
    t.status = "pending";  // v0.53.0: 修存量 bug（同 handle_push）
    if (!t.prompt.empty()) batch.push_back(std::move(t));
  }
  board().push_batch(batch);
  return {{"success", true}, {"count", batch.size()},
          {"pending", board().pending_count()}};
}

nlohmann::json handle_clear(const nlohmann::json&) {
  board().clear();
  return {{"success", true}, {"ok", true}};
}

}  // namespace kanban
}  // namespace thin_agent

extern "C" const char* thin_agent_plugin_init2(
    thin_agent::SkillRegistry& registry, thin_agent::PluginContext& ctx) {
  auto svc = ctx.config("kanban");
  if (svc.is_object()) {
    auto p = svc.value("board", static_cast<int64_t>(0));
    if (p) {
      thin_agent::kanban::g_board =
          reinterpret_cast<thin_agent::KanbanBoard*>(p);
      thin_agent::kanban::g_kanban_ctx = &ctx;  // v0.53.50: 存活锚
    }
  }
  registry.register_cpp_handler("kanban_push", thin_agent::kanban::handle_push);
  registry.register_cpp_handler("kanban_push_batch",
                                thin_agent::kanban::handle_push_batch);
  registry.register_cpp_handler("kanban_status",
                                thin_agent::kanban::handle_status);
  registry.register_cpp_handler("kanban_clear",
                                thin_agent::kanban::handle_clear);
  return "kanban";
}
