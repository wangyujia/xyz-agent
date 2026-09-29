#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {
namespace agent {

// 前向声明
class ITransport;

/// MCP (Model Context Protocol) 客户端。
///
/// 通过注入 ITransport 实现解耦：
///   - 生产环境：注入 StdioTransport 或 HttpTransport
///   - 测试环境：注入 MockTransport
///
/// 协议参考：https://spec.modelcontextprotocol.io/
class McpClient {
 public:
  /// MCP 工具描述（与 ToolSchema 可互转）。
  struct McpTool {
    std::string name;
    std::string description;
    nlohmann::json input_schema;  ///< JSON Schema of parameters
  };

  /// @param transport 传输实现（生命周期由调用方管理）
  /// @param endpoint  连接端点（stdio: 命令路径; http: URL）
  McpClient(ITransport& transport, const std::string& endpoint);
  ~McpClient();

  /// 初始化连接并握手。
  bool connect(const std::string& client_name = "thin_agent",
               const std::string& client_version = "1.0.0");

  /// 获取服务器提供的工具列表。
  std::vector<McpTool> list_tools();

  /// 调用工具。
  nlohmann::json call_tool(const std::string& name,
                           const nlohmann::json& arguments);

  /// 是否已连接。
  bool is_connected() const { return connected_; }

 private:
  ITransport& transport_;
  std::string endpoint_;
  bool connected_ = false;
};

/// 将 MCP 工具列表转换为 ToolSchema 列表，注册到 ToolRegistry。
void register_mcp_tools(McpClient& client,
                        const std::string& prefix = "mcp_");

/// 构建 JSON-RPC 2.0 请求字符串（不发送）。
std::string build_jsonrpc_request(const std::string& method,
                                  const nlohmann::json& params,
                                  int id);

/// 解析 tools/list 的 JSON-RPC 响应，提取工具列表。
std::vector<McpClient::McpTool> parse_tool_list_response(
    const std::string& json_response);

/// 解析 tools/call 的 JSON-RPC 响应。
nlohmann::json parse_tool_call_response(const std::string& json_response);

}  // namespace agent
}  // namespace thin_agent
