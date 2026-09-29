/// libskill_workflow.so — 工作流管理插件
///
/// 注册的 handler:
///   workflow_list   — 列出所有可用工作流
///   workflow_view   — 查看指定工作流详情
///   workflow_create — 创建新工作流
///   workflow_set    — 切换到指定工作流
///   workflow_delete — 删除工作流
///   workflow_run    — 执行工作流步骤（含自动重试）
///
/// 仅在 --dev 模式下加载。所有 handler 返回统一 JSON 格式:
///   { "success": true/false, "output": "...", "error": "..." }

#include "thin_agent/plugin/PluginInterface.h"
#include "thin_agent/core/WorkflowManager.h"

#include <string>

namespace thin_agent {
namespace workflow {

using json = nlohmann::json;

// ── Handler: workflow_list ──
// 列出所有工作流，标注当前激活的
    json handle_workflow_list(const json&) {
  auto wfs = thin_agent::WorkflowManager::instance().list_all();
  json out;
  out["success"] = true;
  out["workflows"] = json::array();
  for (const auto& w : wfs) {
    out["workflows"].push_back({
      {"name", w.name},
      {"description", w.description},
      {"domain", w.domain},
      {"steps", w.post_code_steps.size()}
    });
  }
  auto cur = thin_agent::WorkflowManager::instance().current();
  out["current"] = cur.name;
  out["output"] = "Found " + std::to_string(wfs.size()) +
                  " workflows, current: " + cur.name;
  return out;
}

// ── Handler: workflow_view ──
// 查看指定工作流的详细信息
    json handle_workflow_view(const json& params) {
  std::string name = params.value("name", "");
  if (name.empty())
    return {{"success", false}, {"error", "name_required"}};

  auto wf = thin_agent::WorkflowManager::instance().view(name);
  if (wf.name.empty())
    return {{"success", false}, {"error", "not_found: " + name}};

  json out;
  out["success"] = true;
  out["name"] = wf.name;
  out["description"] = wf.description;
  out["domain"] = wf.domain;
  out["steps"] = json::array();
  for (const auto& s : wf.post_code_steps) {
    out["steps"].push_back({{"desc", s.desc}, {"cmd", s.cmd}});
  }

  std::string output = "Workflow: " + wf.name +
                       "\nDescription: " + wf.description +
                       "\nSteps:\n";
  for (size_t i = 0; i < wf.post_code_steps.size(); ++i) {
    output += std::to_string(i + 1) + ". " +
              wf.post_code_steps[i].desc + ": " +
              wf.post_code_steps[i].cmd + "\n";
  }
  out["output"] = output;
  return out;
}

// ── Handler: workflow_create ──
// 创建新的用户工作流
    json handle_workflow_create(const json& params) {
  thin_agent::WorkflowDef wf;
  wf.name = params.value("name", "");
  wf.description = params.value("description", "");
  wf.domain = params.value("domain", "programming");

  if (wf.name.empty())
    return {{"success", false}, {"error", "name_required"}};

  if (params.contains("steps") && params["steps"].is_array()) {
    for (const auto& s : params["steps"]) {
      thin_agent::WorkflowStep step;
      step.desc = s.value("desc", "");
      step.cmd  = s.value("cmd", "");
      if (!step.cmd.empty()) wf.post_code_steps.push_back(step);
    }
  }

  if (wf.post_code_steps.empty())
    return {{"success", false}, {"error", "steps_required"}};

  bool ok = thin_agent::WorkflowManager::instance().create(wf);
  return {
    {"success", ok},
    {"output", ok ? "Workflow " + wf.name + " created"
                  : "Failed to create workflow"}
  };
}

// ── Handler: workflow_set ──
// 切换当前激活的工作流
    json handle_workflow_set(const json& params) {
  std::string name = params.value("name", "");
  if (name.empty())
    return {{"success", false}, {"error", "name_required"}};

  bool ok = thin_agent::WorkflowManager::instance().set_current(name);
  return {
    {"success", ok},
    {"output", ok ? "Switched to workflow: " + name
                  : "Switch failed: " + name + " not found"}
  };
}

// ── Handler: workflow_delete ──
// 删除用户工作流
    json handle_workflow_delete(const json& params) {
  std::string name = params.value("name", "");
  if (name.empty())
    return {{"success", false}, {"error", "name_required"}};

  bool ok = thin_agent::WorkflowManager::instance().remove(name);
  return {
    {"success", ok},
    {"output", ok ? "Workflow " + name + " deleted"
                  : "Failed to delete workflow"}
  };
}

// ── Handler: workflow_run ──
// 执行工作流步骤。语言无关: cmd 可以是 cmake / pytest / cargo / npm / …
// 失败时返回结构化错误(含 retry 计数), 驱动 LLM 自动重试。
    json handle_workflow_run(const json& params) {
  std::string name = params.value("name", "");
  int retry = params.value("retry", 1);
  std::string project_dir = params.value("project_dir", "");

  if (name.empty())
    return {{"success", false}, {"error", "name_required"}};

  return thin_agent::WorkflowManager::instance().run(name, retry, project_dir);
}

}  // namespace workflow
}  // namespace thin_agent

// ── 插件入口 ──

extern "C" const char* thin_agent_plugin_init(
    thin_agent::SkillRegistry& registry) {

  registry.register_cpp_handler("workflow_list",   thin_agent::workflow::handle_workflow_list);
  registry.register_cpp_handler("workflow_view",   thin_agent::workflow::handle_workflow_view);
  registry.register_cpp_handler("workflow_create", thin_agent::workflow::handle_workflow_create);
  registry.register_cpp_handler("workflow_set",    thin_agent::workflow::handle_workflow_set);
  registry.register_cpp_handler("workflow_delete", thin_agent::workflow::handle_workflow_delete);
  registry.register_cpp_handler("workflow_run",    thin_agent::workflow::handle_workflow_run);

  return "workflow";
}
