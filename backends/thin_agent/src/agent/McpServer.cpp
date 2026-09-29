#include "thin_agent/agent/McpServer.h"

#include <cstdlib>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "thin_agent/Version.h"
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/core/PathValidator.h"

namespace thin_agent {
namespace agent {

// ═══════════════════════════════════════════════════
// 构造
// ═══════════════════════════════════════════════════

McpServer::McpServer(SkillRegistry& skill_registry)
    : skill_registry_(skill_registry) {
  // 默认服务器信息
  server_name_ = "thin_agent";
  server_version_ = std::string(kThinAgentVersion);
}

void McpServer::set_server_info(const std::string& name, const std::string& version) {
  server_name_ = name;
  server_version_ = version;
}

// ═══════════════════════════════════════════════════
// stdio 模式（阻塞循环）
// ═══════════════════════════════════════════════════

void McpServer::run_stdio() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;

    // 尝试解析 JSON-RPC
    std::string response;
    try {
      response = handle_request(line);
    } catch (const std::exception& e) {
      nlohmann::json err_resp;
      err_resp["jsonrpc"] = "2.0";
      err_resp["id"] = nullptr;
      err_resp["error"] = {
        {"code", -32603},
        {"message", std::string("Internal error: ") + e.what()}
      };
      response = err_resp.dump();
    }

    // v0.53.92: notification（JSON-RPC 2.0 无 id）按协议**不得回复**——handle_request
    // 返回空串表示"无响应"，但此前仍 `cout << "" << endl` 写出**空行**：空行不是合法
    // JSON 帧，严格客户端会报 invalid JSON 甚至断开连接。
    if (response.empty()) continue;

    // 写入 stdout（MCP 协议要求一行一个 JSON-RPC 消息）
    std::cout << response << std::endl;
    std::cout.flush();  // 必须在每行后 flush，否则对端会卡住
  }
}

// ═══════════════════════════════════════════════════
// 单条请求处理
// ═══════════════════════════════════════════════════

std::string McpServer::handle_request(const std::string& json_rpc_str) {
  // 如果注册了外部 handler，委托出去
  if (handler_) return handler_(json_rpc_str);

  nlohmann::json req;
  try {
    req = nlohmann::json::parse(json_rpc_str);
  } catch (...) {
    // v0.53.92: JSON-RPC 2.0 规定 parse error 的 id 必须是 **null**（无法确定 id）；
    // 此前写 0，与本文件 catch 分支的 nullptr 自相矛盾
    return make_error(nullptr, -32700, "Parse error");
  }

  // 验证 JSON-RPC 2.0
  if (!req.contains("method") || !req["method"].is_string()) {
    return make_error(id_of(req), -32600, "Invalid Request: missing method");
  }

  std::string method = req["method"].get<std::string>();

  // JSON-RPC 2.0: notification (no "id") → no response
  bool is_notification = !req.contains("id");

  const nlohmann::json id = id_of(req);

  // 路由
  if (method == "initialize")        return handle_initialize(req);
  if (method == "initialized")       return is_notification ? "" : make_response(id, nlohmann::json::object());
  // v0.54.0 (R87): **notification（无 id）一律不得应答**（R80 立的律）此前只覆盖
  // ping/initialized/resources/list/未知方法，`tools/*` 漏了 —— 通知形式的 tools/list 或
  // tools/call 会让服务端回一帧 `{"id":null,...}`，严格客户端会话错位（正是 R80 修掉的那类）。
  if (method == "tools/list") {
    if (is_notification) { (void)handle_list_tools(req); return ""; }  // 只读：执行但不许应答
    return handle_list_tools(req);
  }
  if (method == "tools/call") {
    if (is_notification) {
      // 不执行：无 id 的调用结果无处关联，悄悄跑副作用（工具可能有写盘/网络动作）比拒答更坏。
      std::cerr << "[WARN] tools/call as notification (no id) ignored: not executed"
                << std::endl;
      return "";
    }
    return handle_call_tool(req);
  }
  if (method == "ping")             return is_notification ? "" : make_response(id, {{"pong", true}});
  if (method == "resources/list") {
    if (is_notification) return "";
    // v0.54.0 (R87): 与 tools/list 一致 —— 初始化前不得访问（MCP: 除 initialize/ping 外的
    // 请求在初始化前应报 -32002），此前 resources/list 没有做这个检查（同类不一致）。
    if (!initialized_) return make_error(id, -32002, "Not initialized");
    return make_response(id, {{"resources", nlohmann::json::array()}});
  }
  if (method == "resources/read") {
    if (is_notification) return "";
    if (!initialized_) return make_error(id, -32002, "Not initialized");
    // v0.54.0 (R87): 本服务不提供任何资源（resources/list 恒为空），但**能力面已声明
    // resources**（见 handle_initialize）。因此读取必须回**语义正确的错误**：
    // -32002「Resource not found」——此前回 -32601 Method not found，与已声明的能力矛盾
    // （声明了 resources 却"没有这个方法"）。
    const std::string uri = req.contains("params") && req["params"].is_object()
                                ? req["params"].value("uri", std::string())
                                : std::string();
    return make_error(id, -32002, "Resource not found: " + uri);
  }

  if (is_notification) return "";  // silent ignore for unknown notifications
  return make_error(id, -32601, "Method not found: " + method);
}

