// v0.53.1: patrol 插件 —— 巡检工具（patrol_now/patrol_status/patrol_config）。
//
// 从 AgentService::register_meta_memory_tools 迁出（核心瘦身第二批）。
// PatrolProbe 为零依赖叶子类（头文件 PatrolProbe.h），探针配置经
// ctx.config("patrol") 注入（enabled/interval/quiet_mode/probes）——
// 配置仍由核心 chat_policy.json 装配（场景配置留在配置层，插件只读）。

#include <nlohmann/json.hpp>

#include <string>

#include "thin_agent/core/PatrolProbe.h"
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/plugin/PluginContext.h"

namespace thin_agent {
namespace patrol {

static PatrolConfig g_cfg;

nlohmann::json handle_now(const nlohmann::json&) {
  auto results = PatrolProbe::run_all(g_cfg);
  nlohmann::json arr = nlohmann::json::array();
  bool has_warning = false;
  for (const auto& r : results) {
    if (r.warning) has_warning = true;
    arr.push_back({
      {"name", r.name}, {"ok", r.ok}, {"warning", r.warning},
      {"summary", r.summary}, {"data", r.data}
    });
  }
  return {
    {"success", true}, {"results", arr}, {"count", results.size()},
    {"has_warning", has_warning}, {"quiet_mode", g_cfg.quiet_mode}
  };
}

nlohmann::json handle_status(const nlohmann::json&) {
  nlohmann::json cfg;
  cfg["enabled"] = g_cfg.enabled;
  cfg["interval_min"] = g_cfg.cron_interval_min;
  cfg["quiet_mode"] = g_cfg.quiet_mode;
  cfg["disk_enabled"] = g_cfg.disk.enabled;
  cfg["disk_path"] = g_cfg.disk.path;
  cfg["disk_warn_pct"] = g_cfg.disk.warn_pct;
  cfg["memory_enabled"] = g_cfg.memory.enabled;
  cfg["memory_warn_pct"] = g_cfg.memory.warn_pct;
  cfg["process_enabled"] = g_cfg.process.enabled;
  cfg["process_names"] = g_cfg.process.names;
  cfg["log_errors_enabled"] = g_cfg.log_errors.enabled;
  cfg["log_errors_path"] = g_cfg.log_errors.log_path;
  cfg["log_errors_warn_count"] = g_cfg.log_errors.warn_count;
  return {{"success", true}, {"config", cfg}};
}

nlohmann::json handle_config(const nlohmann::json& params) {
  // 只读（原核心实现同款语义：提供参数则忽略）
  (void)params;
  return handle_status(params);
}

}  // namespace patrol
}  // namespace thin_agent

extern "C" const char* thin_agent_plugin_init2(
    thin_agent::SkillRegistry& registry, thin_agent::PluginContext& ctx) {
  // 配置注入（chat_policy.json patrol 节，核心装配后写入）
  auto cfg = ctx.config("patrol");
  if (cfg.is_object()) {
    auto& p = thin_agent::patrol::g_cfg;
    p.enabled = cfg.value("enabled", true);
    p.cron_interval_min = cfg.value("interval_min", 30);
    p.quiet_mode = cfg.value("quiet_mode", false);
    if (cfg.contains("disk")) {
      p.disk.enabled = cfg["disk"].value("enabled", true);
      p.disk.path = cfg["disk"].value("path", "/");
      p.disk.warn_pct = cfg["disk"].value("warn_pct", 90);
    }
    if (cfg.contains("memory")) {
      p.memory.enabled = cfg["memory"].value("enabled", true);
      p.memory.warn_pct = cfg["memory"].value("warn_pct", 90);
    }
    if (cfg.contains("process")) {
      p.process.enabled = cfg["process"].value("enabled", false);
      if (cfg["process"].contains("names"))
        for (const auto& n : cfg["process"]["names"])
          p.process.names.push_back(n.get<std::string>());
    }
    if (cfg.contains("log_errors")) {
      p.log_errors.enabled = cfg["log_errors"].value("enabled", false);
      p.log_errors.log_path = cfg["log_errors"].value("path", "");
      p.log_errors.warn_count = cfg["log_errors"].value("warn_count", 10);
    }
  }

  registry.register_cpp_handler("patrol_now", thin_agent::patrol::handle_now);
  registry.register_cpp_handler("patrol_status", thin_agent::patrol::handle_status);
  registry.register_cpp_handler("patrol_config", thin_agent::patrol::handle_config);
  return "patrol";
}
