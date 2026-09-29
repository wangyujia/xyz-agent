// unit_agent_loop: AgentLoop 子Agent路径测试 (v0.39.1)
//
// 1. AgentLoop set_skill_manager 可正确接收 SkillManager*
// 2. Vision ChatMessage → build_request_body 端到端传播

#include <cstdlib>
#include <iostream>
#include <vector>

#include "thin_agent/agent/AgentLoop.h"
#include "thin_agent/agent/EmbeddingProvider.h"
#include "thin_agent/agent/VectorStore.h"
#include "thin_agent/agent/SkillManager.h"
#include "thin_agent/llm/CloudLlmClient.h"
#include "thin_agent/llm/LlmCircuitBreaker.h"
#include "thin_agent/llm/DemoConfigCompat.h"

using namespace thin_agent;
using namespace thin_agent::agent;

namespace {
int failures = 0;
void check(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAIL: " << msg << "\n"; ++failures; }
}
}  // namespace

static void ensure_dummy_tool_registered() {
  // v0.53.19: 裸 AgentLoop 进程的 ToolRegistry 无工具→tools_openai 空→
  // use_native_fc 分支不进（走 call_llm_text/RESPONSE mock）——FC 轮测试
  // 须先注册一个工具让 native FC 分支激活
  static bool done = false;
  if (done) return;
  done = true;
  agent::ToolSchema ts;
  ts.name = "list_dir";
  ts.description = "list directory";
  agent::ToolParameter p;
  p.name = "dir";
  p.type = "string";
  p.description = "target dir";
  p.required = true;
  ts.parameters.push_back(p);
  ts.execute = [](const nlohmann::json& params) -> nlohmann::json {
    return {{"ok", true}, {"result", {{"count", 2}}}};
  };
  ToolRegistry::instance().register_tool(std::move(ts));
}

void test_turns_exhausted_not_fake_green() {
  // v0.53.17: turns 耗尽必须 ok=false+degraded（此前 ok=true+固定文案=假绿，
  // 真任务实测：写码任务 turns 顶格后文件已写未编译，调用方拿到 ok=True）
  DemoConfigCompat cfg;
  cfg.provider = "mock";
  cfg.model_name = "mock";
  cfg.api_base = "http://localhost";
  AgentLoopConfig loop_cfg;
  loop_cfg.max_turns = 1;  // 极小轮数逼耗尽
  AgentLoop loop(cfg, "dummy-session", ToolRegistry::instance(), loop_cfg);
  ensure_dummy_tool_registered();
  LlmCircuitBreaker::instance().reset();  // 消除测试顺序熔断污染
  // v0.53.19: 双 mock 注入——意图轮 chat_completion 读 RESPONSE（只注 TOOLS
  // 时意图轮真连 localhost 三连败触发熔断，circuit_open 掩盖真实行为）
  ::setenv("THIN_AGENT_TEST_CLOUD_RESPONSE", "{\"final_answer\":\"\"}", 1);
  // FC 轮：每轮都返回 tool_calls（list_dir 无限循环）逼 turns 耗尽
  ::setenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE",
           "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"tool_calls\":"
           "[{\"id\":\"c1\",\"type\":\"function\",\"function\":"
           "{\"name\":\"list_dir\",\"arguments\":\"{\\\"dir\\\":\\\"/tmp\\\"}\"}}]}}]}", 1);
  auto result = loop.run("写一个很长的程序", "");
  ::unsetenv("THIN_AGENT_TEST_CLOUD_RESPONSE");
  check(!result.ok, "v0.53.17: turns 耗尽 ok=false（不再假绿）");

  check(result.degraded, "turns 耗尽 degraded=true");
  check(result.error == "turns_exhausted", "error=turns_exhausted");
  check(result.final_answer.find("max_turns") != std::string::npos ||
            result.final_answer.find("轮") != std::string::npos,
        "文案含实际进度信息（轮数/工具数）");
}

void test_malformed_json_not_final_answer() {
  // v0.53.19: 出口②——parse_error 分支（截断的 {"choices":...）不得当
  // final_answer 返回 ok=true（h1d 实测漏网路径）
  DemoConfigCompat cfg;
  cfg.provider = "mock";
  cfg.model_name = "mock";
  cfg.api_base = "http://localhost";
  AgentLoopConfig loop_cfg;
  loop_cfg.max_turns = 3;
  AgentLoop loop(cfg, "dummy-session", ToolRegistry::instance(), loop_cfg);
  ensure_dummy_tool_registered();
  LlmCircuitBreaker::instance().reset();  // 消除测试顺序熔断污染
  ::setenv("THIN_AGENT_TEST_CLOUD_RESPONSE", "{\"final_answer\":\"\"}", 1);
  // 注入：每轮都返回截断的 choices JSON（模拟 GLM 断流）
  ::setenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE",
           "{\"choices\":[{\"message\":{\"content\":\"text\",\"role\":\"assistant\"", 1);
  auto result = loop.run("做个东西", "");
  ::unsetenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE");
  const bool leaked = result.ok &&
      result.final_answer.compare(0, 10, "{\"choices\":") == 0;
  check(!leaked, "v0.53.19: 截断 choices JSON 不得作为 final_answer 泄漏");
}

void test_agent_loop_skill_setter() {
  DemoConfigCompat cfg;
  cfg.provider = "mock";
  cfg.model_name = "mock";
  cfg.api_base = "http://localhost";
  AgentLoopConfig loop_cfg;
  AgentLoop loop(cfg, "", ToolRegistry::instance(), loop_cfg);

  auto emb = std::make_shared<LocalHashEmbeddingProvider>(256);
  auto store = std::make_shared<VectorStore>();
  store->init("/tmp/test_loop_skill.db", emb->dimension());
  SkillManager mgr(emb, store);

  // 验证 setter 不 throw（指针被接受并可用于后续注入）
  loop.set_skill_manager(&mgr);
  check(true, "set_skill_manager accepted");

  std::remove("/tmp/test_loop_skill.db");
}

void test_vision_chatmessage_to_payload() {
  std::vector<ChatMessage> msgs;
  {
    ChatMessage umsg{"user", "这是什么？"};
    umsg.image_base64 = "aGVsbG8=";
    msgs.push_back(umsg);
  }

  // vision=true
  {
    std::string body = CloudLlmClient::build_request_body(
        msgs, "glm-4v", 2048,
        nlohmann::json::object(), nlohmann::json::array(), false, true);
    auto j = nlohmann::json::parse(body);
    const auto& content = j["messages"][0]["content"];
    check(content.is_array(), "vision=true: content is array");
    check(content[1]["image_url"]["url"].get<std::string>().find("aGVsbG8=") != std::string::npos,
          "vision=true: base64 propagated");
  }

  // vision=false
  {
    std::string body = CloudLlmClient::build_request_body(
        msgs, "deepseek-v4", 2048,
        nlohmann::json::object(), nlohmann::json::array(), false, false);
    auto j = nlohmann::json::parse(body);
    check(j["messages"][0]["content"].is_string(), "vision=false: content string");
  }
}

int main() {
  test_turns_exhausted_not_fake_green();
  test_malformed_json_not_final_answer();
  test_agent_loop_skill_setter();
  test_vision_chatmessage_to_payload();

  if (failures == 0) {
    std::cout << "ALL unit_agent_loop PASSED" << std::endl;
    return 0;
  }
  std::cerr << failures << " test(s) FAILED" << std::endl;
  return 1;
}
