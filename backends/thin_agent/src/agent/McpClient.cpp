#include "thin_agent/agent/McpClient.h"

#include "thin_agent/agent/ITransport.h"
#include "thin_agent/agent/ToolRegistry.h"

namespace thin_agent {
namespace agent {

// ── McpClient ────────────────────────────────────────────────────

McpClient::McpClient(ITransport& transport, const std::string& endpoint)
    : transport_(transport), endpoint_(endpoint) {}

McpClient::~McpClient() {
  transport_.disconnect();
}

bool McpClient::connect(const std::string& client_name,
                        const std::string& client_version) {
  if (!transport_.connect(endpoint_)) return false;

  // 握手
  nlohmann::json init_params;
  init_params["protocolVersion"] = "2024-11-05";
  init_params["capabilities"] = nlohmann::json::object();
  init_params["clientInfo"]["name"] = client_name;
  init_params["clientInfo"]["version"] = client_version;

  auto req = build_jsonrpc_request("initialize", init_params, 1);
  auto resp_str = transport_.send(req);
  try {
    auto resp = nlohmann::json::parse(resp_str);
    if (!resp.contains("result")) return false;
  } catch (...) {
    return false;
  }

  // 发送 initialized 通知
  auto notif = build_jsonrpc_request("notifications/initialized", nullptr, 0);
  transport_.send(notif);  // 忽略响应

  connected_ = true;
  return true;
}

std::vector<McpClient::McpTool> McpClient::list_tools() {
  if (!connected_) return {};

  auto req = build_jsonrpc_request("tools/list", nullptr, 2);
  auto resp_str = transport_.send(req);
  return parse_tool_list_response(resp_str);
}

nlohmann::json McpClient::call_tool(const std::string& name,
                                     const nlohmann::json& arguments) {
  if (!connected_) {
    return {{"ok", false}, {"error", "not connected"}};
  }

  nlohmann::json params;
  params["name"] = name;
  params["arguments"] = arguments;

  auto req = build_jsonrpc_request("tools/call", params, 3);
  auto resp_str = transport_.send(req);
  return parse_tool_call_response(resp_str);
}

// ── 注册 MCP 工具到 ToolRegistry ──────────────────────────────────

void register_mcp_tools(McpClient& client, const std::string& prefix) {
  auto mcp_tools = client.list_tools();

  for (const auto& mt : mcp_tools) {
    ToolSchema ts;
    ts.name = prefix + mt.name;
    ts.description = "[MCP] " + mt.description;

    if (mt.input_schema.contains("properties") &&
        mt.input_schema["properties"].is_object()) {
      const auto& props = mt.input_schema["properties"];
      const auto& required =
          mt.input_schema.value("required", nlohmann::json::array());

      for (auto it = props.begin(); it != props.end(); ++it) {
        ToolParameter p;
        p.name = it.key();
        p.type = it.value().value("type", "string");
        p.description = it.value().value("description", "");
        p.required = false;
        for (const auto& r : required) {
          if (r.is_string() && r.get<std::string>() == p.name) {
            p.required = true;
            break;
          }
        }
        ts.parameters.push_back(p);
      }
    }

    std::string tool_name = mt.name;
    ts.execute = [&client, tool_name](const nlohmann::json& params) {
      return client.call_tool(tool_name, params);
    };

    ToolRegistry::instance().register_tool(std::move(ts));
  }
}

// ══════════════════════════════════════════════════════════════════
// 可测试的纯函数
// ══════════════════════════════════════════════════════════════════

std::string build_jsonrpc_request(const std::string& method,
                                  const nlohmann::json& params,
                                  int id) {
  nlohmann::json req;
  req["jsonrpc"] = "2.0";
  req["method"] = method;
  req["id"] = id;
  if (!params.is_null()) req["params"] = params;
  return req.dump() + "\n";
}

std::vector<McpClient::McpTool> parse_tool_list_response(
    const std::string& json_response) {
  std::vector<McpClient::McpTool> tools;
  try {
    auto resp = nlohmann::json::parse(json_response);
    if (!resp.contains("result")) return tools;
    const auto& arr = resp["result"]["tools"];
    if (!arr.is_array()) return tools;
    for (const auto& t : arr) {
      McpClient::McpTool tool;
      tool.name = t.value("name", "");
      tool.description = t.value("description", "");
      tool.input_schema = t.value("inputSchema", nlohmann::json::object());
      tools.push_back(tool);
    }
  } catch (...) {
    // v0.53.68: MCP 工具列表解析失败不能纯静默——调用方拿到空列表
    /// 无法区分"服务器真空"vs"响应畸形",LLM 的 MCP 能力静默消失;
    /// stderr 打点(McpClient 无日志依赖,保持隔离)
    std::fprintf(stderr, "[mcp] list_tools parse failed for endpoint\n");
  }
  return tools;
}

nlohmann::json parse_tool_call_response(const std::string& json_response) {
  try {
    auto resp = nlohmann::json::parse(json_response);
    if (!resp.contains("result")) {
      return {{"ok", false},
              {"error", resp.value("error", nlohmann::json::object())
                            .value("message", "unknown error")}};
    }
    const auto& result = resp["result"];
    nlohmann::json out;
    out["ok"] = !result.contains("isError") || !result["isError"].get<bool>();
    out["result"] = result.value("content", nlohmann::json::array());
    if (!out["ok"].get<bool>()) {
      out["error"] = result.value("content", nlohmann::json::array()).dump();
    }
    return out;
  } catch (...) {
    return {{"ok", false}, {"error", "json parse error"}};
  }
}

}  // namespace agent
}  // namespace thin_agent
