#pragma once

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <vector>

namespace thin_agent {

/// 工作流步骤
struct WorkflowStep {
  std::string cmd;   // shell 命令
  std::string desc;  // 步骤描述
  int max_retries = 0;  // 0 = 继承 WorkflowDef.max_retries
};

/// 完整工作流定义
struct WorkflowDef {
  std::string name;                // 唯一名称
  std::string description;         // 人类可读描述
  std::string domain;              // 领域: programming, data-science, ops, ...
  int max_retries = 3;             // 全局默认重试次数（0 = 不重试）
  std::vector<WorkflowStep> post_code_steps;  // 代码编写后的执行步骤
};

/// 四层工作流管理系统
///
/// 优先级（从高到低）:
///   Layer 4: 会话覆盖  ~/.thin_agent/workflows/_session.json
///   Layer 3: 项目配置  <project>/.thin_agent/workflow.json
///   Layer 2: 用户预设  ~/.thin_agent/workflows/user/<name>.json
///   Layer 1: 内置默认  ~/.thin_agent/workflows/builtin/<name>.json
///
/// _current 符号链接指向当前激活的工作流（Layer 1 或 2）。
class WorkflowManager {
public:
  static WorkflowManager& instance();

  /// 列出所有可用工作流（builtin + user）
  std::vector<WorkflowDef> list_all();

  /// 查看指定工作流详情
  WorkflowDef view(const std::string& name);

  /// 创建/覆写用户工作流
  bool create(const WorkflowDef& wf);

  /// 切换当前工作流（更新 _current 链接）
  bool set_current(const std::string& name);

  /// 删除用户工作流
  bool remove(const std::string& name);

  /// 获取当前激活的工作流（按四层优先级）
  WorkflowDef current(const std::string& project_dir = "");

  /// 获取工作流目录
  static std::string workflows_dir();
  static std::string builtin_dir();
  static std::string user_dir();
  static std::string current_link();
  static std::string session_path();

  /// 将工作流步骤注入到 system prompt 中（替换第4步及之后的构建步骤）
  std::string inject_into_system_prompt(const std::string& prompt,
                                         const WorkflowDef& wf) const;

  /// 将工作流步骤注入到中文 self-introduction 文本中
  std::string inject_into_profile_zh(const std::string& body,
                                      const WorkflowDef& wf) const;

  /// 将工作流步骤注入到英文 self-introduction 文本中
  std::string inject_into_profile_en(const std::string& body,
                                      const WorkflowDef& wf) const;

  /// 执行工作流：按序运行步骤，失败时返回结构化错误（含 retry 状态）。
  /// 语言无关——只执行 cmd，不关心里面是 cmake / pytest / cargo / npm / …。
  /// @param workflow_name 工作流名
  /// @param retry         当前重试次数（从 1 开始，调用方自管理）
  /// @param project_dir   项目根目录（用于解析项目级 workflow.json）
  /// @return { status, step, error, retry, max_retries, ... }
  nlohmann::json run(const std::string& workflow_name,
                     int retry = 1,
                     const std::string& project_dir = "");

private:
  WorkflowManager() = default;
  WorkflowDef load(const std::string& path);
  bool save(const std::string& path, const WorkflowDef& wf);

  /// 从文件名推导 workflow name
  static std::string name_from_path(const std::string& path);

  /// 生成工作流步骤的 system prompt 文本（中英文）
  std::string steps_to_prompt_text(const WorkflowDef& wf, bool zh) const;
};

}  // namespace thin_agent
