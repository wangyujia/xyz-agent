#include "thin_agent/llm/CloudLlmClient.h"
#include "thin_agent/llm/DemoConfigCompat.h"

#include <cassert>
#include <cstdio>
#include <nlohmann/json.hpp>

// Test fixtures for Anthropic Messages format conversion.
// Uses the public static methods of CloudLlmClient.

using json = nlohmann::json;
using thin_agent::CloudLlmClient;
using thin_agent::DemoConfigCompat;
using thin_agent::ChatMessage;

int main() {
  int failures = 0;
  auto pass = [&](const char* name) { printf("  PASS: %s\n", name); };
  auto fail = [&](const char* name, const char* detail) {
    ++failures;
    printf("  FAIL: %s — %s\n", name, detail);
  };

  // ── is_anthropic_mode / infer_chat_endpoint_for_mode ──
  {
    thin_agent::DemoConfigCompat cfg;
    cfg.api_mode = "anthropic_messages";
    auto ep = CloudLlmClient::infer_chat_endpoint_for_mode(cfg);
    if (ep.find("anthropic.com") != std::string::npos)
      pass("anthropic endpoint");
    else
      fail("anthropic endpoint", ep.c_str());

    cfg.api_mode = "";
    auto ep2 = CloudLlmClient::infer_chat_endpoint_for_mode(cfg);
    if (ep2.find("anthropic.com") == std::string::npos)
      pass("openai-compatible endpoint (not anthropic)");
    else
      fail("openai-compatible endpoint", ep2.c_str());
  }

  // ── build_anthropic_request_body: system → top-level ──
  {
    std::vector<ChatMessage> msgs = {
      {"system", "You are helpful."},
      {"user", "Hello"}
    };
    std::string body = CloudLlmClient::build_anthropic_request_body(msgs, "claude-sonnet", 4096, json::array());
    auto j = json::parse(body);

    if (j.value("system", "") == "You are helpful.") pass("system→top-level");
    else fail("system→top-level", j.dump().c_str());

    if (j["messages"].size() == 1) pass("only user in messages array");
    else fail("only user in messages", std::to_string(j["messages"].size()).c_str());

    if (j["messages"][0]["role"] == "user") pass("user role preserved");
    else fail("user role", j["messages"][0].dump().c_str());
  }

  // ── build_anthropic_request_body: tool → tool_result ──
  {
    std::vector<ChatMessage> msgs = {
      {"user", "What is the time?"},
      {"assistant", "", "", "", json::array({json::object({
        {"id", "toolu_001"},
        {"type", "function"},
        {"function", {{"name", "get_time"}, {"arguments", "{}"}}}
      })})},
      {"tool", "2024-01-01 12:00", "toolu_001"}
    };
    std::string body = CloudLlmClient::build_anthropic_request_body(msgs, "claude", 4096, json::array());
    auto j = json::parse(body);

    if (j["messages"].size() == 3) pass("tool roundtrip message count");
    else fail("tool count", std::to_string(j["messages"].size()).c_str());

    auto& tool_msg = j["messages"][2];
    if (tool_msg["role"] == "user") pass("tool role→user");
    else fail("tool role", tool_msg["role"].get<std::string>().c_str());

    auto& tc = tool_msg["content"];
    if (tc.is_array() && tc[0]["type"] == "tool_result") pass("tool→tool_result type");
    else fail("tool_result type", tc.dump().c_str());

    if (tc[0]["tool_use_id"] == "toolu_001") pass("tool_use_id preserved");
    else fail("tool_use_id", tc[0]["tool_use_id"].get<std::string>().c_str());
  }

  // ── build_anthropic_request_body: OpenAI tools → Anthropic format ──
  {
    json openai_tools = json::array({
      {{"type", "function"},
       {"function", {
         {"name", "read_file"},
         {"description", "Read a file"},
         {"parameters", {{"type", "object"}, {"properties", json::object()}}}
       }}}
    });

    std::vector<ChatMessage> msgs = {{"user", "Read /tmp/x"}};
    std::string body = CloudLlmClient::build_anthropic_request_body(msgs, "claude", 4096, openai_tools);
    auto j = json::parse(body);

    if (j.contains("tools") && j["tools"].is_array()) pass("tools array present");
    else fail("tools array", j.dump().c_str());

    auto& at = j["tools"][0];
    if (at["name"] == "read_file") pass("tool name preserved");
    else fail("tool name", at["name"].get<std::string>().c_str());

    if (at["description"] == "Read a file") pass("tool description preserved");
    else fail("tool desc", at["description"].get<std::string>().c_str());

    if (at.contains("input_schema") && !at.contains("function"))
      pass("anthropic tool: no function wrapper");
    else fail("no function wrapper", at.dump().c_str());
  }

  // ── parse_anthropic_response: text ──
  {
    json resp = {
      {"type", "message"},
      {"role", "assistant"},
      {"content", json::array({
        {{"type", "text"}, {"text", "Hello there!"}}
      })},
      {"stop_reason", "end_turn"}
    };
    auto result = CloudLlmClient::parse_anthropic_response(resp.dump(), 200);
    if (result.ok) pass("parse text response ok");
    else fail("parse text ok", result.error.c_str());

    auto parsed = json::parse(result.text);
    if (parsed["choices"][0]["message"]["content"] == "Hello there!")
      pass("parse text content correct");
    else fail("text content", parsed.dump().c_str());
  }

  // ── parse_anthropic_response: tool_use ──
  {
    json resp = {
      {"type", "message"},
      {"role", "assistant"},
      {"content", json::array({
        {{"type", "tool_use"}, {"id", "toolu_002"}, {"name", "read_file"},
         {"input", {{"path", "/tmp/x"}}}}
      })},
      {"stop_reason", "tool_use"}
    };
    auto result = CloudLlmClient::parse_anthropic_response(resp.dump(), 200);
    if (result.ok) pass("parse tool_use response ok");
    else fail("parse tool_use ok", result.error.c_str());

    auto parsed = json::parse(result.text);
    auto& tcs = parsed["choices"][0]["message"]["tool_calls"];
    if (tcs.is_array() && tcs.size() == 1) pass("tool_use → tool_calls array");
    else fail("tool_calls array", parsed.dump().c_str());

    if (tcs[0]["function"]["name"] == "read_file") pass("tool_use name→function.name");
    else fail("tool name", tcs[0].dump().c_str());

    auto args_str = tcs[0]["function"]["arguments"];
    if (args_str.is_string()) {
      auto args = json::parse(args_str.get<std::string>());
      if (args["path"] == "/tmp/x") pass("tool_use input→arguments roundtrip");
      else fail("args roundtrip", args.dump().c_str());
    } else {
      fail("args is string", args_str.dump().c_str());
    }
  }

  // ── parse_anthropic_response: error ──
  {
    json err = {
      {"type", "error"},
      {"error", {{"type", "invalid_request_error"}, {"message", "bad"}}}
    };
    auto result = CloudLlmClient::parse_anthropic_response(err.dump(), 400);
    if (!result.ok) pass("error response not ok");
    else fail("error response", result.text.c_str());
  }

  // ── parse_sse_stream preserves old behavior ──
  {
    // Smoke test: ensure parse_sse_stream doesn't crash on valid SSE
    std::string sse = "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\ndata: [DONE]\n";
    std::vector<std::string> chunks;
    auto result = thin_agent::CloudLlmClient::parse_sse_stream(sse, &chunks);
    (void)result;
    pass("parse_sse_stream doesn't crash");
  }

  printf("\n%s (%d failures)\n",
         failures == 0 ? "ALL provider tests PASSED" : "SOME FAILED",
         failures);
  return failures > 0 ? 1 : 0;
}
