// unit_local_models：ModelPool + TemplateModel + AgentTracer 综合测试。

#include <iostream>
#include <string>
#include <memory>

#include "thin_agent/local/ILocalModel.h"
#include "thin_agent/local/ModelPool.h"
#include "thin_agent/local/TemplateModel.h"
#include "thin_agent/local/HybridRouter.h"
#include "thin_agent/agent/AgentTracer.h"
#include "thin_agent/agent/ToolRegistry.h"
#include "thin_agent/agent/McpServer.h"
#include "thin_agent/core/SkillRegistry.h"

using namespace thin_agent::local;
using namespace thin_agent::agent;

namespace {

int failures = 0;
void check(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAIL: " << msg << "\n"; ++failures; }
}

// ── ModelPool ─────────────────────────────────────────────────

int test_model_pool_add_match() {
  auto tm = std::make_unique<TemplateModel>();
  tm->load();
  ModelPool::instance().add(std::move(tm));
  check(ModelPool::instance().size() == 1, "pool size after add");

  auto matches = ModelPool::instance().match("chat");
  check(!matches.empty(), "match chat finds model");
  check(matches[0]->name() == "template", "matched model name");

  auto* best = ModelPool::instance().best("chat");
  check(best != nullptr, "best finds model");
  check(best->name() == "template", "best model name");

  // 不存在的 capability
  auto no = ModelPool::instance().best("translate");
  check(no == nullptr, "best returns null for missing capability");

  ModelPool::instance().remove("template");
  check(ModelPool::instance().size() == 0, "pool empty after remove");
  return 0;
}

int test_model_pool_cascade() {
  auto tm = std::make_unique<TemplateModel>();
  tm->load();
  ModelPool::instance().add(std::move(tm));

  auto result = ModelPool::instance().cascade("chat", "help", 256);
  check(result.ok, "cascade succeeds");
  check(!result.output.empty(), "cascade returns output");
  check(result.model_used == "template", "cascade used template model");

  // 无对应 capability
  auto no = ModelPool::instance().cascade("translate", "hello");
  check(!no.ok, "cascade fails for missing capability");

  ModelPool::instance().remove("template");
  return 0;
}

int test_model_pool_stats() {
  auto tm = std::make_unique<TemplateModel>();
  tm->load();
  ModelPool::instance().add(std::move(tm));

  auto status = ModelPool::instance().status_all();
  check(status.size() == 1, "status_all returns correct count");
  check(status[0]["name"] == "template", "status has model name");
  check(status[0]["loaded"] == true, "status reports loaded");

  ModelPool::instance().remove("template");
  return 0;
}

// ─── ModelPool::reload (v0.9.2) ──────────────────────────────

int test_model_pool_reload_nonexistent() {
  // reload 一个不存在的模型名应该返回 false
  bool ok = ModelPool::instance().reload("nonexistent_model", "/nonexistent/path.gguf");
  check(!ok, "reload nonexistent model returns false");
  return 0;
}

int test_model_pool_reload_invalid_gguf() {
  // 先注册一个 template 模型
  auto tm = std::make_unique<TemplateModel>();
  tm->load();
  ModelPool::instance().add(std::move(tm));
  check(ModelPool::instance().size() == 1, "pool has 1 model before reload");

  // reload 到不存在的 GGUF 文件 → 失败（原模型会被 unload）
  bool ok = ModelPool::instance().reload("template", "/nonexistent/path.gguf");
  check(!ok, "reload with invalid GGUF returns false");

  // 清理
  ModelPool::instance().remove("template");
  return 0;
}

// ── TemplateModel ─────────────────────────────────────────────

int test_template_model_basic() {
  TemplateModel tm;
  tm.load();
  check(tm.is_loaded(), "template model loaded");
  check(tm.backend() == "template", "backend is template");
  check(tm.memory_bytes() == 0, "template model zero memory");

  auto reply = tm.infer("hello", 256);
  check(!reply.empty(), "hello gets reply");

  auto help = tm.infer("help me", 256);
  check(!help.empty(), "help gets reply");

  auto unknown = tm.infer("xyzabc123", 256);
  check(!unknown.empty(), "unknown gets fallback reply");

  return 0;
}

// ── HybridRouter ──────────────────────────────────────────────

int test_hybrid_router() {
  // 确保 ModelPool 有 template model
  auto tm = std::make_unique<TemplateModel>();
  tm->load();
  ModelPool::instance().add(std::move(tm));

  HybridRouter router;
  auto result = router.route("chat", "hello", 256);
  check(result.ok, "hybrid router succeeds");
  check(!result.output.empty(), "hybrid router returns output");
  check(result.model_used.find("template") != std::string::npos, "hybrid used template");

  auto chat = router.chat("help");
  check(!chat.empty(), "chat method works");

  ModelPool::instance().remove("template");
  return 0;
}

// ── AgentTracer ───────────────────────────────────────────────

int test_agent_tracer_basic() {
  AgentTracer::instance().clear();
  AgentTracer::instance().begin_session("test-session");

  auto s1 = AgentTracer::instance().start_span("llm_call");
  AgentTracer::instance().record_llm(s1, 100, 50, "test-model");
  AgentTracer::instance().end_span(s1, "ok");

  auto s2 = AgentTracer::instance().start_span("tool_call", s1);
  AgentTracer::instance().record_tool(s2, "test_tool",
      nlohmann::json{{"arg", "val"}}, nlohmann::json{{"result", 42}});
  AgentTracer::instance().end_span(s2, "ok");

  auto trace = AgentTracer::instance().get_trace();
  check(trace["session_id"] == "test-session", "trace session id");
  check(trace["spans"].size() == 2, "trace has 2 spans");

  auto summary = AgentTracer::instance().summary();
  check(summary["llm_calls"] == 1, "summary llm_calls");
  check(summary["tool_calls"] == 1, "summary tool_calls");
  check(summary["total_input_tokens"] == 100, "summary input tokens");
  check(summary["total_output_tokens"] == 50, "summary output tokens");

  AgentTracer::instance().clear();
  return 0;
}

// ── ToolRegistry conflict ─────────────────────────────────────

int test_tool_registry_conflict() {
  ToolRegistry::instance().clear();

  ToolSchema t1;
  t1.name = "test_tool";
  t1.description = "first";
  t1.execute = [](const nlohmann::json&) { return nlohmann::json{{"ok", true}}; };
  ToolRegistry::instance().register_tool(t1);

  // replace 策略（默认）
  ToolSchema t2;
  t2.name = "test_tool";
  t2.description = "second";
  t2.execute = [](const nlohmann::json&) { return nlohmann::json{{"ok", true}}; };
  auto conflicts = ToolRegistry::instance().register_tool(t2, "replace");
  check(conflicts.size() == 1, "replace reports conflict");
  check(conflicts[0] == "test_tool", "replace conflict name");

  auto* found = ToolRegistry::instance().find("test_tool");
  check(found != nullptr && found->description == "second", "replace overwrote");

  // prefix 策略
  ToolSchema t3;
  t3.name = "test_tool";
  t3.description = "third";
  auto c3 = ToolRegistry::instance().register_tool(t3, "prefix", "mcp_");
  check(c3.size() == 1, "prefix reports conflict");
  auto* prefixed = ToolRegistry::instance().find("mcp_test_tool");
  check(prefixed != nullptr, "prefix creates new name");

  // reject 策略
  ToolSchema t4;
  t4.name = "mcp_test_tool";
  t4.description = "fourth";
  auto c4 = ToolRegistry::instance().register_tool(t4, "reject");
  check(c4.size() == 1, "reject reports conflict");
  auto* still = ToolRegistry::instance().find("mcp_test_tool");
  check(still->description == "third", "reject preserved original");

  ToolRegistry::instance().clear();
  return 0;
}

// ── McpServer ─────────────────────────────────────────────────

int test_mcp_server() {
  thin_agent::SkillRegistry sr;
  // 注册 echo 工具为 cpp_handler
  sr.register_cpp_handler("echo", [](const nlohmann::json& p) {
    return nlohmann::json{{"ok", true}, {"result", {{"echo", p.value("text", "")}}}};
  });

  McpServer server(sr);
  server.set_server_info("test_server", "0.1.0");

  // initialize
  auto init_resp = server.handle_request(
      R"({"jsonrpc":"2.0","method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}},"id":1})");
  check(init_resp.find("serverInfo") != std::string::npos, "initialize has serverInfo");
  check(init_resp.find("test_server") != std::string::npos, "initialize server name");

  // tools/list — echo won't show here because SkillRegistry doesn't list cpp-only handlers
  // cpp handlers registered via register_cpp_handler don't appear in get_enabled_actions()
  // unless they also have a chat_policy.json entry. This is expected behavior.

  // tools/call
  auto call_resp = server.handle_request(
      R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"echo","arguments":{"text":"hello"}},"id":3})");
  check(call_resp.find("hello") != std::string::npos, "tools/call echoes text");

  // 不存在的工具 — find_action returns nullptr before dispatch_cpp
  auto bad_resp = server.handle_request(
      R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"nonexist","arguments":{}},"id":4})");
  check(bad_resp.find("Tool not found") != std::string::npos
        || bad_resp.find("no_cpp_handler") != std::string::npos,
        "nonexistent tool returns error");

  return 0;
}

