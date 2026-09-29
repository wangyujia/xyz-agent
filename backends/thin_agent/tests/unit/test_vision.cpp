// unit_vision: 多模态 Vision 支持测试 (v0.39.0)
//
// 验证 build_request_body 在 vision=true 时的 content 数组序列化，
// 以及 vision=false/默认值 时的纯文本降级。

#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "thin_agent/llm/CloudLlmClient.h"

using namespace thin_agent;

// ── 测试框架 ──────────────────────────────────────────────────────
namespace {
int failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
  }
}

void check_str_contains(const std::string& haystack,
                        const std::string& needle, const char* msg) {
  if (haystack.find(needle) == std::string::npos) {
    std::cerr << "FAIL: " << msg << " — 期望包含 \"" << needle
              << "\"，实际: \"" << haystack.substr(0, 300) << "\"\n";
    ++failures;
  }
}

}  // namespace

// ════════════════════════════════════════════════════════════════════
// 1. vision=true: 有图片时 content 序列化为数组
// ════════════════════════════════════════════════════════════════════

void test_vision_content_array() {
  std::vector<ChatMessage> msgs = {
    ChatMessage{"system", "你是助手"},
    ChatMessage{"user", "这是什么？"},
  };
  // 第 2 条 user 消息带图片
  msgs[1].image_base64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk";
  msgs[1].name = "";  // 保持其他字段默认

  std::string body = CloudLlmClient::build_request_body(
      msgs, "glm-4v", 2048,
      nlohmann::json::object(),  // response_format
      nlohmann::json::array(),   // tools (empty)
      false,                     // stream
      true                       // vision ← 关键
  );

  auto j = nlohmann::json::parse(body);
  auto& messages = j["messages"];

  // system 消息：纯文本
  const auto& sys_content = messages[0]["content"];
  check(sys_content.is_string(), "sys content is string");
  check(sys_content.get<std::string>() == "你是助手", "sys content correct");

  // user 消息：content 数组（text + image_url）
  const auto& user_content = messages[1]["content"];
  check(user_content.is_array(), "user content is array");
  check(user_content.size() == 2, "user content has 2 parts");

  // 第1部分：text
  check(user_content[0]["type"] == "text", "part 0 type=text");
  check(user_content[0]["text"] == "这是什么？", "part 0 text correct");

  // 第2部分：image_url
  check(user_content[1]["type"] == "image_url", "part 1 type=image_url");
  check_str_contains(user_content[1]["image_url"]["url"].get<std::string>(),
                     "data:image/png;base64,", "data URI prefix present");
  check_str_contains(user_content[1]["image_url"]["url"].get<std::string>(),
                     "iVBORw0", "base64 data present");
}

// ════════════════════════════════════════════════════════════════════
// 2. vision=false（默认）：带图片时 content 仍是纯文本
// ════════════════════════════════════════════════════════════════════

void test_vision_disabled_plain_text() {
  std::vector<ChatMessage> msgs = {
    ChatMessage{"user", "这是什么？"},
  };
  msgs[0].image_base64 = "fakebase64data";

  // vision=false（显式）
  std::string body = CloudLlmClient::build_request_body(
      msgs, "deepseek-v4-pro", 2048,
      nlohmann::json::object(),
      nlohmann::json::array(),
      false,
      false  // vision=false
  );

  auto j = nlohmann::json::parse(body);
  const auto& content = j["messages"][0]["content"];
  check(content.is_string(), "vision=false: content is string, not array");
}

void test_vision_default_disabled() {
  std::vector<ChatMessage> msgs = {
    ChatMessage{"user", "这是什么？"},
  };
  msgs[0].image_base64 = "fakebase64data";

  // 不传 vision 参数（默认 false）
  std::string body = CloudLlmClient::build_request_body(
      msgs, "deepseek-v4-pro", 2048);

  auto j = nlohmann::json::parse(body);
  const auto& content = j["messages"][0]["content"];
  check(content.is_string(), "default vision=false: content is string");
}

// ════════════════════════════════════════════════════════════════════
// 3. 空图片：vision=true 但 image_base64 为空 → 纯文本
// ════════════════════════════════════════════════════════════════════

void test_vision_empty_image() {
  std::vector<ChatMessage> msgs = {
    ChatMessage{"user", "普通文本消息"},
  };
  // image_base64 默认空

  std::string body = CloudLlmClient::build_request_body(
      msgs, "glm-4v", 2048,
      nlohmann::json::object(), nlohmann::json::array(), false,
      true  // vision=true 但 image 为空
  );

  auto j = nlohmann::json::parse(body);
  const auto& content = j["messages"][0]["content"];
  check(content.is_string(), "vision=true empty image: content is string");
  check(content.get<std::string>() == "普通文本消息", "text preserved");
}

// ════════════════════════════════════════════════════════════════════
// 4. 多轮对话：只有带图片的 user 消息序列化为数组
// ════════════════════════════════════════════════════════════════════

void test_vision_multi_turn() {
  std::vector<ChatMessage> msgs = {
    ChatMessage{"system", "你是助手"},
    ChatMessage{"user", "你好"},
    ChatMessage{"assistant", "你好！有什么可以帮你的？"},
    ChatMessage{"user", "这个图是什么？"},
  };
  msgs[3].image_base64 = "abc123base64";  // 只有第 4 条带图

  std::string body = CloudLlmClient::build_request_body(
      msgs, "glm-4v", 2048,
      nlohmann::json::object(), nlohmann::json::array(), false,
      true);

  auto j = nlohmann::json::parse(body);
  auto& messages = j["messages"];
  check(messages.size() == 4, "4 messages total");

  // msg 0: system → string
  check(messages[0]["content"].is_string(), "msg0 system is string");
  // msg 1: user without image → string
  check(messages[1]["content"].is_string(), "msg1 user w/o image is string");
  // msg 2: assistant → string
  check(messages[2]["content"].is_string(), "msg2 assistant is string");
  // msg 3: user with image → array
  check(messages[3]["content"].is_array(), "msg3 user w/ image is array");
  check(messages[3]["content"][1]["image_url"]["url"].get<std::string>()
            .find("abc123base64") != std::string::npos,
        "msg3 image data correct");
}

// ════════════════════════════════════════════════════════════════════
// main
// ════════════════════════════════════════════════════════════════════

int main() {
  test_vision_content_array();
  test_vision_disabled_plain_text();
  test_vision_default_disabled();
  test_vision_empty_image();
  test_vision_multi_turn();

  if (failures == 0) {
    std::cout << "ALL unit_vision PASSED" << std::endl;
    return 0;
  }
  std::cerr << failures << " test(s) FAILED" << std::endl;
  return 1;
}
