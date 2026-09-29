// v0.53.1: collab 插件增补 —— 子代理协作消息面（agent_message/agent_inbox）。
//
// 从 AgentService 迁出（核心瘦身第二批）。SubAgentBus 实例留核心
//（spawn_agent 执行链 drain 注入深耦合），经 ctx.config("collab").bus
// 地址注入——与黑板同域同协议。

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/core/SubAgentBus.h"
#include "thin_agent/plugin/PluginContext.h"

namespace thin_agent {
namespace collab_msg {

static SubAgentBus* g_bus = nullptr;
/// v0.53.50: 注入方存活锚(与 collab/cron/meta/kanban 同款)
static thin_agent::PluginContext* g_bus_ctx = nullptr;

static bool bus_alive() {
  // v0.53.50: 注入方已亡视为不可用(悬垂 g_bus 防护)
  return g_bus && thin_agent::PluginContext::is_alive(g_bus_ctx);
}

nlohmann::json handle_agent_message(const nlohmann::json& params) {
  if (!bus_alive()) return {{"success", false}, {"error", "bus not available"}};
  std::string to = params.value("to", "");
  std::string msg = params.value("message", "");
  if (to.empty() || msg.empty())
    return {{"success", false}, {"error", "to and message required"}};
  g_bus->send("__llm__", to, msg);
  return {{"success", true}, {"from", "__llm__"}, {"to", to}};
}

nlohmann::json handle_agent_inbox(const nlohmann::json& params) {
  if (!bus_alive()) return {{"success", false}, {"error", "bus not available"}};
  std::string agent_id = params.value("agent_id", "");
  auto msgs = agent_id.empty() ? std::vector<std::string>{}
                               : g_bus->peek(agent_id);
  nlohmann::json arr = nlohmann::json::array();
  for (auto& m : msgs) arr.push_back(m);
  return {{"success", true}, {"agent_id", agent_id}, {"messages", arr},
          {"count", msgs.size()}};
}

}  // namespace collab_msg
}  // namespace thin_agent

extern "C" const char* thin_agent_plugin_init2(
    thin_agent::SkillRegistry& registry, thin_agent::PluginContext& ctx) {
  auto svc = ctx.config("collab");
  if (svc.is_object()) {
    auto p = svc.value("bus", static_cast<int64_t>(0));
    if (p) {
      thin_agent::collab_msg::g_bus_ctx = &ctx;  // v0.53.50: 存活锚
    }
    if (p) thin_agent::collab_msg::g_bus =
        reinterpret_cast<thin_agent::SubAgentBus*>(p);
  }
  registry.register_cpp_handler("agent_message",
                                thin_agent::collab_msg::handle_agent_message);
  registry.register_cpp_handler("agent_inbox",
                                thin_agent::collab_msg::handle_agent_inbox);
  return "collab_msg";
}
