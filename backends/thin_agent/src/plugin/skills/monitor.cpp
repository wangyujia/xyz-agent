// v0.53.0: monitor 插件 —— 主动监控工具（monitor_watch_file/start/stop/status）。
//
// 从 AgentService 迁出。ProactiveMonitor 类随迁本插件（自包含告警
// 轮询）；告警回调经 PluginContext.broadcast 推送（原核心 on_alert
// lambda → broadcast_json，插件侧同语义）。
// 核心 proactive_monitor_ 成员与 on_alert 接线删除（断链提交）。

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "thin_agent/core/ProactiveMonitor.h"
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/plugin/PluginContext.h"

namespace thin_agent {
namespace monitor {

/// 核心监控实例（经 ctx.config("monitor").monitor 注入——核心
/// on_alert 接线/自动启动共享同一实例；未注入时退化自建）。
static ProactiveMonitor* g_mon = nullptr;

static ProactiveMonitor& mon() {
  static ProactiveMonitor fallback;
  return g_mon ? *g_mon : fallback;
}

nlohmann::json handle_watch_file(const nlohmann::json& params) {
  std::string path = params.value("path", "");
  if (path.empty())
    return {{"success", false}, {"error", "path required"}};
  std::vector<std::string> patterns;
  if (params.contains("patterns") && params["patterns"].is_array())
    for (const auto& p : params["patterns"])
      patterns.push_back(p.get<std::string>());
  mon().watch_file(path, patterns);
  return {{"success", true}, {"path", path}, {"patterns", patterns}};
}

nlohmann::json handle_start(const nlohmann::json& params) {
  mon().start(params.value("interval_ms", 5000));
  return {{"success", true}, {"status", mon().status()}};
}

nlohmann::json handle_stop(const nlohmann::json&) {
  mon().stop();
  return {{"success", true}, {"message", "monitor stopped"}};
}

nlohmann::json handle_status(const nlohmann::json&) {
  auto alerts = mon().poll_once();
  return {{"success", true}, {"status", mon().status()}, {"alerts", alerts}};
}

}  // namespace monitor
}  // namespace thin_agent

extern "C" const char* thin_agent_plugin_init2(
    thin_agent::SkillRegistry& registry, thin_agent::PluginContext& ctx) {
  auto svc = ctx.config("monitor");
  if (svc.is_object()) {
    auto p = svc.value("monitor", static_cast<int64_t>(0));
    if (p) thin_agent::monitor::g_mon =
        reinterpret_cast<thin_agent::ProactiveMonitor*>(p);
  }
  registry.register_cpp_handler("monitor_watch_file",
                                thin_agent::monitor::handle_watch_file);
  registry.register_cpp_handler("monitor_start",
                                thin_agent::monitor::handle_start);
  registry.register_cpp_handler("monitor_stop",
                                thin_agent::monitor::handle_stop);
  registry.register_cpp_handler("monitor_status",
                                thin_agent::monitor::handle_status);
  return "monitor";
}
