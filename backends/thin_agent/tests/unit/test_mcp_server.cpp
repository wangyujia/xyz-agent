/// test_mcp_server.cpp — McpServer 单元测试
#include <cassert>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "thin_agent/agent/McpServer.h"
#include "thin_agent/core/SkillRegistry.h"

using namespace thin_agent;
using namespace thin_agent::agent;

static nlohmann::json parse_response(const std::string& raw) {
  return nlohmann::json::parse(raw);
}

static SkillRegistry make_test_registry() {
  SkillRegistry reg;
  nlohmann::json skills_json = nlohmann::json::object();
  {
    nlohmann::json action;
    action["name"] = "test_echo";
    action["desc"] = "Echo back a message";
    action["params"] = {{"message", "Message to echo"}};
    action["optional"] = nlohmann::json::array();
    action["handlers"] = {{"linux", {{"type", "shell"}, {"cmd", "echo {message}"}}}};

    nlohmann::json skill;
    skill["enabled"] = true;
    skill["desc"] = "Test skill";
    skill["actions"] = nlohmann::json::array({action});
    skills_json["test_skill"] = skill;
  }
  nlohmann::json whitelist_json;
  reg.load_from_policy(skills_json, whitelist_json);
  return reg;
}

// ═══════════════════════════════════
// 初始化
// ═══════════════════════════════════
static void test_initialize() {
  auto reg = make_test_registry();
  McpServer server(reg);
  std::string req = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}})";
  auto json = parse_response(server.handle_request(req));
  assert(json["id"] == 1);
  assert(json["result"]["protocolVersion"] == "2024-11-05");
  assert(json["result"]["serverInfo"]["name"] == "thin_agent");
  std::cout << "PASS: test_initialize\n";
}

static void test_initialize_custom_name() {
  auto reg = make_test_registry();
  McpServer server(reg);
  server.set_server_info("my_agent", "2.0.0");
  std::string req = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})";
  auto json = parse_response(server.handle_request(req));
  assert(json["result"]["serverInfo"]["name"] == "my_agent");
  assert(json["result"]["serverInfo"]["version"] == "2.0.0");
  std::cout << "PASS: test_initialize_custom_name\n";
}

static void test_ping() {
  auto reg = make_test_registry();
  McpServer server(reg);
  auto json = parse_response(server.handle_request(R"({"jsonrpc":"2.0","id":2,"method":"ping"})"));
  assert(json["result"]["pong"] == true);
  std::cout << "PASS: test_ping\n";
}

// ═══════════════════════════════════
// tools/list
// ═══════════════════════════════════
static void test_list_tools() {
  auto reg = make_test_registry();
  McpServer server(reg);
  server.handle_request(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})");
  auto json = parse_response(server.handle_request(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})"));
  assert(json["result"]["tools"].is_array());
  assert(json["result"]["tools"].size() == 1);
  assert(json["result"]["tools"][0]["name"] == "test_echo");
  assert(json["result"]["tools"][0]["inputSchema"]["properties"].contains("message"));
  std::cout << "PASS: test_list_tools\n";
}

static void test_list_tools_empty() {
  SkillRegistry empty;
  McpServer server(empty);
  server.handle_request(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})");
  auto json = parse_response(server.handle_request(R"({"jsonrpc":"2.0","id":4,"method":"tools/list"})"));
  assert(json["result"]["tools"].is_array());
  assert(json["result"]["tools"].empty());
  std::cout << "PASS: test_list_tools_empty\n";
}

// ═══════════════════════════════════
// tools/call
// ═══════════════════════════════════
static void test_call_tool_shell() {
  auto reg = make_test_registry();
  McpServer server(reg);
  server.handle_request(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})");
  auto json = parse_response(server.handle_request(
    R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"test_echo","arguments":{"message":"hello"}}})"));
  assert(json["result"]["content"].is_array());
  assert(json["result"]["content"][0]["text"] == "hello\n");
  std::cout << "PASS: test_call_tool_shell\n";
}

static void test_call_tool_not_found() {
  auto reg = make_test_registry();
  McpServer server(reg);
  server.handle_request(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})");
  auto json = parse_response(server.handle_request(
    R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"nonexistent"}})"));
  assert(json.contains("error"));
  std::cout << "PASS: test_call_tool_not_found\n";
}

static void test_call_tool_no_args() {
  auto reg = make_test_registry();
  McpServer server(reg);
  server.handle_request(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})");
  auto json = parse_response(server.handle_request(
    R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"test_echo"}})"));
  assert(json["result"]["content"].is_array());
  std::cout << "PASS: test_call_tool_no_args\n";
}

static void test_call_tool_cpp_handler() {
  SkillRegistry reg;
  reg.register_cpp_handler("cpp_echo", [](const nlohmann::json& params) {
    return nlohmann::json{{"success", true}, {"output", "ECHO: " + params.value("message", "default")}};
  });
  McpServer server(reg);
  server.handle_request(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})");
  auto json = parse_response(server.handle_request(
    R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"cpp_echo","arguments":{"message":"test"}}})"));
  std::string text = json["result"]["content"][0]["text"];
  assert(text.find("ECHO: test") != std::string::npos);
  std::cout << "PASS: test_call_tool_cpp_handler\n";
}

// ═══════════════════════════════════
// 错误处理
// ═══════════════════════════════════
static void test_parse_error() {
  auto reg = make_test_registry();
  McpServer server(reg);
  auto json = parse_response(server.handle_request("not json"));
  assert(json["error"]["code"] == -32700);
  std::cout << "PASS: test_parse_error\n";
}

static void test_method_not_found() {
  auto reg = make_test_registry();
  McpServer server(reg);
  auto json = parse_response(server.handle_request(R"({"jsonrpc":"2.0","id":9,"method":"nonexistent"})"));
  assert(json["error"]["code"] == -32601);
  std::cout << "PASS: test_method_not_found\n";
}

static void test_missing_method() {
  auto reg = make_test_registry();
  McpServer server(reg);
  auto json = parse_response(server.handle_request(R"({"jsonrpc":"2.0","id":10})"));
  assert(json["error"]["code"] == -32600);
  std::cout << "PASS: test_missing_method\n";
}

static void test_resources_list() {
  auto reg = make_test_registry();
  McpServer server(reg);
  server.handle_request(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})");
  auto json = parse_response(server.handle_request(R"({"jsonrpc":"2.0","id":11,"method":"resources/list"})"));
  assert(json["result"]["resources"].is_array());
  std::cout << "PASS: test_resources_list\n";
}

static void test_initialized_ack() {
  auto reg = make_test_registry();
  McpServer server(reg);
  auto json = parse_response(server.handle_request(R"({"jsonrpc":"2.0","id":12,"method":"initialized"})"));
  assert(json["result"].is_object());
  std::cout << "PASS: test_initialized_ack\n";
}

int main() {
  std::cout << "=== McpServer Tests ===\n";
  test_initialize();
  test_initialize_custom_name();
  test_ping();
  test_list_tools();
  test_list_tools_empty();
  test_call_tool_shell();
  test_call_tool_not_found();
  test_call_tool_no_args();
  test_call_tool_cpp_handler();
  test_parse_error();
  test_method_not_found();
  test_missing_method();
  test_resources_list();
  test_initialized_ack();
  std::cout << "\nALL TESTS PASSED\n";
  return 0;
}
