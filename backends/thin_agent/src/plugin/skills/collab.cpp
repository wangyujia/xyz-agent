// v0.53.0: collab 插件 —— 多 Agent 协作存储面（黑板 bb_* + 子代理消息收件箱 agent_inbox）。
//
// 从 AgentService 迁出。Blackboard 类（零依赖叶子类）随迁本插件；
// SubAgentBus 留核心（spawn_agent 执行链深耦合）——agent_inbox 经
// PluginContext 扩展位查询。bb_read/bb_write/agent_inbox 三个 handler。
// 黑板的"spawn 前注入"属引擎行为，留核心（经 service_query）。

#include <nlohmann/json.hpp>

#include "thin_agent/core/Blackboard.h"
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/plugin/PluginContext.h"

namespace thin_agent {
namespace collab {

/// 核心黑板实例指针（经 ctx.config("collab").blackboard 注入——与
/// spawn_agent 的 prompt 注入共享同一实例；未注入时插件自建退化实例）。
static Blackboard* g_board = nullptr;
/// v0.53.49: 注入方 ctx 存活锚——多实例场景 svc1 析构后 g_board 悬垂
///（与 cron 插件 g_ctx 同款,Z1 修复模式),注册表键查询防 UAF
static thin_agent::PluginContext* g_board_ctx = nullptr;

static Blackboard& board() {
  static Blackboard fallback;  // 未注入（插件独立运行/测试）时退化
  // v0.53.49: 注入方已亡则回落 fallback(悬垂 g_board 不可用)
  if (g_board && !thin_agent::PluginContext::is_alive(g_board_ctx)) {
    g_board = nullptr;
  }
  return g_board ? *g_board : fallback;
}

nlohmann::json handle_bb_write(const nlohmann::json& params) {
  std::string key = params.value("key", "");
  if (key.empty()) return {{"success", false}, {"error", "key required"}};
  board().write(key, params.contains("value") ? params["value"]
                                              : nlohmann::json());
  return {{"success", true}, {"key", key}};
}

nlohmann::json handle_bb_read(const nlohmann::json& params) {
  std::string key = params.value("key", "");
  if (key.empty()) return {{"success", false}, {"error", "key required"}};
  auto val = board().read(key);
  return {{"success", true}, {"key", key}, {"value", val},
          {"found", !val.is_null()}};
}

}  // namespace collab
}  // namespace thin_agent

extern "C" const char* thin_agent_plugin_init2(
    thin_agent::SkillRegistry& registry, thin_agent::PluginContext& ctx) {
  auto svc = ctx.config("collab");
  if (svc.is_object()) {
    auto p = svc.value("blackboard", static_cast<int64_t>(0));
    if (p) {
      thin_agent::collab::g_board =
          reinterpret_cast<thin_agent::Blackboard*>(p);
      thin_agent::collab::g_board_ctx = &ctx;  // v0.53.49: 存活锚
    }
  }
  registry.register_cpp_handler("bb_write", thin_agent::collab::handle_bb_write);
  registry.register_cpp_handler("bb_read", thin_agent::collab::handle_bb_read);
  return "collab";
}