void test_model_streaming() {
  // TemplateModel streaming (default impl: 一次性返回，done=true 带数据)
  auto tm = std::make_unique<thin_agent::local::TemplateModel>();
  tm->load();
  int chunk_count = 0;
  std::string collected;
  tm->infer_stream("状态", [&](const std::string& token, bool done) {
    if (!token.empty()) { ++chunk_count; collected += token; }
  }, 256, "chat");
  check(chunk_count == 1, "template stream chunk_count=1");
  check(!collected.empty(), "template stream output non-empty");

  // ModelPool cascade_stream（需要注册模型）
  auto tm2 = std::make_unique<thin_agent::local::TemplateModel>();
  tm2->load();
  thin_agent::local::ModelPool::instance().add(std::move(tm2));

  int cascade_chunks = 0;
  std::string cascade_collected;
  auto cascade_result = thin_agent::local::ModelPool::instance().cascade_stream(
      "chat", "系统体检",
      [&](const std::string& token, bool done) {
        if (!token.empty()) { ++cascade_chunks; cascade_collected += token; }
      }, 256);
  check(cascade_result.ok, "cascade_stream ok");
  check(cascade_chunks >= 1, "cascade_stream chunk_count>=1");
  check(!cascade_collected.empty(), "cascade_stream output non-empty");
  check(!cascade_result.model_used.empty(), "cascade_stream model_used set");
  check(!cascade_result.tried.empty(), "cascade_stream tried set");

  // HybridRouter route_stream
  thin_agent::local::HybridRouter router;
  int route_chunks = 0;
  std::string route_collected;
  auto route_result = router.route_stream("chat", "状态",
      [&](const std::string& token, bool done) {
        if (!token.empty()) { ++route_chunks; route_collected += token; }
      }, 256);
  check(route_result.ok, "route_stream ok");
  check(route_chunks >= 1, "route_stream chunk_count>=1");
  check(!route_collected.empty(), "route_stream output non-empty");

  ModelPool::instance().remove("template");
}

