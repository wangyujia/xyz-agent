#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace thin_agent {

class SkillRegistry;

namespace agent {

/// MCP Server：将 thin_agent 的工具暴露为 MCP 协议服务。
///
/// 支持两种模式：
///   - stdio: 从 stdin 读取 JSON-RPC 请求，写到 stdout（供外部进程调用）
///   - callback: 注册回调，每次收到请求时调用（供嵌入使用）
///
/// 协议：JSON-RPC 2.0 over stdio/callback
/// 方法：initialize, tools/list, tools/call
class McpServer {
 public:
  /// 请求处理回调：收到 JSON-RPC 请求 → 返回 JSON-RPC 响应。
  using RequestHandler = std::function<std::string(const std::string& json_rpc_request)>;

  /// @param skill_registry 工具注册表（提供 tools/list + tools/call 的后端）
  explicit McpServer(SkillRegistry& skill_registry);

  /// 启动 stdio 模式（阻塞，从 stdin 读，写到 stdout）。
  void run_stdio();

  /// 注册回调模式的处理函数。
  void set_handler(RequestHandler handler) { handler_ = std::move(handler); }

  /// 处理单条 JSON-RPC 请求（供 callback 模式使用）。
  std::string handle_request(const std::string& json_rpc_str);

  /// 设置服务器信息。
  void set_server_info(const std::string& name, const std::string& version);

 private:
  std::string handle_initialize(const nlohmann::json& req);
  std::string handle_list_tools(const nlohmann::json& req);
  std::string handle_call_tool(const nlohmann::json& req);

  /// v0.53.92: id 改为**原样透传 nlohmann::json**——JSON-RPC 2.0 允许 id 为字符串或数字，
  /// 此前用 `int id = req.value("id", 0)`：字符串 id 会抛 type_error（→ 客户端拿到
  /// -32603 + id=null，会话彻底错位），数字 id 之外的类型也一律退化成 0。
  static nlohmann::json id_of(const nlohmann::json& req);
  std::string make_response(const nlohmann::json& id, const nlohmann::json& result);
  std::string make_error(const nlohmann::json& id, int code, const std::string& message);

  SkillRegistry& skill_registry_;
  std::string server_name_ = "thin_agent";
  std::string server_version_ = "1.0.0";
  bool initialized_ = false;
  RequestHandler handler_;
};

}  // namespace agent
}  // namespace thin_agent