// ═══════════════════════════════════════════════════
// initialize
// ═══════════════════════════════════════════════════

std::string McpServer::handle_initialize(const nlohmann::json& req) {
  const nlohmann::json id = id_of(req);

  nlohmann::json result;
  result["protocolVersion"] = "2024-11-05";
  // v0.54.0 (R87): 声明面必须与**已实现的路由面**一致 —— 本服务实现了 resources/list
  // （空列表）与 resources/read（语义化"资源不存在"），此前 capabilities 只声明 tools，
  // 客户端按声明判断会认为服务不支持资源面（声明与实现不一致）。
  result["capabilities"] = {
    {"tools", {{"listChanged", false}}},
    {"resources", {{"listChanged", false}, {"subscribe", false}}}
  };
  result["serverInfo"] = {
    {"name", server_name_},
    {"version", server_version_}
  };

  initialized_ = true;
  return make_response(id, result);
}

// ═══════════════════════════════════════════════════
// tools/list
// ═══════════════════════════════════════════════════

std::string McpServer::handle_list_tools(const nlohmann::json& req) {
  const nlohmann::json id = id_of(req);

  if (!initialized_) return make_error(id, -32002, "Not initialized");

  nlohmann::json tools = nlohmann::json::array();

  // 从 SkillRegistry 获取所有已注册的 action
  for (const auto& [name, action_def] : skill_registry_.action_map()) {
    nlohmann::json tool;
    tool["name"] = name;
    tool["description"] = action_def.desc;

    // 构建 inputSchema
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required_arr = nlohmann::json::array();

    for (const auto& [param_name, param_desc] : action_def.params) {
      properties[param_name] = {
        {"type", "string"},
        {"description", param_desc}
      };
      // 非可选参数加入 required
      bool is_optional = false;
      for (const auto& opt : action_def.optional_params) {
        if (opt == param_name) { is_optional = true; break; }
      }
      if (!is_optional) required_arr.push_back(param_name);
    }

    tool["inputSchema"] = {
      {"type", "object"},
      {"properties", properties},
      {"required", required_arr}
    };

    tools.push_back(tool);
  }

  return make_response(id, {{"tools", tools}});
}

// ═══════════════════════════════════════════════════
// tools/call
// ═══════════════════════════════════════════════════

