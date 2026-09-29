// unit_usage_tracking：token 用量解析测试
// 验证 v0.47.1 CloudChatResult.usage 从 API 响应中正确解析

#include <cassert>
#include <iostream>
#include <string>

#include "thin_agent/llm/CloudLlmClient.h"
#include "test_macros.h"

int main() {
  using namespace thin_agent;

  // ── 测试 1: 非流式响应 — usage 在 JSON 顶层 ──
  {
    std::string resp = R"({
      "choices": [{"message": {"role": "assistant", "content": "Hello!"}}],
      "usage": {"prompt_tokens": 52, "completion_tokens": 3, "total_tokens": 55}
    })";
    auto r = CloudLlmClient::parse_response(resp, 200);
    ASSERT_EQ("nonstream_ok", r.ok, true);
    ASSERT_EQ("nonstream_prompt", r.usage.prompt_tokens, 52);
    ASSERT_EQ("nonstream_completion", r.usage.completion_tokens, 3);
    ASSERT_EQ("nonstream_total", r.usage.total_tokens, 55);
  }

  // ── 测试 2: 无 usage 字段 — 全零 ──
  {
    std::string resp = R"({
      "choices": [{"message": {"role": "assistant", "content": "Hi"}}]
    })";
    auto r = CloudLlmClient::parse_response(resp, 200);
    ASSERT_EQ("no_usage_ok", r.ok, true);
    ASSERT_EQ("no_usage_prompt", r.usage.prompt_tokens, 0);
    ASSERT_EQ("no_usage_completion", r.usage.completion_tokens, 0);
    ASSERT_EQ("no_usage_total", r.usage.total_tokens, 0);
  }

  // ── 测试 3: tool_calls 分支也有 usage ──
  {
    std::string resp = R"({
      "choices": [{"message": {
        "role": "assistant",
        "content": null,
        "tool_calls": [{"id": "call_1", "type": "function",
          "function": {"name": "shell_exec", "arguments": "{\"cmd\":\"ls\"}"}}]
      }}],
      "usage": {"prompt_tokens": 100, "completion_tokens": 20, "total_tokens": 120}
    })";
    auto r = CloudLlmClient::parse_response(resp, 200);
    ASSERT_EQ("toolcall_ok", r.ok, true);
    ASSERT_EQ("toolcall_prompt", r.usage.prompt_tokens, 100);
    ASSERT_EQ("toolcall_completion", r.usage.completion_tokens, 20);
    ASSERT_EQ("toolcall_total", r.usage.total_tokens, 120);
  }

  // ── 测试 4: 流式响应 — usage 在最后一个 chunk ──
  {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"!\"}}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":3,\"total_tokens\":13}}\n"
        "data: [DONE]\n";
    std::vector<std::string> chunks;
    auto r = CloudLlmClient::parse_sse_stream(sse, &chunks);
    ASSERT_EQ("stream_ok", r.ok, true);
    ASSERT_EQ("stream_text", r.text, std::string("Hello!"));
    ASSERT_EQ("stream_prompt", r.usage.prompt_tokens, 10);
    ASSERT_EQ("stream_completion", r.usage.completion_tokens, 3);
    ASSERT_EQ("stream_total", r.usage.total_tokens, 13);
  }

  // ── 测试 5: 流式响应无 usage — 全零 ──
  {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n"
        "data: [DONE]\n";
    auto r = CloudLlmClient::parse_sse_stream(sse, nullptr);
    ASSERT_EQ("stream_no_usage_ok", r.ok, true);
    ASSERT_EQ("stream_no_usage_total", r.usage.total_tokens, 0);
  }

  // ── 测试 6: usage 字段存在但值为 0 ──
  {
    std::string resp = R"({
      "choices": [{"message": {"role": "assistant", "content": "ok"}}],
      "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0}
    })";
    auto r = CloudLlmClient::parse_response(resp, 200);
    ASSERT_EQ("zero_usage_ok", r.ok, true);
    ASSERT_EQ("zero_usage_total", r.usage.total_tokens, 0);
  }

  TEST_REPORT();
}
