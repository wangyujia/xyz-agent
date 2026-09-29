// test_llm_malformed_guard：v0.52.13 残缺响应守卫回归
//
// 背景：真 e2e 服务死亡（SIGABRT）——nlohmann const operator[] 对
// 缺失 key 断言崩溃。GLM 超时重试后返回残缺体（缺 message/
// tool_call 缺 function）→ parse_tool_calls 断言崩 → 波次 38/39
// 永久 pending。日志末条证据：
//   json.hpp:19306 Assertion `m_value.object->find(key) != ...' failed
//
// 验证（残缺体全部走守卫路径，不崩、返回空/安全值）：
// 1. 缺 choices / choices 空数组
// 2. choices[0] 缺 message
// 3. tool_call 缺 function / 缺 arguments（跳过条目）
// 4. 正常完整响应不受影响（正例）
// 5. Anthropic 体缺 content（missing_content 错误，不崩）
#include "test_macros.h"

#include "thin_agent/llm/CloudLlmClient.h"

using namespace thin_agent;

int main() {
  // 1) 缺 choices
  {
    auto r = CloudLlmClient::parse_tool_calls(R"({"id":"x"})");
    ASSERT_TRUE("缺 choices 返回空不崩", r.empty());
  }
  // 2) choices 空
  {
    auto r = CloudLlmClient::parse_tool_calls(R"({"choices":[]})");
    ASSERT_TRUE("空 choices 返回空不崩", r.empty());
  }
  // 3) choices[0] 缺 message
  {
    auto r = CloudLlmClient::parse_tool_calls(R"({"choices":[{"a":1}]})");
    ASSERT_TRUE("缺 message 返回空不崩", r.empty());
  }
  // 3b) message 缺 tool_calls
  {
    auto r = CloudLlmClient::parse_tool_calls(
        R"({"choices":[{"message":{"role":"assistant"}}]})");
    ASSERT_TRUE("缺 tool_calls 返回空不崩", r.empty());
  }
  // 3c) tool_call 缺 function
  {
    auto r = CloudLlmClient::parse_tool_calls(
        R"({"choices":[{"message":{"tool_calls":[{"id":"t1"}]}}]})");
    ASSERT_TRUE("残缺 tool_call 条目跳过不崩", r.empty());
  }
  // 3d) function 缺 arguments
  {
    auto r = CloudLlmClient::parse_tool_calls(
        R"({"choices":[{"message":{"tool_calls":[)"
        R"({"id":"t1","function":{"name":"write"}}]}}]})");
    ASSERT_TRUE("缺 arguments 条目跳过不崩", r.empty());
  }
  // 4) 正常完整（正例：字符串 arguments）
  {
    auto r = CloudLlmClient::parse_tool_calls(
        R"({"choices":[{"message":{"tool_calls":[)"
        R"({"id":"t1","function":{"name":"write_file","arguments":)"
        R"("{\"path\":\"/tmp/a\"}"}}]}}]})");
    ASSERT_TRUE("正常响应解析出 1 条", r.size() == 1);
    ASSERT_TRUE("名字正确", r[0].name == "write_file");
    ASSERT_TRUE("arguments 解析为对象",
                r[0].arguments.value("path", "") == "/tmp/a");
  }
  // 5) Anthropic 残缺（缺 content）
  {
    auto r = CloudLlmClient::parse_anthropic_response(
        R"({"type":"message","id":"m1"})", 200);
    ASSERT_TRUE("缺 content 报 missing_content 不崩",
                r.error == "missing_content");
  }

  return TEST_REPORT();
}
