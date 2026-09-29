#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "thin_agent/agent/ToolSchema.h"

namespace thin_agent {

// 前向声明
class AgentService;
class ActionExecutor;
class TaskEngine;
struct DemoConfigCompat;

class IHttpClient;

namespace agent {

/// 工具执行上下文：工具回调所需的运行时状态引用。
struct ToolContext {
  /// 当前会话 ID（部分工具需要，如 memory_recent）。
  std::string session_id;

  /// AgentService 指针（用于 status / memory / event 查询）。
  /// 为 nullptr 时对应工具不可用。
  AgentService* agent_service = nullptr;

  /// ActionExecutor 指针（用于 capture_photo / recording）。
  ActionExecutor* action_executor = nullptr;

  /// TaskEngine 指针（用于任务提交）。
  TaskEngine* task_engine = nullptr;

  /// 配置引用（用于 weather/news 的 provider 配置）。
  const DemoConfigCompat* config = nullptr;

  /// HTTP 客户端（注入，用于 weather/news 实际 HTTP 请求）。
  IHttpClient* http = nullptr;
};

/// 创建所有内置 Agent 工具的 Schema，绑定到指定上下文。
/// 返回的 ToolSchema.execute 通过 lambda 捕获了 context 中的指针。
///
/// 工具列表：
///   - status          : 查询 Agent 运行状态
///   - weather         : 查询指定城市天气
///   - news            : 查询新闻
///   - memory_recent   : 获取短期对话记忆
///   - memory_search   : 搜索长期记忆
///   - capture_photo   : 拍照
///   - start_recording : 开始录像
///   - stop_recording  : 停止录像
std::vector<ToolSchema> create_builtin_tools(const ToolContext& ctx);

}  // namespace agent
}  // namespace thin_agent
