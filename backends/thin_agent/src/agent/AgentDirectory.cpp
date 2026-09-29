#include "thin_agent/agent/AgentDirectory.h"

#include "thin_agent/agent/ToolRegistry.h"
#include "thin_agent/llm/IHttpClient.h"

namespace thin_agent {
namespace agent {

AgentDirectory& AgentDirectory::instance() {
  static AgentDirectory dir;
  return dir;
}

void AgentDirectory::register_agent(const AgentInfo& info) {
  agents_[info.id] = info;
}

void AgentDirectory::unregister_agent(const std::string& id) {
  agents_.erase(id);
}

const AgentInfo* AgentDirectory::find(const std::string& id) const {
  auto it = agents_.find(id);
  return it != agents_.end() ? &it->second : nullptr;
}

std::vector<AgentInfo> AgentDirectory::list() const {
  std::vector<AgentInfo> out;
  for (const auto& [_, info] : agents_) out.push_back(info);
  return out;
}

std::vector<AgentInfo> AgentDirectory::find_by_capability(
    const std::string& cap) const {
  std::vector<AgentInfo> out;
  for (const auto& [_, info] : agents_) {
    for (const auto& c : info.capabilities) {
      if (c == cap) { out.push_back(info); break; }
    }
  }
  return out;
}

void register_delegate_tool(IHttpClient& http, const std::string& platform) {
  ToolSchema ts;
  ts.name = "delegate_to_agent";
  ts.description =
      "将任务委托给其他 Agent 执行。可指定目标 agent_id 或能力标签，"
      "Agent 目录会自动路由到匹配的远程 Agent。";
  ts.parameters = {
      ToolParameter{"task", "string", "要委托的任务描述", true},
      ToolParameter{"agent_id", "string", "目标 Agent ID（可选）", false},
      ToolParameter{"capability", "string", "按能力标签匹配 Agent（可选）", false},
  };
  ts.dangerous = false;

  ts.execute = [&http, platform](const nlohmann::json& params) -> nlohmann::json {
    std::string task = params.value("task", "");
    std::string agent_id = params.value("agent_id", "");
    std::string capability = params.value("capability", "");

    const AgentInfo* target = nullptr;
    if (!agent_id.empty()) {
      target = AgentDirectory::instance().find(agent_id);
    } else if (!capability.empty()) {
      auto matches = AgentDirectory::instance().find_by_capability(capability);
      if (!matches.empty()) target = &matches[0];
    }

    if (!target) {
      return {{"ok", false},
               {"error", "no matching agent found"}};
    }

    if (platform == "http") {
      nlohmann::json payload;
      payload["task"] = task;
      payload["from"] = "thin_agent";

      auto resp = http.post(target->endpoint, payload.dump(), "");

      if (!resp.ok || resp.status_code >= 400) {
        return {{"ok", false},
                 {"error", "delegate request failed, HTTP " + std::to_string(resp.status_code)}};
      }

      try {
        return nlohmann::json::parse(resp.body);
      } catch (...) {
        return {{"ok", true}, {"result", {{"response", resp.body}}}};
      }
    }

    return {{"ok", true},
             {"result",
              {{"delegated_to", target->name},
               {"agent_id", target->id},
               {"message", "task delegated via " + platform}}}};
  };

  ToolRegistry::instance().register_tool(std::move(ts));
}

}  // namespace agent
}  // namespace thin_agent