// ─── HybridRouter Adaptive (v0.10.1) ──────────────────────────

void test_classify_complexity() {
  using namespace thin_agent::local;

  check(HybridRouter::classify_complexity("help") == TaskComplexity::simple,
        "short word → simple");
  check(HybridRouter::classify_complexity("") == TaskComplexity::simple,
        "empty → simple");

  check(HybridRouter::classify_complexity("what is the weather like today?")
        == TaskComplexity::moderate, "question → moderate");

  check(HybridRouter::classify_complexity(
      "debug the crash in the async thread pool implementation")
        == TaskComplexity::complex, "debug keyword → complex");

  check(HybridRouter::classify_complexity(
      "implement a new function that handles user authentication with OAuth2")
        == TaskComplexity::complex, "implement keyword → complex");

  std::string long_input(350, 'x');
  check(HybridRouter::classify_complexity(long_input) == TaskComplexity::complex,
        "long input → complex");

  check(HybridRouter::classify_complexity("line1\nline2\nline3\nline4\nline5\nline6\nline7\nline8\nline9\nline10")
        == TaskComplexity::complex, "multiline → complex");
}

void test_adaptive_router_simple() {
  using namespace thin_agent::local;

  auto tm = std::make_unique<TemplateModel>();
  tm->load();
  ModelPool::instance().add(std::move(tm));

  HybridRouter::Config cfg;
  cfg.enable_adaptive = true;
  HybridRouter router(cfg);

  auto result = router.route("chat", "hi", 256);
  check(result.ok, "simple route ok");
  check(result.model_used.find("simple:") != std::string::npos,
        "simple route tagged as simple");

  ModelPool::instance().remove("template");
}

void test_adaptive_router_complex() {
  using namespace thin_agent::local;

  auto tm = std::make_unique<TemplateModel>();
  tm->load();
  ModelPool::instance().add(std::move(tm));

  HybridRouter::Config cfg;
  cfg.enable_adaptive = true;
  HybridRouter router(cfg);

  auto result = router.route("chat",
      "implement a function that debugs the crash in the async thread pool", 512);
  check(result.ok, "complex route ok");
  check(result.model_used.find("complex:") != std::string::npos,
        "complex route tagged as complex");

  ModelPool::instance().remove("template");
}

void test_adaptive_router_stream() {
  using namespace thin_agent::local;

  auto tm = std::make_unique<TemplateModel>();
  tm->load();
  ModelPool::instance().add(std::move(tm));

  HybridRouter::Config cfg;
  cfg.enable_adaptive = true;
  HybridRouter router(cfg);

  int chunks = 0;
  std::string collected;
  auto result = router.route_stream("chat", "状态", [&](const std::string& t, bool) {
    if (!t.empty()) { ++chunks; collected += t; }
  }, 256);

  check(result.ok, "adaptive stream ok");
  check(chunks >= 1, "adaptive stream has chunks");
  check(!collected.empty(), "adaptive stream collected text");
  check(!result.model_used.empty(), "adaptive stream model_used set");

  ModelPool::instance().remove("template");
}

}  // namespace

int main() {
  test_model_pool_add_match();
  test_model_pool_cascade();
  test_model_pool_stats();
  test_model_pool_reload_nonexistent();
  test_model_pool_reload_invalid_gguf();
  test_template_model_basic();
  test_hybrid_router();
  test_agent_tracer_basic();
  test_tool_registry_conflict();
  test_mcp_server();
  test_model_streaming();
  test_classify_complexity();
  test_adaptive_router_simple();
  test_adaptive_router_complex();
  test_adaptive_router_stream();

  std::cerr << "\n=== Local model tests: " << failures << " failures ===\n";
  return failures > 0 ? 1 : 0;
}