std::string McpServer::handle_call_tool(const nlohmann::json& req) {
  const nlohmann::json id = id_of(req);

  if (!initialized_) return make_error(id, -32002, "Not initialized");

  if (!req.contains("params") || !req["params"].is_object()) {
    return make_error(id, -32602, "Invalid params: missing params object");
  }

  const auto& params = req["params"];
  std::string tool_name = params.value("name", "");

  if (tool_name.empty()) {
    return make_error(id, -32602, "Invalid params: missing tool name");
  }

  // 查找 action 定义
  auto* action_def = skill_registry_.find_action(tool_name);
  if (!action_def) {
    return make_error(id, -32602, "Tool not found: " + tool_name);
  }

  // 提取参数
  nlohmann::json tool_args;
  if (params.contains("arguments") && params["arguments"].is_object()) {
    tool_args = params["arguments"];
  }

  // 执行
  nlohmann::json result;
  try {
    // 优先尝试 C++ handler
    auto cpp_result = skill_registry_.dispatch_cpp(tool_name, tool_args);

    // dispatch_cpp 返回空 json 表示没有 C++ handler，走 shell
    // v0.49.2: 错误串为 "no_cpp_handler:<name>"（带后缀），改前缀匹配
    if (cpp_result.is_null() || (cpp_result.is_object() && cpp_result.empty()) ||
        (cpp_result.is_object() && cpp_result.contains("error") &&
         cpp_result["error"].is_string() &&
         cpp_result["error"].get<std::string>().rfind("no_cpp_handler", 0) == 0)) {
    // Fallback: shell handler
      std::string os = SkillRegistry::detect_os();
      auto* handler = action_def->find_handler(os);
      if (!handler) {
        result["isError"] = true;
        result["content"] = nlohmann::json::array({
          {{"type", "text"}, {"text", "No handler for OS: " + os}}
        });
        return make_response(id, result);
      }

      // 展开变量并执行 shell 命令
      std::string cmd = SkillRegistry::expand_vars(handler->cmd, tool_args);
      // v0.52.7: 外插值零转义（expand_vars 直接替换）——外插参数携带
      // 元字符即注入。命令整体过分级判官：deny 子串命中（$/sudo/curl
      // 等）或非白名单首词 → 拒执行。与 shell_exec/workflow 同一判官。
      if (grade_shell_risk(cmd) == ToolRisk::Dangerous) {
        result["isError"] = true;
        result["content"] = nlohmann::json::array({
          {"type", "text"},
          {"text", "blocked by risk grading (v0.52.7): command rejected"}
        });
        return make_response(id, result);
      }
      // v0.53.49: 看门狗前缀——外插命令(LLM 参数展开)可能交互式
      /// 等待/挂起(top/less/等待 stdin),裸 popen 永久占住 stdio 循环
      const std::string wrapped =
          "timeout 120 " + cmd;
      FILE* pipe = popen(wrapped.c_str(), "r");
      if (!pipe) {
        result["isError"] = true;
        result["content"] = nlohmann::json::array({
          {{"type", "text"}, {"text", "popen failed"}}
        });
        return make_response(id, result);
      }

      std::string output;
      char buf[4096];
      while (fgets(buf, sizeof(buf), pipe)) output += buf;
      int rc = pclose(pipe);

      if (rc != 0) {
        result["isError"] = true;
      }
      // MCP content must be an array of content blocks
      result["content"] = nlohmann::json::array({
        {{"type", "text"}, {"text", output}}
      });
    } else {
      // C++ handler 成功
      if (cpp_result.contains("success") && !cpp_result["success"].get<bool>()) {
        result["isError"] = true;
      }
      result["content"] = nlohmann::json::array({
        {{"type", "text"}, {"text", cpp_result.dump()}}
      });
    }
  } catch (const std::exception& e) {
    result["isError"] = true;
    result["content"] = nlohmann::json::array({
      {{"type", "text"}, {"text", std::string("Exception: ") + e.what()}}
    });
  }

  return make_response(id, result);
}

// ═══════════════════════════════════════════════════
// 工具方法
// ═══════════════════════════════════════════════════

/// id 原样提取：有 id 用原值（字符串/数字皆可），无 id（notification）给 null。
nlohmann::json McpServer::id_of(const nlohmann::json& req) {
  return req.contains("id") ? req["id"] : nlohmann::json(nullptr);
}

std::string McpServer::make_response(const nlohmann::json& id, const nlohmann::json& result) {
  nlohmann::json resp;
  resp["jsonrpc"] = "2.0";
  resp["id"] = id;
  resp["result"] = result;
  return resp.dump();
}

std::string McpServer::make_error(const nlohmann::json& id, int code,
                                 const std::string& message) {
  nlohmann::json resp;
  resp["jsonrpc"] = "2.0";
  resp["id"] = id;
  resp["error"] = {
    {"code", code},
    {"message", message}
  };
  return resp.dump();
}

}  // namespace agent
}  // namespace thin_agent
