// v0.53.0: cron 插件 —— 定时任务工具（cron_add/list/remove/stats/
// update/pause/resume/trigger）。
//
// 从 AgentService 迁出。CronScheduler 类随迁本插件（自包含 SQLite
// 调度）；核心的"回调→chat 管线"接线经 PluginContext.chat 重实现
//（原实现：cron_scheduler_->start(db, [this]{...handle_request...})，
// 插件侧同语义：ctx.chat(session, prompt, task_meta)）。
// patrol 定时任务的注册（init_cron）属场景装配，由本插件的
// patrol 配置节驱动（enabled 时 add_task patrol_probe）。
//
// 注意：v0.52.x 的 cron 任务字段（no_agent/deliver_to/workdir/model/
// provider/context_from/skills/toolsets）全量保留——字段语义在
// CronScheduler 类内，类整体随迁即继承。

#include <nlohmann/json.hpp>

#include <string>

#include "thin_agent/core/CronScheduler.h"
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/plugin/PluginContext.h"

namespace thin_agent {
namespace cron {

static CronScheduler& sched() {
  static CronScheduler inst;
  static bool stop_registered = false;  // v0.53.3: 一次性注册退出停机
  if (!stop_registered) {
    stop_registered = true;
    // 静态实例析构序不受控（可能晚于 main 后线程仍 joinable）——
    // atexit 先 stop ticker，避免 std::thread::~thread terminate
    //（生产实证：test_agent_service 第二实例加载 cron 插件后 abort）
    std::atexit([] { inst.stop(); });
  }
  return inst;
}

static PluginContext* g_ctx = nullptr;
static std::string g_data_dir;

nlohmann::json handle_add(const nlohmann::json& params) {
  std::string name = params.value("name", "");
  std::string schedule = params.value("schedule", "");
  std::string prompt = params.value("prompt", "");
  if (name.empty() || schedule.empty())
    return {{"success", false}, {"error", "name and schedule required"}};
  auto task = sched().add_task_full(
      name, schedule, prompt,
      params.value("enabled", true),
      params.value("max_repeat", 0),
      params.value("no_agent", false),
      params.value("no_agent_script", ""),
      params.value("deliver_to", "chat"),
      params.value("workdir", ""),
      params.value("model", ""),
      params.value("provider", ""),
      params.value("context_from", 0),
      params.value("skills", ""),
      params.value("toolsets", ""));
  return {{"success", true}, {"task", task}};
}

nlohmann::json handle_list(const nlohmann::json&) {
  return {{"success", true}, {"tasks", sched().list_tasks()}};
}

nlohmann::json handle_remove(const nlohmann::json& params) {
  int task_id = params.value("task_id", 0);
  if (task_id <= 0)
    return {{"success", false}, {"error", "task_id required"}};
  bool ok = sched().remove_task(task_id);
  return {{"success", ok}, {"task_id", task_id}};
}

nlohmann::json handle_stats(const nlohmann::json&) {
  return {{"success", true}, {"stats", sched().stats()}};
}

nlohmann::json handle_update(const nlohmann::json& params) {
  int task_id = params.value("task_id", 0);
  if (task_id <= 0)
    return {{"success", false}, {"error", "task_id required"}};
  nlohmann::json fields;
  for (const auto& k : {"name", "schedule", "prompt", "enabled",
                        "max_repeat", "no_agent", "no_agent_script",
                        "deliver_to", "workdir", "model", "provider",
                        "context_from", "skills", "toolsets"}) {
    if (params.contains(k)) fields[k] = params[k];
  }
  bool ok = sched().update_task(task_id, fields);
  return {{"success", ok}, {"task_id", task_id}};
}

nlohmann::json handle_pause(const nlohmann::json& params) {
  int task_id = params.value("task_id", 0);
  if (task_id <= 0)
    return {{"success", false}, {"error", "task_id required"}};
  bool ok = sched().set_enabled(task_id, false);
  return {{"success", ok}, {"task_id", task_id}, {"paused", true}};
}

nlohmann::json handle_resume(const nlohmann::json& params) {
  int task_id = params.value("task_id", 0);
  if (task_id <= 0)
    return {{"success", false}, {"error", "task_id required"}};
  bool ok = sched().set_enabled(task_id, true);
  return {{"success", ok}, {"task_id", task_id}, {"resumed", true}};
}

nlohmann::json handle_trigger(const nlohmann::json& params) {
  int task_id = params.value("task_id", 0);
  if (task_id <= 0)
    return {{"success", false}, {"error", "task_id required"}};
  bool ok = sched().trigger_now(task_id);
  return {{"success", ok}, {"task_id", task_id}};
}

}  // namespace cron
}  // namespace thin_agent

extern "C" const char* thin_agent_plugin_init2(
    thin_agent::SkillRegistry& registry, thin_agent::PluginContext& ctx) {
  using namespace thin_agent::cron;
  g_ctx = &ctx;
  g_data_dir = ctx.data_dir("cron");
  // 调度启动：db 落插件数据目录；回调经 context 驱动 chat 管线
  //（v0.52.26 断点续跑语义保留：running→interrupted 清扫在类内）。
  sched().start(g_data_dir + "/cron.db",
                [](const nlohmann::json& task) {
                  // v0.53.47: 静态存活探测——虚调用 alive() 在 g_ctx
                  /// 悬垂时读虚表即 UB(多实例测试实测 SegFault 于此行);
                  /// is_alive 只做注册表键查询,悬垂安全
                  if (!thin_agent::PluginContext::is_alive(g_ctx)) return;
                  std::string name = task.value("name", "unnamed");
                  std::string prompt = task.value("prompt", "");
                  if (prompt.empty()) return;
                  auto result = g_ctx->chat("", prompt, task);
                  (void)result;
                  if (true) g_ctx->log("cron", "info", "task fired",
                                       {{"name", name}});
                });

  // v0.53.1: patrol 定时任务装配（原 init_cron 场景接线）——必须在
  // start 之后：start 前 db_=null，add_task 静默返回空 JSON（e2e 踩坑）
  {
    auto pcfg = ctx.config("cron");
    if (pcfg.is_object() && pcfg.value("patrol_enabled", false)) {
      int interval = pcfg.value("patrol_interval_min", 30);
      // 幂等：重启不重复注册（add_task 同名会插新行——先查重）
      bool exists = false;
      for (const auto& t : sched().list_tasks())
        if (t.value("name", "") == "patrol_probe") { exists = true; break; }
      if (!exists)
        sched().add_task("patrol_probe",
                         "every " + std::to_string(interval) + "m",
                         "RUN_PATROL_PROBE", true);
    }
  }

  registry.register_cpp_handler("cron_add", handle_add);
  registry.register_cpp_handler("cron_list", handle_list);
  registry.register_cpp_handler("cron_remove", handle_remove);
  registry.register_cpp_handler("cron_stats", handle_stats);
  registry.register_cpp_handler("cron_update", handle_update);
  registry.register_cpp_handler("cron_pause", handle_pause);
  registry.register_cpp_handler("cron_resume", handle_resume);
  registry.register_cpp_handler("cron_trigger", handle_trigger);
  return "cron";
}
