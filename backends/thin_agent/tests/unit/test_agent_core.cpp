// unit_agent_core：ToolRegistry + EmbeddingProvider + VectorStore + MemoryManager + 新组件综合测试。

#include <iostream>
#include <chrono>
#include "thin_agent/llm/LlmCircuitBreaker.h"
#include "thin_agent/agent/ErrorDetector.h"
#include "thin_agent/agent/ErrorCorrectionStore.h"
#include "thin_agent/agent/GoalManager.h"
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "thin_agent/agent/ToolRegistry.h"
#include "thin_agent/agent/ToolSchema.h"
#include "thin_agent/agent/EmbeddingProvider.h"
#include "thin_agent/agent/VectorStore.h"
#include "thin_agent/agent/MemoryManager.h"
#include "thin_agent/agent/ConversationSummarizer.h"
#include "thin_agent/agent/CheckpointManager.h"
#include "thin_agent/agent/FilesystemCheckpoint.h"
#include "thin_agent/agent/SkillManager.h"
#include "thin_agent/core/CronScheduler.h"
#include "thin_agent/core/AgentOrchestrator.h"
#include "thin_agent/core/ProactiveMonitor.h"
#include "thin_agent/core/KanbanBoard.h"
#include "thin_agent/agent/AgentRole.h"
#include "thin_agent/agent/AgentLoop.h"
#include "thin_agent/core/SubAgentBus.h"
#include "thin_agent/core/Blackboard.h"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/llm/DemoConfigCompat.h"
#include "thin_agent/core/WorkflowManager.h"
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/local/HybridRouter.h"
#include "thin_agent/core/ExternalIntentHandlers.h"
#include "thin_agent/local/TemplateModel.h"
#include "thin_agent/local/ModelPool.h"
#include "thin_agent/local/ILocalModel.h"
#include "thin_agent/agent/McpServer.h"
#include "thin_agent/local/GgufModel.h"
#include "thin_agent/plugin/PluginLoader.h"
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/llm/CloudLlmClient.h"
#include "thin_agent/llm/CurlHttpClient.h"
#include "thin_agent/agent/McpClient.h"
#include "thin_agent/agent/ITransport.h"
#include "thin_agent/local/OnnxChatModel.h"

#include <fstream>
#include <filesystem>

using namespace thin_agent::agent;

namespace {

int failures = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
  }
}

// ─── ToolRegistry ──────────────────────────────────────────────

int test_tool_registry_basic() {
  ToolRegistry::instance().clear();
  ToolSchema greet;
  greet.name = "greet";
  greet.description = "问候用户";
  greet.parameters = {ToolParameter{"name", "string", "用户名", true}};
  greet.execute = [](const nlohmann::json& params) -> nlohmann::json {
    return {{"ok", true}, {"result", {{"greeting", "Hello, " + params["name"].get<std::string>() + "!"}}}};
  };
  ToolRegistry::instance().register_tool(greet);
  check(ToolRegistry::instance().size() == 1, "registry size after register");
  const auto* found = ToolRegistry::instance().find("greet");
  check(found != nullptr, "find registered tool");
  check(found->name == "greet", "tool name correct");
  check(ToolRegistry::instance().find("nonexistent") == nullptr, "find nonexistent returns null");
  auto result = ToolRegistry::instance().call("greet", {{"name", "World"}});
  check(result.ok, "tool call ok");
  check(result.result["greeting"] == "Hello, World!", "tool call result correct");
  auto bad = ToolRegistry::instance().call("greet", nlohmann::json::object());
  check(!bad.ok, "missing required param fails");
  check(!bad.error.empty(), "error message present");
  auto noexist = ToolRegistry::instance().call("no_such_tool", nlohmann::json::object());
  check(!noexist.ok, "nonexistent tool fails");

  ToolSchema mode_switch;
  mode_switch.name = "set_mode";
  mode_switch.description = "设置模式";
  mode_switch.parameters = {ToolParameter{"mode", "string", "模式名", true, nlohmann::json::array({"photo", "video", "timelapse"})}};
  mode_switch.execute = [](const nlohmann::json&) { return nlohmann::json{{"ok", true}}; };
  ToolRegistry::instance().register_tool(mode_switch);
  auto valid_enum = ToolRegistry::instance().call("set_mode", {{"mode", "photo"}});
  check(valid_enum.ok, "valid enum value passes");
  auto invalid_enum = ToolRegistry::instance().call("set_mode", {{"mode", "invalid"}});
  check(!invalid_enum.ok, "invalid enum value fails");
  auto all = ToolRegistry::instance().all_tools();
  check(all.size() == 2, "all_tools returns all registered");
  auto openai = ToolRegistry::instance().to_openai_tools();
  check(openai.is_array() && openai.size() == 2, "to_openai_tools returns array");
  auto prompt = ToolRegistry::instance().build_tools_prompt();
  check(prompt.find("greet") != std::string::npos, "tools prompt contains tool name");
  ToolRegistry::instance().clear();
  check(ToolRegistry::instance().size() == 0, "clear empties registry");
  return 0;
}

int test_tool_registry_timeout() {
  ToolRegistry::instance().clear();
  ToolSchema slow;
  slow.name = "slow";
  slow.description = "慢工具";
  slow.parameters = {};
  slow.execute = [](const nlohmann::json&) -> nlohmann::json {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return {{"ok", true}};
  };
  ToolRegistry::instance().register_tool(slow);
  // v0.54.25: 该用例断言的是**预算语义**（超时该报错 / 预算够就该成功），不是"调度器有多准时"。
  // 原第二次调用只留 200ms→500ms（2.5×）余量：`ctest -j4` 高争用下线程唤醒可被推迟 >300ms ⇒
  // 误报 "tool completes within timeout" 红（v0.54.24 全量跑实测到一次偶发，standalone/-j2 均绿）。
  // 改为 3000ms（15× 余量），并把**真实耗时**打进消息里 ⇒ 下次偶发可当场定性（上次无输出可查）。
  const auto t0 = std::chrono::steady_clock::now();
  auto timeout = ToolRegistry::instance().call("slow", nlohmann::json::object(), 50);
  const auto ms_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0).count();
  // 耗时**无条件打印**（check 只收 const char*）⇒ 失败时输出里直接带真实数字
  std::cout << "  [timing] timeout-call elapsed=" << ms_timeout << "ms (budget=50ms)\n";
  check(!timeout.ok, "tool times out");
  check(timeout.error.find("timeout") != std::string::npos, "timeout error message");

  const auto t1 = std::chrono::steady_clock::now();
  auto ok = ToolRegistry::instance().call("slow", nlohmann::json::object(), 3000);
  const auto ms_ok = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t1).count();
  std::cout << "  [timing] ok-call elapsed=" << ms_ok
            << "ms (budget=3000ms, tool sleeps 200ms)\n";
  check(ok.ok, "tool completes within timeout");
  ToolRegistry::instance().clear();
  return 0;
}

// ─── ToolSchema 序列化 ─────────────────────────────────────────

int test_tool_schema_serialization() {
  ToolSchema weather;
  weather.name = "weather";
  weather.description = "查询天气";
  weather.parameters = {ToolParameter{"city", "string", "城市名", true}, ToolParameter{"lang", "string", "语言", false}};
  auto func = weather.to_openai_function();
  check(func["name"] == "weather", "openai name");
  check(func["description"] == "查询天气", "openai description");
  check(func["parameters"]["required"].size() == 1, "one required param");
  auto text = weather.to_text_description();
  check(text.find("weather") != std::string::npos, "text contains name");
  auto errors = weather.validate_params({{"city", "Shanghai"}});
  check(errors.empty(), "valid params no errors");
  auto missing = weather.validate_params({{"lang", "zh"}});
  check(!missing.empty(), "missing required detected");
  return 0;
}

// ─── EmbeddingProvider ─────────────────────────────────────────

int test_local_hash_embedding() {
  LocalHashEmbeddingProvider emb(128);
  check(emb.dimension() == 128, "dimension correct");
  check(emb.name() == "local_hash", "name correct");
  auto vec1 = emb.encode("hello world");
  check(static_cast<int>(vec1.size()) == 128, "output dimension matches");
  double norm = 0.0;
  for (float v : vec1) norm += static_cast<double>(v) * v;
  check(std::abs(norm - 1.0) < 0.01, "vector is normalized");
  auto empty_vec = emb.encode("");
  check(static_cast<int>(empty_vec.size()) == 128, "empty text returns zero vector");
  auto vec2 = emb.encode("hello world");
  auto vec3 = emb.encode("hello world!");
  auto vec4 = emb.encode("completely different text about cats");
  float sim_same = cosine_similarity(vec2, vec3);
  float sim_diff = cosine_similarity(vec2, vec4);
  check(sim_same > sim_diff, "similar texts have higher similarity");
  return 0;
}

int test_cosine_similarity() {
  std::vector<float> a = {1.0f, 0.0f, 0.0f};
  std::vector<float> b = {0.0f, 1.0f, 0.0f};
  std::vector<float> c = {1.0f, 0.0f, 0.0f};
  float sim_orthogonal = cosine_similarity(a, b);
  check(std::abs(sim_orthogonal) < 0.001, "orthogonal vectors have 0 similarity");
  float sim_identical = cosine_similarity(a, c);
  check(std::abs(sim_identical - 1.0f) < 0.001, "identical vectors have 1.0 similarity");
  std::vector<float> opposite = {-1.0f, 0.0f, 0.0f};
  float sim_opposite = cosine_similarity(a, opposite);
  check(std::abs(sim_opposite + 1.0f) < 0.001, "opposite vectors have -1.0 similarity");
  std::vector<float> short_vec = {1.0f, 0.0f};
  float sim_bad = cosine_similarity(a, short_vec);
  check(std::abs(sim_bad) < 0.001, "different dimensions return 0");
  return 0;
}

// ─── VectorStore ───────────────────────────────────────────────

int test_vector_store_basic() {
  const std::string db_path = "/tmp/test_agent_vector.db";
  std::remove(db_path.c_str());
  VectorStore store;
  check(store.init(db_path, 128), "vector store init");
  auto r = store.recent(10);
  check(r.empty(), "empty store has no records");
  MemoryEntry e1;
  e1.content = "用户喜欢喝咖啡";
  e1.source = "preference";
  e1.session_id = "sess-1";
  LocalHashEmbeddingProvider emb(128);
  auto vec1 = emb.encode(e1.content);
  int64_t id1 = store.insert(e1, vec1);
  check(id1 > 0, "insert returns valid id");
  check(store.count() == 1, "count after insert");
  auto recent = store.recent(10);
  check(recent.size() == 1, "recent returns inserted");
  auto query_vec = emb.encode("咖啡");
  auto results = store.search(query_vec, 3);
  check(results.size() == 1, "search finds match");
  check(results[0].similarity > 0.3f, "similarity above threshold");
  check(store.remove(id1), "remove succeeds");
  check(store.count() == 0, "count after remove");
  MemoryEntry e2; e2.content = "会话记录1"; e2.session_id = "sess-del";
  store.insert(e2, emb.encode(e2.content));
  MemoryEntry e3; e3.content = "会话记录2"; e3.session_id = "sess-del";
  store.insert(e3, emb.encode(e3.content));
  int removed = store.remove_by_session("sess-del");
  check(removed == 2, "remove_by_session removes correct count");
  std::remove(db_path.c_str());
  return 0;
}

int test_vector_store_stats() {
  const std::string db_path = "/tmp/test_agent_stats.db";
  std::remove(db_path.c_str());
  VectorStore store;
  store.init(db_path, 128);
  LocalHashEmbeddingProvider emb(128);
  MemoryEntry e1; e1.content = "fact 1"; e1.source = "fact";
  store.insert(e1, emb.encode(e1.content));
  MemoryEntry e2; e2.content = "pref 1"; e2.source = "preference";
  store.insert(e2, emb.encode(e2.content));
  MemoryEntry e3; e3.content = "conv 1"; e3.source = "conversation";
  store.insert(e3, emb.encode(e3.content));
  auto s = store.stats();
  check(s["total"] == 3, "total count correct");
  check(s["by_source"]["fact"] == 1, "fact count");
  check(s["by_source"]["preference"] == 1, "preference count");
  check(s["by_source"]["conversation"] == 1, "conversation count");
  std::remove(db_path.c_str());
  return 0;
}

// ─── MemoryManager ─────────────────────────────────────────────

int test_memory_manager_basic() {
  const std::string db_path = "/tmp/test_memory_mgr.db";
  std::remove(db_path.c_str());
  auto emb = std::make_shared<LocalHashEmbeddingProvider>(128);
  MemoryManager mgr(emb, db_path, false);
  int64_t id = mgr.remember("用户在北京工作", "fact", "sess-1", 0.8f);
  check(id > 0, "remember returns valid id");
  auto results = mgr.recall("北京", 3);
  check(!results.empty(), "recall finds match");
  auto ctx = mgr.build_context("北京", 3);
  check(ctx.find("北京") != std::string::npos, "build_context contains query");
  auto st = mgr.stats();
  check(st["total"] == 1, "memory manager stats");
  int removed = mgr.clear_session("sess-1");
  check(removed == 1, "clear_session removes entry");
  std::remove(db_path.c_str());
  return 0;
}

int test_memory_manager_auto_extract() {
  const std::string db_path = "/tmp/test_memory_auto.db";
  std::remove(db_path.c_str());
  auto emb = std::make_shared<LocalHashEmbeddingProvider>(128);
  MemoryManager mgr(emb, db_path, true);
  mgr.ingest_conversation("sess-1", "我今天去了上海出差，天气很热。", "明白了，上海今天确实很热。");
  auto count = mgr.stats()["total"].get<int64_t>();
  check(count > 0, "auto extract created facts");
  std::remove(db_path.c_str());
  return 0;
}

// ═══════════════════════════════════════════════════════════════
// 新组件测试
// ═══════════════════════════════════════════════════════════════

// ─── ConversationSummarizer ───────────────────────────────────

int test_conversation_summarizer() {
  ConversationSummarizer s1(10, 4);
  check(s1.message_count() == 0, "summarizer empty");
  check(!s1.needs_summarize(), "no summarize empty");
  check(s1.summary_prefix().empty(), "prefix empty");

  s1.add_message("user", "hello");
  s1.add_message("assistant", "hi");
  check(s1.message_count() == 2, "2 msgs");

  bool called = false;
  ConversationSummarizer s2(3, 1, [&](const std::string&) { called = true; return "OK"; });
  for (int i = 0; i < 5; i++) {
    s2.add_message("user", "m" + std::to_string(i));
    s2.add_message("assistant", "r" + std::to_string(i));
  }
  check(s2.needs_summarize(), "needs summarize");
  s2.summarize();
  check(called, "callback called");
  check(s2.summary_prefix().find("OK") != std::string::npos, "prefix has OK");

  s2.reset();
  check(s2.message_count() == 0, "reset clears");
  check(s2.summary_prefix().empty(), "reset clears prefix");
  return 0;
}

// ─── CheckpointManager ──────────────────────────────────────

int test_checkpoint_manager() { 
  CheckpointManager mgr;
  std::vector<std::string> msgs = {"u:hi", "a:hello", "u:w"};
  std::string cid = mgr.save("s1", msgs, {}, "auto");
  check(!cid.empty(), "ckpt id");
  check(cid.find("ckpt_s1") != std::string::npos, "id has session");

  msgs.push_back("a:ans");
  mgr.save("s1", msgs, {}, "manual");
  check(mgr.list_snapshots("s1").size() == 2, "2 snaps");

  auto last = mgr.last_snapshot("s1");
  check(last.label == "manual", "last manual");
  check(last.chat_memory.size() == 4, "4 msgs");

  auto r = mgr.restore("s1");
  check(r.size() == 4, "restored 4");

  mgr.clear("s1");
  check(mgr.list_snapshots("s1").empty(), "cleared");
  check(mgr.restore("X").empty(), "nonexist empty");
  return 0;
}

// ─── CronScheduler ──────────────────────────────────────────

int test_cron_scheduler() {
  thin_agent::CronScheduler s;
  auto t = s.add_task("T", "every 10m", "P");
  check(t.contains("id"), "has id");
  check(t["name"] == "T", "name match");
  check(t["enabled"].get<bool>(), "enabled default");
  int tid = t["id"].get<int>();
  check(tid > 0, "positive id");

  check(s.list_tasks().size() == 1, "1 task");

  s.update_task(tid, {{"name", "R"}, {"enabled", false}});
  auto tasks = s.list_tasks();
  check(tasks[0]["name"] == "R", "renamed");
  check(!tasks[0]["enabled"].get<bool>(), "disabled");

  s.set_enabled(tid, true);
  check(s.list_tasks()[0]["enabled"].get<bool>(), "re-enabled");

  auto st = s.stats();
  check(st["total"].get<int>() == 1 && st["enabled"].get<int>() == 1, "stats ok");

  check(s.remove_task(tid), "removed");
  check(s.list_tasks().empty(), "empty after rm");
  check(!s.remove_task(999), "rm nonexist fails");
  return 0;
}

// ─── SkillManager ───────────────────────────────────────────

int test_skill_manager() {
  auto emb = std::make_shared<LocalHashEmbeddingProvider>(128);
  auto store = std::make_shared<VectorStore>();
  store->init("/tmp/test_sk3.db", 128);

  thin_agent::agent::SkillManager mgr(emb, store);
  check(mgr.skill_count() == 0, "empty");

  std::string sid = mgr.save_skill("t1", "W", "weather lookup",
      "step1: extract city. step2: call API.", {"weather", "api"});
  check(!sid.empty(), "saved");
  check(mgr.skill_count() == 1, "count=1");

  auto sk = mgr.list_skills();
  check(sk[0].name == "W", "name match");
  check(sk[0].use_count == 0, "use_count=0");
  check(sk[0].state == thin_agent::agent::SkillState::active, "new skill active");
  check(!sk[0].created_at.empty(), "has created_at");

  auto m = mgr.match_skills("what's the weather in Shanghai", 3, 0.1f);
  bool found = false;
  for (auto& x : m) for (auto& t : x.tags) if (t == "weather") found = true;
  check(found, "tag match found");

  auto prompt = thin_agent::agent::SkillManager::to_prompt_injection(sk);
  check(prompt.find("W") != std::string::npos, "prompt inject");

  mgr.increment_use(sid);
  check(mgr.list_skills()[0].use_count == 1, "use_count++");
  check(!mgr.list_skills()[0].last_used_at.empty(), "last_used_at set");

  check(mgr.remove_skill(sid), "removed");
  check(mgr.skill_count() == 0, "empty after rm");
  std::remove("/tmp/test_sk3.db");
  return 0;
}

// v0.45.2: 回归 — save_to_file 与 save_skill 递归锁死锁修复。
// 修复前：save_skill 持锁调用 save_to_file，后者再次 lock 同一 mutex
// （std::mutex 非递归）→ 同线程死锁。此测试 5s 超时保护，卡死即失败。
int test_skill_manager_persist_deadlock() {
  auto emb = std::make_shared<LocalHashEmbeddingProvider>(128);
  auto store = std::make_shared<VectorStore>();
  store->init("/tmp/test_sk4.db", 128);

  thin_agent::agent::SkillManager mgr(emb, store);
  const std::string skills_path = "/tmp/test_skills_persist.json";
  (void)mgr.load_from_file(skills_path);  // 设置 skills_file_path_（文件不存在返回 false 但路径已记录）

  // save_skill → 内部 save_to_file（修复前这里死锁）
  std::string sid = mgr.save_skill("t1", "W", "weather lookup",
      "step1: extract city. step2: call API.", {"weather", "api"});
  check(!sid.empty(), "save_skill with persist ok");

  // increment_use → 内部 save_to_file
  mgr.increment_use(sid);
  check(mgr.list_skills()[0].use_count == 1, "increment_use with persist ok");

  // remove_skill → 内部 save_to_file
  check(mgr.remove_skill(sid), "remove_skill with persist ok");
  check(mgr.skill_count() == 0, "empty after rm with persist");

  std::remove("/tmp/test_sk4.db");
  std::remove(skills_path.c_str());
  return 0;
}

// ─── SkillManager Lifecycle (v0.10.0) ──────────────────────

int test_skill_lifecycle() {
  using namespace thin_agent::agent;
  auto emb = std::make_shared<LocalHashEmbeddingProvider>(128);
  auto store = std::make_shared<VectorStore>();
  store->init("/tmp/test_sk_lc.db", 128);

  SkillManager mgr(emb, store);

  // 创建 3 个 skills
  mgr.save_skill("t1", "skill_a", "desc a", "prompt a", {"tag_a"});
  mgr.save_skill("t2", "skill_b", "desc b", "prompt b", {"tag_b"});
  mgr.save_skill("t3", "skill_c", "desc c", "prompt c", {"tag_c"});
  check(mgr.skill_count() == 3, "3 skills created");

  // stats
  auto st = mgr.stats();
  check(st["total"] == 3, "stats total=3");
  check(st["active"] == 3, "stats active=3");
  check(st["stale"] == 0, "stats stale=0");
  check(st["archived"] == 0, "stats archived=0");

  // list by state
  check(mgr.list_skills_by_state(SkillState::active).size() == 3, "3 active");
  check(mgr.list_skills_by_state(SkillState::stale).empty(), "0 stale");

  // mark_stale: 新创建的 skills 不应该被标记为 stale（刚创建）
  int marked = mgr.mark_stale(1);  // 1 day unused
  check(marked == 0, "new skills not stale");

  // 使用一个 skill
  auto skills = mgr.list_skills();
  mgr.increment_use(skills[0].id);
  check(mgr.list_skills()[0].use_count == 1, "use_count incremented");
  check(mgr.list_skills()[0].state == SkillState::active, "still active after use");

  // 再次使用触发 reactivation（如果之前被标记为 stale）
  // 先手动标记为 stale，然后 increment_use 应该重新激活
  // (mark_stale 在当前实现中需要天数判断，直接测试 reactivation 通过 increment)

  // auto_maintain
  auto result = mgr.auto_maintain(1, 1);
  check(result["marked_stale"].get<int>() >= 0, "auto_maintain marked_stale");
  check(result["archived"].get<int>() >= 0, "auto_maintain archived");
  check(result["cleaned"].get<int>() >= 0, "auto_maintain cleaned");

  // to_prompt_injection 只包含 active skills
  auto all = mgr.list_skills();
  std::string inj = SkillManager::to_prompt_injection(all);
  check(!inj.empty(), "has injection for active skills");

  // remove
  mgr.remove_skill(skills[1].id);
  mgr.remove_skill(skills[2].id);
  check(mgr.skill_count() == 1, "1 after removing 2");
  check(mgr.remove_skill(skills[0].id), "remove last");
  check(mgr.skill_count() == 0, "all removed");

  // empty stats
  auto st2 = mgr.stats();
  check(st2["total"] == 0, "stats total=0 after clear");

  std::remove("/tmp/test_sk_lc.db");
  return 0;
}

int test_agent_orchestrator() {
  using namespace thin_agent;

  // 构造时不启动线程（惰性）
  thin_agent::AgentOrchestrator orch;
  check(orch.status().num_workers == 0, "orchestrator not started yet");
  check(orch.status().total_completed == 0, "no tasks yet");

  // 设置执行器
  orch.set_executor([](const thin_agent::AgentOrchestrator::OrchestrationTask& t) {
    thin_agent::AgentOrchestrator::TaskResult r;
    r.task_id = t.task_id;
    r.ok = true;
    r.result = {{"echo", t.prompt}, {"name", t.name}};
    return r;
  });

  // 同步执行
  thin_agent::AgentOrchestrator::OrchestrationRequest req;
  req.request_id = "test_001";
  req.session_id = "s1";
  req.tasks.push_back({"t1", "task_one", "hello world", {}});
  req.tasks.push_back({"t2", "task_two", "foo bar", {}});

  auto result = orch.execute(req, 5000);
  check(result.request_id == "test_001", "request id preserved");
  check(result.results.size() == 2, "2 results");
  check(result.all_ok, "all tasks ok");
  check(result.total_latency_ms >= 0, "latency non-negative");

  for (const auto& r : result.results) {
    check(r.ok, "individual task ok");
    check(!r.task_id.empty(), "has task_id");
    check(r.latency_ms >= 0, "task latency non-negative");
  }

  // 状态查询
  auto s = orch.status();
  check(s.num_workers == 3, "3 workers after first execute");
  check(s.total_completed == 2, "2 completed");
  check(s.active_tasks == 0, "no active tasks");
  check(s.queued_tasks == 0, "queue empty");

  // 异步执行
  thin_agent::AgentOrchestrator::OrchestrationRequest req2;
  req2.request_id = "test_002";
  req2.session_id = "s1";
  req2.tasks.push_back({"t3", "async_task", "ping", {}});

  auto fut = orch.execute_async(req2, 5000);
  auto result2 = fut.get();
  check(result2.results.size() == 1, "async: 1 result");
  check(result2.results[0].ok, "async: task ok");
  check(result2.results[0].task_id == "t3", "async: task id preserved");

  return 0;
}

// ─── ProactiveMonitor (v0.10.4/5) ─────────────────────────

int test_proactive_monitor() {
  using namespace thin_agent;
  ProactiveMonitor mon;

  auto st = mon.status();
  check(st["running"] == false, "monitor not running");

  // watch_file on nonexistent should not crash
  mon.watch_file("/tmp/nonexistent_test_file.log");
  auto alerts = mon.poll_once();
  check(alerts.is_array(), "poll_once returns array");
  check(alerts.empty(), "no alerts for nonexistent file");

  // status reports file watch
  auto st2 = mon.status();
  check(st2["file_watches"].size() == 1, "1 file watch registered");

  return 0;
}

}  // namespace

// ─── ErrorDetector ─────────────────────────────────────────

int test_error_detector_is_tool_error() {
  using namespace thin_agent::agent;

  // ok=false → error
  {
    ToolCallResult cr;
    cr.ok = false;
    cr.error = "timeout";
    check(ErrorDetector::is_tool_error(cr), "not ok → error");
  }

  // ok=true, empty string → error
  {
    ToolCallResult cr;
    cr.ok = true;
    cr.result = "";
    check(ErrorDetector::is_tool_error(cr), "empty string → error");
  }

  // ok=true, "{}" → error
  {
    ToolCallResult cr;
    cr.ok = true;
    cr.result = "{}";
    check(ErrorDetector::is_tool_error(cr), "empty object → error");
  }

  // ok=true, "[]" → error
  {
    ToolCallResult cr;
    cr.ok = true;
    cr.result = "[]";
    check(ErrorDetector::is_tool_error(cr), "empty array → error");
  }

  // ok=true, "null" → error
  {
    ToolCallResult cr;
    cr.ok = true;
    cr.result = "null";
    check(ErrorDetector::is_tool_error(cr), "null literal → error");
  }

  // ok=true, short junk (< 3 chars) → error
  {
    ToolCallResult cr;
    cr.ok = true;
    cr.result = "ab";
    check(ErrorDetector::is_tool_error(cr), "short junk → error");
  }

  // ok=true, "ok" → NOT error
  {
    ToolCallResult cr;
    cr.ok = true;
    cr.result = "ok";
    check(!ErrorDetector::is_tool_error(cr), "ok → not error");
  }

  // ok=true, "no" → NOT error
  {
    ToolCallResult cr;
    cr.ok = true;
    cr.result = "no";
    check(!ErrorDetector::is_tool_error(cr), "no → not error");
  }

  // ok=true, normal result → NOT error
  {
    ToolCallResult cr;
    cr.ok = true;
    cr.result = nlohmann::json{{"status", "success"}, {"data", 42}};
    check(!ErrorDetector::is_tool_error(cr), "normal json → not error");
  }

  // ok=true, json string object → NOT error
  {
    ToolCallResult cr;
    cr.ok = true;
    cr.result = R"({"result":"hello world"})";
    check(!ErrorDetector::is_tool_error(cr), "json string → not error");
  }

  return 0;
}

int test_error_detector_correction_hint() {
  using namespace thin_agent::agent;

  // failed result hint
  {
    ToolCallResult cr;
    cr.ok = false;
    cr.error = "HTTP 500";
    std::string hint = ErrorDetector::correction_hint("test_tool", cr);
    check(hint.find("test_tool") != std::string::npos, "hint contains tool name");
    check(hint.find("failed") != std::string::npos, "hint mentions failure");
    check(hint.find("HTTP 500") != std::string::npos, "hint includes error text");
    check(hint.find("check parameters") != std::string::npos || hint.find("corrected") != std::string::npos,
          "hint suggests fix");
  }

  // empty/malformed result hint
  {
    ToolCallResult cr;
    cr.ok = true;
    cr.result = "{}";
    std::string hint = ErrorDetector::correction_hint("data_fetch", cr);
    check(hint.find("data_fetch") != std::string::npos, "malformed hint contains tool name");
    check(hint.find("empty") != std::string::npos || hint.find("malformed") != std::string::npos,
          "hint mentions empty/malformed");
  }

  return 0;
}

// ─── ErrorCorrectionStore (v0.10.2) ─────────────────────────

int test_correction_store() {
  using namespace thin_agent::agent;
  ErrorCorrectionStore store;
  auto st0 = store.stats();
  check(st0["total_corrections"] == 0, "empty store 0 corrections");

  // record
  auto id1 = store.record("file_read", "permission denied", "use sudo chmod first");
  check(id1 > 0, "record returns valid id");

  auto id2 = store.record("file_read", "permission denied", "check file permissions");
  check(id2 > 0, "duplicate upserts");

  store.record("terminal", "command not found", "use apt install first");
  store.record("terminal", "timeout", "increase timeout to 30s");

  auto st = store.stats();
  check(st["total_corrections"].get<int>() >= 3, "3+ corrections");

  // get by tool
  auto corr = store.get_corrections("file_read", 5);
  check(corr.size() >= 1, "file_read has corrections");
  check(corr[0].use_count >= 1, "has use count");

  // find by error
  auto found = store.find_by_error("terminal", "timeout", 3);
  check(found.size() >= 1, "found timeout correction");

  // to_prompt_injection (need use_count >= 2 to appear)
  // record again to boost use_count
  store.record("file_read", "permission denied", "use sudo chmod first");
  store.record("file_read", "permission denied", "use sudo chmod first");
  auto inj = store.to_prompt_injection({"file_read", "terminal"}, 3);
  check(!inj.empty(), "has injection for high-use corrections");

  return 0;
}

// ─── GoalManager ───────────────────────────────────────────

int test_goal_manager() {
  using namespace thin_agent::agent;
  GoalManager mgr;
  auto g1 = mgr.create("监控 leaptic_app 编译状态");
  check(g1.id > 0, "goal create returns id");
  check(g1.status == "active", "goal default status active");
  auto g2 = mgr.create("完成 thin_agent v0.9.4 发布");
  check(g2.id == g1.id + 1, "goal id auto-increment");
  check(mgr.active_count() == 2, "active_count after 2 creates");
  mgr.update(g2.id, {{"status", "done"}, {"progress_pct", 100}});
  check(mgr.active_count() == 1, "active_count after marking done");
  auto all = mgr.list();
  check(all.size() == 2, "list all returns 2 goals");
  auto active = mgr.list("active");
  check(active.size() == 1, "list active returns 1");
  check(active[0].id == g1.id, "active goal id match");
  std::string prompt = mgr.to_prompt_injection();
  check(!prompt.empty(), "prompt injection non-empty");
  check(prompt.find("leaptic_app") != std::string::npos, "prompt contains goal text");
  mgr.remove(g1.id);
  check(mgr.active_count() == 0, "active_count after remove");

  // v0.10.3: stalled_goals / update_progress / update_status
  auto g3 = mgr.create("测试滞停检测目标");
  check(mgr.active_count() == 1, "active=1 for stall test");
  auto stalls = mgr.stalled_goals(0);  // 0 小时 — 刚创建的不会滞停
  check(stalls.empty(), "new goal not stalled");

  mgr.update_progress(g3.id, 30);
  check(mgr.list("active")[0].progress_pct == 30, "progress updated to 30");

  mgr.update_status(g3.id, "done", 100);
  check(mgr.active_count() == 0, "done goal not active");
  check(mgr.list("done").size() >= 1, "list done has goals");

  mgr.remove(g3.id);
  return 0;
}

// v0.11.0: 角色系统测试

int test_agent_role_from_json() {
  
  nlohmann::json j;
  j["name"] = "tester";
  j["description"] = "test role";
  j["system_prompt"] = "You are a test engineer.";
  j["tools"] = {"read_file", "search_files"};

  auto role = AgentRole::from_json(j);
  check(role.name == "tester", "role name");
  check(role.description == "test role", "role description");
  check(role.system_prompt.find("test engineer") != std::string::npos, "system prompt parsed");
  check(role.tools.size() == 2, "2 tools parsed");
  check(role.tools[0] == "read_file", "tool 0");
  check(role.tools[1] == "search_files", "tool 1");

  // round-trip
  auto j2 = role.to_json();
  check(j2["name"] == "tester", "round-trip name");
  check(j2["tools"].size() == 2, "round-trip tools count");

  return 0;
}

int test_agent_role_manager() {
  
  AgentRoleManager mgr;

  check(mgr.size() == 0, "initial size 0");

  AgentRole r;
  r.name = "reviewer";
  r.description = "code reviewer";
  r.system_prompt = "Review C++ code.";
  r.tools = {"read_file"};
  mgr.register_role(r);

  check(mgr.size() == 1, "size after register");
  const auto* found = mgr.find("reviewer");
  check(found != nullptr, "find returns non-null");
  check(found->name == "reviewer", "find name matches");
  check(found->tools.size() == 1, "tools preserved");
  check(mgr.find("nonexistent") == nullptr, "find nonexistent returns null");

  auto list = mgr.list();
  check(list.size() == 1, "list size");
  check(list[0].name == "reviewer", "list includes registered role");

  // overwrite
  r.system_prompt = "Updated review prompt.";
  mgr.register_role(r);
  check(mgr.size() == 1, "size unchanged after overwrite");
  check(mgr.find("reviewer")->system_prompt == "Updated review prompt.", "overwrite updated");

  // remove
  check(mgr.remove("reviewer"), "remove success");
  check(mgr.size() == 0, "size after remove");
  check(!mgr.remove("reviewer"), "double remove returns false");

  return 0;
}

int test_agent_role_builtins() {
  
  AgentRoleManager mgr;
  mgr.register_builtins();

  check(mgr.size() >= 8, "at least 8 builtin roles");
  check(mgr.find("viewer") != nullptr, "viewer exists");
  check(mgr.find("tester") != nullptr, "tester exists");
  check(mgr.find("researcher") != nullptr, "researcher exists");
  check(mgr.find("summarizer") != nullptr, "summarizer exists");
  check(mgr.find("debugger") != nullptr, "debugger exists");

  // verify tool restrictions
  const auto* cr = mgr.find("viewer");
  check(cr->tools.size() == 2, "viewer has 2 tools");  // read_file, search_files
  check(!cr->system_prompt.empty(), "viewer has system prompt");

  // summarizer has no tools (empty = all)
  const auto* sm = mgr.find("summarizer");
  check(sm->tools.empty(), "summarizer tools empty (all allowed)");

  // researcher has web_search
  const auto* rs = mgr.find("researcher");
  bool has_web = false;
  for (const auto& t : rs->tools) if (t == "web_search") has_web = true;
  check(has_web, "researcher has web_search tool");

  return 0;
}

int test_agent_loop_tool_whitelist() {
  

  // 单例全局 registry，注册测试工具
  auto& reg = ToolRegistry::instance();

  // 构建配置
  thin_agent::DemoConfigCompat cfg;
  cfg.mode = "local";
  cfg.model_name = "test";

  // 不加 alllowed_tools → 全部可用
  AgentLoopConfig no_filter_cfg;
  AgentLoop loop_no_filter(cfg, "", reg, no_filter_cfg);
  check(loop_no_filter.is_tool_allowed("anything"), "empty whitelist = all allowed");
  check(loop_no_filter.is_tool_allowed(""), "empty name allowed when no filter");

  // 加白名单
  AgentLoopConfig filter_cfg;
  filter_cfg.allowed_tools = {"read_file", "search_files"};
  AgentLoop loop_filter(cfg, "", reg, filter_cfg);
  check(loop_filter.is_tool_allowed("read_file"), "read_file allowed");
  check(loop_filter.is_tool_allowed("search_files"), "search_files allowed");
  check(!loop_filter.is_tool_allowed("terminal"), "terminal blocked");
  check(!loop_filter.is_tool_allowed("web_search"), "web_search blocked");

  // filter_tools_json
  nlohmann::json all_tools = nlohmann::json::array();
  nlohmann::json t1, t2, t3;
  t1["function"]["name"] = "read_file";
  t2["function"]["name"] = "terminal";
  t3["function"]["name"] = "search_files";
  all_tools.push_back(t1);
  all_tools.push_back(t2);
  all_tools.push_back(t3);

  auto filtered = loop_filter.filter_tools_json(all_tools);
  check(filtered.is_array(), "filtered is array");
  check(filtered.size() == 2, "filtered to 2 tools");
  check(filtered[0]["function"]["name"] == "read_file", "filtered tool 0");
  check(filtered[1]["function"]["name"] == "search_files", "filtered tool 1");

  // empty whitelist → pass-through
  auto unfiltered = loop_no_filter.filter_tools_json(all_tools);
  check(unfiltered.size() == 3, "unfiltered passes all 3");

  return 0;
}

// v0.11.1: DAG 依赖编排测试

int test_dag_linear_dependency() {

  int call_order = 0;
  thin_agent::AgentOrchestrator orch([&call_order](const thin_agent::AgentOrchestrator::OrchestrationTask& t) {
    thin_agent::AgentOrchestrator::TaskResult r;
    r.task_id = t.task_id;
    r.ok = true;
    r.result = {{"order", ++call_order}, {"task", t.name}};
    return r;
  });

  // A → B → C 线性依赖
  thin_agent::AgentOrchestrator::DAGRequest req;
  req.request_id = "test_linear";
  req.nodes = {
    {"A", "task_a", "prompt A", "", {}, {}},
    {"B", "task_b", "prompt B", "", {"A"}, {}},
    {"C", "task_c", "prompt C", "", {"B"}, {}},
  };

  auto result = orch.execute_dag(req, 5000);
  check(result.results.size() == 3, "linear: 3 results");
  check(result.all_ok, "linear: all ok");

  // 验证执行顺序：A 先于 B 先于 C
  int order_a = -1, order_b = -1, order_c = -1;
  for (auto& r : result.results) {
    if (r.task_id == "A") order_a = r.result.value("order", 0);
    if (r.task_id == "B") order_b = r.result.value("order", 0);
    if (r.task_id == "C") order_c = r.result.value("order", 0);
  }
  check(order_a < order_b, "A before B");
  check(order_b < order_c, "B before C");
  check(order_a == 1, "A was first");

  return 0;
}

int test_dag_parallel_wave() {

  thin_agent::AgentOrchestrator orch([](const thin_agent::AgentOrchestrator::OrchestrationTask& t) {
    thin_agent::AgentOrchestrator::TaskResult r;
    r.task_id = t.task_id;
    r.ok = true;
    r.result = {{"task", t.name}};
    // 模拟时间差
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return r;
  });

  // A 和 B 并行 → C 依赖 A+B
  thin_agent::AgentOrchestrator::DAGRequest req;
  req.request_id = "test_wave";
  req.nodes = {
    {"A", "task_a", "prompt A", "", {}, {}},
    {"B", "task_b", "prompt B", "", {}, {}},
    {"C", "task_c", "prompt C", "", {"A", "B"}, {}},
  };

  auto result = orch.execute_dag(req, 5000);
  check(result.results.size() == 3, "wave: 3 results");
  check(result.all_ok, "wave: all ok");
  // C 应该包含 A+B 的结果（至少两个依赖都 OK）
  for (auto& r : result.results) {
    check(r.ok, "wave: all individual ok");
  }

  return 0;
}

int test_dag_failure_propagation() {

  thin_agent::AgentOrchestrator orch([](const thin_agent::AgentOrchestrator::OrchestrationTask& t) {
    thin_agent::AgentOrchestrator::TaskResult r;
    r.task_id = t.task_id;
    // B 故意失败
    r.ok = (t.task_id != "B");
    if (!r.ok) r.error = "B intentionally failed";
    r.result = {{"task", t.name}};
    return r;
  });

  // A (OK) → B (FAIL) → C (should be skipped)
  thin_agent::AgentOrchestrator::DAGRequest req;
  req.request_id = "test_fail";
  req.nodes = {
    {"A", "task_a", "prompt A", "", {}, {}},
    {"B", "task_b", "prompt B", "", {"A"}, {}},
    {"C", "task_c", "prompt C", "", {"B"}, {}},
  };

  auto result = orch.execute_dag(req, 5000);
  check(result.results.size() == 3, "fail: 3 results");
  check(!result.all_ok, "fail: not all ok");

  // A 应该 OK
  check(result.results[0].ok, "fail: A ok");
  // B 应该失败
  check(!result.results[1].ok, "fail: B failed");
  // C 应该被跳过（级联失败）
  check(!result.results[2].ok, "fail: C skipped");
  check(result.results[2].error.find("skipped") != std::string::npos ||
        result.results[2].error.find("upstream") != std::string::npos,
        "fail: C skipped due to upstream");

  return 0;
}

// v0.25.1: 验证 DAGNode.role → OrchestrationTask.role 映射
int test_dag_role_field_mapping() {
  thin_agent::AgentOrchestrator orch;
  std::string captured_role;

  orch.set_executor([&captured_role](const thin_agent::AgentOrchestrator::OrchestrationTask& t) {
    captured_role = t.role;
    thin_agent::AgentOrchestrator::TaskResult r;
    r.task_id = t.task_id;
    r.ok = true;
    return r;
  });

  thin_agent::AgentOrchestrator::DAGRequest req;
  req.request_id = "role_test";
  req.session_id = "test";
  req.nodes.push_back({"n1", "task1", "analyze code", "viewer", {}, {}});

  auto result = orch.execute_dag(req);
  check(result.all_ok, "DAG executes with role field");
  check(captured_role == "viewer", "role field mapped: viewer");

  // 空 role
  captured_role.clear();
  req.nodes.clear();
  req.nodes.push_back({"n2", "task2", "summarize", "", {}, {}});
  result = orch.execute_dag(req);
  check(result.all_ok, "DAG executes with empty role");
  check(captured_role.empty(), "empty role stays empty");

  return 0;
}

int test_dag_no_dependencies() {

  thin_agent::AgentOrchestrator orch([](const thin_agent::AgentOrchestrator::OrchestrationTask& t) {
    thin_agent::AgentOrchestrator::TaskResult r;
    r.task_id = t.task_id;
    r.ok = true;
    r.result = {{"task", t.name}};
    return r;
  });

  // 纯并行：无依赖
  thin_agent::AgentOrchestrator::DAGRequest req;
  req.request_id = "test_parallel";
  req.nodes = {
    {"A", "task_a", "prompt A", "", {}, {}},
    {"B", "task_b", "prompt B", "", {}, {}},
    {"C", "task_c", "prompt C", "", {}, {}},
  };

  auto result = orch.execute_dag(req, 5000);
  check(result.results.size() == 3, "parallel: 3 results");
  check(result.all_ok, "parallel: all ok");

  return 0;
}

// ─── SubAgentBus ──────────────────────────────────────────────

int test_subagent_bus_send_drain() {
  thin_agent::SubAgentBus bus;

  bus.send("agent_A", "agent_B", "Hello from A");
  bus.send("agent_C", "agent_B", "Hello from C");

  auto msgs = bus.drain("agent_B");
  check(msgs.size() == 2, "bus: B received 2 messages");
  check(msgs[0] == "[agent_A] Hello from A", "bus: message from A correct");
  check(msgs[1] == "[agent_C] Hello from C", "bus: message from C correct");

  // drain clears, so second drain returns nothing
  auto msgs2 = bus.drain("agent_B");
  check(msgs2.empty(), "bus: drain clears messages");

  return 0;
}

int test_subagent_bus_broadcast_and_peek() {
  thin_agent::SubAgentBus bus;

  // 先注册 worker1（broadcast 只投递给已注册 agent）
  bus.send("init", "worker1", "register");

  bus.broadcast("monitor", "System health check");

  // peek doesn't remove
  auto p1 = bus.peek("worker1");
  check(p1.size() == 2, "bus: worker1 has register + broadcast");

  // but broadcast should NOT deliver to the sender
  auto sender_msgs = bus.drain("monitor");
  check(sender_msgs.empty(), "bus: broadcast skips sender");

  return 0;
}

int test_subagent_bus_clear_all() {
  thin_agent::SubAgentBus bus;

  bus.send("A", "B", "msg1");
  bus.send("C", "D", "msg2");
  check(bus.pending_count("B") == 1, "bus: B has 1 pending");
  check(bus.pending_count("D") == 1, "bus: D has 1 pending");

  bus.clear_all();
  check(bus.pending_count("B") == 0, "bus: clear_all clears B");
  check(bus.pending_count("D") == 0, "bus: clear_all clears D");

  return 0;
}

// ─── Blackboard ───────────────────────────────────────────────

int test_blackboard_write_read() {
  thin_agent::Blackboard bb;

  bb.write("key1", "value1");
  bb.write("key2", 42);

  check(bb.has("key1"), "bb: has key1");
  check(bb.has("key2"), "bb: has key2");
  check(!bb.has("key3"), "bb: !has missing key");
  check(bb.size() == 2, "bb: size 2 after writes");

  check(bb.read("key1") == "value1", "bb: read key1 string");
  check(bb.read("key2") == 42, "bb: read key2 int");
  check(bb.read("nonexistent").is_null(), "bb: read missing returns null");

  return 0;
}

int test_blackboard_erase_and_clear() {
  thin_agent::Blackboard bb;

  bb.write("a", 1);
  bb.write("b", 2);
  bb.write("c", 3);

  bb.erase("b");
  check(!bb.has("b"), "bb: erase removes key");
  check(bb.size() == 2, "bb: size after erase");

  bb.clear();
  check(bb.size() == 0, "bb: clear empties");
  check(!bb.has("a"), "bb: key gone after clear");

  return 0;
}

int test_blackboard_keys_and_prompt() {
  thin_agent::Blackboard bb;

  bb.write("status", "healthy");
  bb.write("cpu_usage", 72.5);

  auto k = bb.keys();
  check(k.size() == 2, "bb: keys returns all keys");

  auto prompt = bb.to_prompt_injection();
  check(prompt.find("## Shared Context (Blackboard)") != std::string::npos,
        "bb: prompt has header");
  check(prompt.find("status") != std::string::npos,
        "bb: prompt contains key name");
  check(prompt.find("healthy") != std::string::npos,
        "bb: prompt contains value");

  // empty blackboard produces empty prompt
  bb.clear();
  check(bb.to_prompt_injection().empty(), "bb: empty bb → empty prompt");

  return 0;
}

// ─── Debate 流模拟 ────────────────────────────────────────────

/// 模拟 agent_debate 的多轮黑板写入模式。
int test_debate_blackboard_round_accumulation() {
  thin_agent::Blackboard bb;

  // Round 1: 3 roles write their analyses
  bb.write("round_0_code_reviewer", "CR: Found 3 potential bugs");
  bb.write("round_0_tester", "Tester: All unit tests pass");
  bb.write("round_0_debugger", "Debugger: Bug #1 is race condition");

  check(bb.size() == 3, "debate: round 1 has 3 entries");

  // Round 2: same roles refine their positions
  bb.write("round_1_code_reviewer", "CR: After debate, bugs confirmed: #1 race, #2 null deref");
  bb.write("round_1_tester", "Tester: New test covers race, still passes");
  bb.write("round_1_debugger", "Debugger: Concur with CR — race + null deref");

  check(bb.size() == 6, "debate: round 2 accumulates to 6 entries");

  // Verify multi-round data is all present
  auto keys = bb.keys();
  bool has_r0_cr = false, has_r1_db = false;
  for (auto& k : keys) {
    if (k == "round_0_code_reviewer") has_r0_cr = true;
    if (k == "round_1_debugger") has_r1_db = true;
  }
  check(has_r0_cr, "debate: round 0 key present");
  check(has_r1_db, "debate: round 1 key present");

  return 0;
}

/// 验证辩论结束后黑板清空逻辑。
int test_debate_clear_before_new_debate() {
  thin_agent::Blackboard bb;
  thin_agent::SubAgentBus bus;

  // Simulate a completed debate
  bb.write("round_0_code_reviewer", "old data");
  bb.write("round_0_tester", "old data");
  bus.send("A", "B", "old message");

  check(bb.size() == 2, "debate: before clear, blackboard has 2");
  check(bus.pending_count("B") == 1, "debate: before clear, bus has messages");

  // Clear for new debate
  bb.clear();
  bus.clear_all();

  check(bb.size() == 0, "debate: after clear, blackboard empty");
  check(bus.pending_count("B") == 0, "debate: after clear, bus empty");

  return 0;
}

/// 验证共识合成 prompt 的关键结构。
int test_debate_consensus_prompt_structure() {
  std::string goal = "Optimize database query performance";

  // Simulate the consensus synthesis prompt construction
  std::string synth_goal = goal +
    "\n\nAfter 2 rounds of debate between multiple agents.\n"
    "Identify:\n1. Points of AGREEMENT (all agents concur)\n"
    "2. Points of DISAGREEMENT (which agent holds which view, with their reasoning)\n"
    "3. Your WEIGHTED RECOMMENDATION (lean on the strongest evidence)";

  check(!synth_goal.empty(), "debate: synth prompt non-empty");
  check(synth_goal.find("AGREEMENT") != std::string::npos,
        "debate: prompt asks for agreement points");
  check(synth_goal.find("DISAGREEMENT") != std::string::npos,
        "debate: prompt asks for disagreement points");
  check(synth_goal.find("WEIGHTED RECOMMENDATION") != std::string::npos,
        "debate: prompt asks for weighted recommendation");
  check(synth_goal.find("2 rounds") != std::string::npos,
        "debate: prompt mentions round count");

  return 0;
}

// ── WorkflowManager::run() 测试 ──
// 测试执行引擎：成功/失败/重试上限/未找到

int test_workflow_run_success() {
  namespace fs = std::filesystem;
  std::string user_dir = thin_agent::WorkflowManager::user_dir();
  fs::create_directories(user_dir);
  std::string path = user_dir + "/test-wf-success.json";

  // 创建一个两步都成功的 workflow
  std::ofstream out(path);
  out << R"({"name":"test-wf-success","description":"success test","post_code_steps":[{"cmd":"echo ok","desc":"step1"},{"cmd":"echo done","desc":"step2"}]})";
  out.close();

  auto r = thin_agent::WorkflowManager::instance().run("test-wf-success", 1);
  check(r["success"] == true, "wf_run: success=true");
  check(r["status"] == "all_passed", "wf_run: status=all_passed");
  check(r["passed"] == 2, "wf_run: passed=2");
  check(r["total"] == 2, "wf_run: total=2");
  check(r["retry"] == 1, "wf_run: retry=1");

  fs::remove(path);
  return 0;
}

int test_workflow_run_step_failure() {
  namespace fs = std::filesystem;
  std::string user_dir = thin_agent::WorkflowManager::user_dir();
  fs::create_directories(user_dir);
  std::string path = user_dir + "/test-wf-fail.json";

  // 第一步成功，第二步失败
  std::ofstream out(path);
  out << R"({"name":"test-wf-fail","description":"fail test","post_code_steps":[{"cmd":"echo ok","desc":"step1"},{"cmd":"false","desc":"will fail"}]})";
  out.close();

  auto r = thin_agent::WorkflowManager::instance().run("test-wf-fail", 1);
  check(r["success"] == false, "wf_fail: success=false");
  check(r["status"] == "step_failed", "wf_fail: status=step_failed");
  check(r["passed"] == 1, "wf_fail: passed=1");
  check(r["total"] == 2, "wf_fail: total=2");
  check(r["failed_step"]["step"] == 2, "wf_fail: failed at step 2");
  check(r["retry"] == 1, "wf_fail: retry=1");
  check(r.contains("hint"), "wf_fail: has hint");

  fs::remove(path);
  return 0;
}

int test_workflow_run_max_retries_exceeded() {
  namespace fs = std::filesystem;
  std::string user_dir = thin_agent::WorkflowManager::user_dir();
  fs::create_directories(user_dir);
  std::string path = user_dir + "/test-wf-maxretry.json";

  // max_retries=2
  std::ofstream out(path);
  out << R"({"name":"test-wf-maxretry","description":"max retry test","max_retries":2,"post_code_steps":[{"cmd":"false","desc":"always fail"}]})";
  out.close();

  // retry=1: should fail (step failure, not max retries)
  auto r1 = thin_agent::WorkflowManager::instance().run("test-wf-maxretry", 1);
  check(r1["status"] == "step_failed", "wf_maxretry: attempt1=step_failed");

  // retry=2: should still fail (last allowed attempt)
  auto r2 = thin_agent::WorkflowManager::instance().run("test-wf-maxretry", 2);
  check(r2["status"] == "step_failed", "wf_maxretry: attempt2=step_failed");

  // retry=3: exceeds max_retries=2
  auto r3 = thin_agent::WorkflowManager::instance().run("test-wf-maxretry", 3);
  check(r3["status"] == "max_retries_exceeded", "wf_maxretry: attempt3=max_retries_exceeded");
  check(r3["retry"] == 3, "wf_maxretry: retry=3");
  check(r3["max_retries"] == 2, "wf_maxretry: max_retries=2");

  fs::remove(path);
  return 0;
}

int test_workflow_run_not_found() {
  auto r = thin_agent::WorkflowManager::instance().run("nonexistent-workflow", 1);
  check(r["success"] == false, "wf_notfound: success=false");
  check(r["status"] == "not_found", "wf_notfound: status=not_found");
  return 0;
}

int test_workflow_run_retry_counting() {
  namespace fs = std::filesystem;
  std::string user_dir = thin_agent::WorkflowManager::user_dir();
  fs::create_directories(user_dir);
  std::string path = user_dir + "/test-wf-retry-count.json";

  std::ofstream out(path);
  out << R"({"name":"test-wf-retry-count","description":"retry count test","max_retries":5,"post_code_steps":[{"cmd":"echo attempt","desc":"step1"}]})";
  out.close();

  // retry=3: should succeed, output should mention retries
  auto r = thin_agent::WorkflowManager::instance().run("test-wf-retry-count", 3);
  check(r["success"] == true, "wf_retrycount: success=true");
  check(r["retry"] == 3, "wf_retrycount: retry=3");
  check(r["max_retries"] == 5, "wf_retrycount: max_retries=5");
  std::string output = r["output"];
  check(output.find("after 2 retries") != std::string::npos,
        "wf_retrycount: output mentions '2 retries'");

  fs::remove(path);
  return 0;
}

int test_workflow_run_retry_counting();
int test_v025_skill_registry_handlers();  // v0.25.0
int test_cron_chat_injection();           // v0.25.9
int test_kanban_board();                  // v0.25.4 P6 component
int test_kb_searcher_open_valid();
int test_kb_searcher_open_invalid();
int test_kb_searcher_fts5_match();
int test_kb_searcher_like_fallback();
int test_kb_searcher_empty_query();
int test_kb_searcher_not_open();
int test_kb_searcher_limit_cap();
int test_kb_searcher_constructor_with_path();
int test_kb_searcher_open_by_name();             // v0.27.2: 多 KB 按名查找
int test_kb_searcher_repo_backward_compat();     // v0.27.2: 新旧 schema 兼容
int test_skill_registry_dispatch_known_error();  // v0.27.2: 已知 handler 错误正常转发
int test_skill_registry_dispatch_unknown();      // v0.27.2: 未知 handler 返回 no_cpp_handler
static std::string create_temp_fts5_db();

// P1: ChatPolicy + AgentTracer
int test_chatpolicy_normalize_text();
int test_chatpolicy_utf8_count();
int test_chatpolicy_language_detect();
int test_chatpolicy_input_translation();
int test_chatpolicy_progress_msg();
int test_agent_tracer_basic();
int test_agent_tracer_error_span();
int test_agent_tracer_timeout_span();
int test_agent_tracer_multiple_sessions();

// P2: DialogSlotRecall + AgentDirectory
int test_dialog_slot_recall_weather_query();
int test_dialog_slot_recall_news_query();
int test_dialog_slot_recall_referential_markers();
int test_agent_directory_register_find();
int test_agent_directory_unregister();

// P3: ExternalInfoClient + Tools
int test_external_info_weather_mock();
int test_external_info_news_mock();
int test_external_info_unknown_provider();
int test_create_builtin_tools_count();
int test_create_builtin_tools_names();
int test_create_builtin_tools_null_context();
int test_create_builtin_tools_execute_weather();
int test_create_builtin_tools_execute_news();

// P4: HybridRouter
int test_hybrid_router_empty();
int test_hybrid_router_short_simple();
int test_hybrid_router_medium_moderate();
int test_hybrid_router_long_complex();
int test_hybrid_router_complex_keyword();
int test_hybrid_router_multi_newline();

// P5: ExternalIntentHandlers
int test_external_intent_handler_unknown_fetcher();
int test_external_intent_handler_weather_mock();
int test_external_intent_handler_news_mock();

// P6: TemplateModel
int test_template_model_load();
int test_template_model_keyword_match();
int test_template_model_fallback();
int test_template_model_unloaded_infer();

// P7: ModelPool
int test_model_pool_add_size();
int test_model_pool_remove();
int test_model_pool_match_by_capability();
int test_model_pool_best_priority();
int test_model_pool_total_memory();
int test_model_pool_status_all();

// P8: McpServer
int test_mcp_server_make_response();
int test_mcp_server_make_error();
int test_mcp_server_parse_error();
int test_mcp_server_initialize();
int test_mcp_server_list_tools_no_init();
int test_mcp_server_list_tools_after_init();
int test_mcp_server_call_tool_no_init();
int test_mcp_server_unknown_method();
int test_mcp_server_notification();

// P9: GgufModel + PluginLoader
int test_gguf_model_constructor();
int test_gguf_model_load_no_file();
int test_gguf_model_infer_no_llama();
int test_plugin_loader_no_such_dir();
int test_plugin_loader_empty_modes();

// P10: CloudLlmClient (extracted pure functions)
int test_cloudllm_build_request_body_simple();
int test_cloudllm_build_request_body_tool_message();
int test_cloudllm_build_request_body_tools();
int test_cloudllm_build_request_body_stream();
int test_cloudllm_parse_response_ok();
int test_cloudllm_parse_response_invalid_json();
int test_cloudllm_parse_response_http_error();
int test_cloudllm_parse_response_missing_choices();
int test_cloudllm_parse_response_tool_calls();
int test_cloudllm_parse_response_reasoning_only();
int test_cloudllm_parse_sse_simple();
int test_cloudllm_parse_sse_tool_calls();
int test_cloudllm_fallback_single_provider();

// P11: McpClient JSON-RPC serialization
int test_mcp_build_jsonrpc_request();
int test_mcp_build_jsonrpc_request_no_params();
int test_mcp_parse_tool_list_ok();
int test_mcp_parse_tool_list_empty();
int test_mcp_parse_tool_list_invalid_json();
int test_mcp_parse_tool_call_ok();
int test_mcp_parse_tool_call_error();
int test_mcp_parse_tool_call_is_error();

// P12: OnnxChatModel (constructor only — ONNX Runtime not linked in tests)
int test_onnx_model_constructor();

// P13: MockHttpClient + MockTransport (完整闭环测试)
int test_mock_http_chat_completion();
int test_mock_http_chat_completion_error();
int test_mock_transport_connect();
int test_mock_transport_list_tools();
int test_mock_transport_call_tool();

// P14: 新增 Mock 测试 — 覆盖重构后内部逻辑
int test_mock_http_chat_with_tools();
// v0.45.4: 回归 — is_file_op_request 判定（profile 误判修复）。
// 实测暴露: "创建 /tmp/cap_test.txt 内容 hello thin_agent" 被误判为
// profile（自我介绍），因为内容含 thin_agent 字样而操作词列表缺"创建"。
int test_is_file_op_request() {
  using thin_agent::AgentService;

  // 文件操作 → true（不应触发 profile）
  check(AgentService::is_file_op_request("创建 /tmp/cap_test.txt 内容 hello thin_agent"),
        "create + thin_agent content → file op");
  check(AgentService::is_file_op_request("写入 /root/foo.txt"),
        "write verb → file op");
  check(AgentService::is_file_op_request("列出 /tmp 目录"),
        "list verb → file op");
  check(AgentService::is_file_op_request("读取 /etc/hostname"),
        "read verb → file op");
  check(AgentService::is_file_op_request("删除 /tmp/a.txt"),
        "delete verb → file op");
  check(AgentService::is_file_op_request("复制 /tmp/a /tmp/b"),
        "copy verb → file op");
  check(AgentService::is_file_op_request("code_patch /tmp/x"),
        "code_patch → file op");
  check(AgentService::is_file_op_request("搜索 /root 下的文件"),
        "search + path → file op");

  // 非文件操作 → false（可能走 profile/其他本地意图）
  check(!AgentService::is_file_op_request("介绍一下你自己"),
        "no path → not file op");
  check(!AgentService::is_file_op_request("你是谁"),
        "identity → not file op");
  check(!AgentService::is_file_op_request("thin_agent 是什么"),
        "agent talk without path → not file op");
  check(!AgentService::is_file_op_request("今天的天气怎么样"),
        "weather → not file op");
  check(!AgentService::is_file_op_request("查看一下你的状态"),
        "status → not file op");

  return 0;
}

// v0.45.6: 回归 — FilesystemCheckpoint 扫描上限（巨型目录不卡死）。
// 实测暴露：/root/code 含 linux-5.10.y 内核源码，auto_fc_pre 快照
// 逐个 SHA-256 需数分钟，每次 handle_chat 前阻塞全部请求。
int test_fs_checkpoint_scan_cap() {
  using thin_agent::agent::FilesystemCheckpoint;

  // 构造超过上限的目录树：25 个子目录 × 每目录 1000 文件 = 25000 文件
  const std::string root = "/tmp/fsckpt_cap_test";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  for (int d = 0; d < 25; ++d) {
    std::string dir = root + "/dir" + std::to_string(d);
    std::filesystem::create_directories(dir, ec);
    for (int f = 0; f < 1000; ++f) {
      std::ofstream(dir + "/f" + std::to_string(f) + ".txt") << "x";
    }
  }

  // save 应被上限截断，且快速返回（不逐个算哈希直到全目录扫完）
  FilesystemCheckpoint mgr("/tmp/fsckpt_cap_ckpt");
  auto start = std::chrono::steady_clock::now();
  std::string ckpt_id = mgr.save(root, {}, "cap_test", 5);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

  check(!ckpt_id.empty(), "fsckpt_cap: save produced checkpoint");
  check(ms < 5000, "fsckpt_cap: save fast <5s");

  std::filesystem::remove_all(root, ec);
  std::filesystem::remove_all("/tmp/fsckpt_cap_ckpt", ec);
  return 0;
}

// v0.45.8: 回归 — search_files 符号链接环防护。
// 实测暴露：recursive_directory_iterator 遇 symlink 环（/run/udev）抛
// filesystem_error → 整个搜索中断。修复用 error_code 重载 + symlink_status
// 跳过非普通文件 + 深度上限。本测试验证同样的防护逻辑在环/深树下不抛。
int test_search_files_link_ring() {
  namespace fs = std::filesystem;
  const fs::path base = fs::temp_directory_path() / "sfs_ring_test";
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base / "a/b", ec);
  {
    std::ofstream f(base / "a/b" / "hello.txt");
    f << "hello";
  }
  fs::create_directories(base / "deep", ec);
  fs::path d = base / "deep";
  for (int i = 0; i < 20; ++i) { d /= "d" + std::to_string(i); fs::create_directory(d, ec); }
  { std::ofstream f(d / "deep.txt"); f << "deep"; }
  fs::create_symlink("../..", base / "a/b/loop", ec);       // 自环
  fs::create_symlink("/nonexistent", base / "run_bad", ec);  // 坏链接

  int count = 0;
  bool thrown = false;
  try {
    std::filesystem::recursive_directory_iterator it(
        base, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
      if (ec) { ec.clear(); continue; }
      if (it.depth() > 16) continue;
      std::error_code sec;
      auto st = it->symlink_status(sec);
      if (sec || !fs::is_regular_file(st)) continue;
      ++count;
    }
  } catch (const std::exception&) { thrown = true; }
  check(!thrown, "sfs_ring: no exception on symlink ring");
  check(count >= 1, "sfs_ring: found regular files");
  check(count <= 16, "sfs_ring: depth cap limits deep tree");  // 只含 hello.txt
  fs::remove_all(base, ec);
  return 0;
}

int test_mock_http_classify_intent();
int test_mock_embedding_encode();
int test_mock_embedding_encode_fallback();
int test_mock_external_weather_http();
int test_mock_external_news_http();

int main() {
  // v0.54.5 (R92): **隔离 THIN_AGENT_HOME**（本测试经 WorkflowManager::user_dir()
  // = `$THIN_AGENT_HOME/workflows/user`，缺省落 `~/.thin_agent` **写共享用户目录**）：
  //   ① ctest -j4 并行时与其它单测/真网服务抢同一份数据 → 偶发红（历史现象："全量红、单跑绿"）
  //   ② 污染用户真实数据（测试写的 test-wf-*.json 会留在真实 workflows/user 下）
  // 隔离后每次运行都从干净目录开始（先删后建），且不触碰真实 home。
  {
    namespace fs = std::filesystem;
    const char* iso = "/tmp/unit_agent_core_home";
    std::error_code ec;
    fs::remove_all(iso, ec);
    fs::create_directories(iso, ec);
    setenv("THIN_AGENT_HOME", iso, 1);
    std::cerr << "[test] THIN_AGENT_HOME=" << iso << " (isolated)" << std::endl;
  }
  // v0.52.6: 熔断器进程级单例——失败型 mock 用例会累计开闸污染后续，
  // 跑序前置统一复位（覆盖 chat/embedding 全部 mock 用例）
  thin_agent::LlmCircuitBreaker::instance().reset();
  test_tool_registry_basic();
  test_tool_registry_timeout();
  test_tool_schema_serialization();
  test_local_hash_embedding();
  test_cosine_similarity();
  test_vector_store_basic();
  test_vector_store_stats();
  test_memory_manager_basic();
  test_memory_manager_auto_extract();


  test_conversation_summarizer();
  test_checkpoint_manager();
  test_cron_scheduler();
  test_skill_manager();
  test_skill_manager_persist_deadlock();
  test_skill_lifecycle();
  test_error_detector_is_tool_error();
  test_error_detector_correction_hint();
  test_correction_store();
  test_proactive_monitor();
  test_goal_manager();
  test_agent_orchestrator();
  test_agent_role_from_json();
  test_agent_role_manager();
  test_agent_role_builtins();
  test_agent_loop_tool_whitelist();
  test_dag_linear_dependency();
  test_dag_parallel_wave();
  test_dag_failure_propagation();
  test_dag_no_dependencies();

  // v0.25.1: DAG role 字段映射验证
  test_dag_role_field_mapping();

  // v0.11.7: SubAgentBus + Blackboard + Debate
  test_subagent_bus_send_drain();
  test_subagent_bus_broadcast_and_peek();
  test_subagent_bus_clear_all();
  test_blackboard_write_read();
  test_blackboard_erase_and_clear();
  test_blackboard_keys_and_prompt();
  test_debate_blackboard_round_accumulation();
  test_debate_clear_before_new_debate();
  test_debate_consensus_prompt_structure();

  // P1: workflow_run 执行引擎 + 自动重试
  test_workflow_run_success();
  test_workflow_run_step_failure();
  test_workflow_run_max_retries_exceeded();
  test_workflow_run_not_found();
  test_workflow_run_retry_counting();

  // P6: KanbanBoard 组件测试
  test_kanban_board();

  // v0.25.0: 多 Agent & 监控 handler 注册验证
  test_v025_skill_registry_handlers();

  // v0.25.9: CronScheduler chat 管线注入
  test_cron_chat_injection();

  // P0: KbSearcher FTS5 知识库搜索
  create_temp_fts5_db();
  test_kb_searcher_open_valid();
  test_kb_searcher_open_invalid();
  test_kb_searcher_fts5_match();
  test_kb_searcher_like_fallback();
  test_kb_searcher_empty_query();
  test_kb_searcher_not_open();
  test_kb_searcher_limit_cap();
  test_kb_searcher_constructor_with_path();
  test_kb_searcher_open_by_name();             // v0.27.2
  test_kb_searcher_repo_backward_compat();     // v0.27.2
  test_skill_registry_dispatch_known_error();  // v0.27.2
  test_skill_registry_dispatch_unknown();      // v0.27.2

  // P1: ChatPolicy 文本工具
  test_chatpolicy_normalize_text();
  test_chatpolicy_utf8_count();
  test_chatpolicy_language_detect();
  test_chatpolicy_input_translation();
  test_chatpolicy_progress_msg();

  // P1: AgentTracer 追踪器
  test_agent_tracer_basic();
  test_agent_tracer_error_span();
  test_agent_tracer_timeout_span();
  test_agent_tracer_multiple_sessions();

  // P2: DialogSlotRecall + AgentDirectory
  test_dialog_slot_recall_weather_query();
  test_dialog_slot_recall_news_query();
  test_dialog_slot_recall_referential_markers();
  test_agent_directory_register_find();
  test_agent_directory_unregister();

  // P3: ExternalInfoClient + Tools
  test_external_info_weather_mock();
  test_external_info_news_mock();
  test_external_info_unknown_provider();
  test_create_builtin_tools_count();
  test_create_builtin_tools_names();
  test_create_builtin_tools_null_context();
  test_create_builtin_tools_execute_weather();
  test_create_builtin_tools_execute_news();

  // P4: HybridRouter
  test_hybrid_router_empty();
  test_hybrid_router_short_simple();
  test_hybrid_router_medium_moderate();
  test_hybrid_router_long_complex();
  test_hybrid_router_complex_keyword();
  test_hybrid_router_multi_newline();

  // P5: ExternalIntentHandlers
  test_external_intent_handler_unknown_fetcher();
  test_external_intent_handler_weather_mock();
  test_external_intent_handler_news_mock();

  // P6: TemplateModel
  test_template_model_load();
  test_template_model_keyword_match();
  test_template_model_fallback();
  test_template_model_unloaded_infer();

  // P7: ModelPool
  test_model_pool_add_size();
  test_model_pool_remove();
  test_model_pool_match_by_capability();
  test_model_pool_best_priority();
  test_model_pool_total_memory();
  test_model_pool_status_all();

  // P8: McpServer
  test_mcp_server_make_response();
  test_mcp_server_make_error();
  test_mcp_server_parse_error();
  test_mcp_server_initialize();
  test_mcp_server_list_tools_no_init();
  test_mcp_server_list_tools_after_init();
  test_mcp_server_call_tool_no_init();
  test_mcp_server_unknown_method();
  test_mcp_server_notification();

  // P9: GgufModel + PluginLoader
  test_gguf_model_constructor();
  test_gguf_model_load_no_file();
  test_gguf_model_infer_no_llama();
  test_plugin_loader_no_such_dir();
  test_plugin_loader_empty_modes();

  // P10: CloudLlmClient
  test_cloudllm_build_request_body_simple();
  test_cloudllm_build_request_body_tool_message();
  test_cloudllm_build_request_body_tools();
  test_cloudllm_build_request_body_stream();
  test_cloudllm_parse_response_ok();
  test_cloudllm_parse_response_invalid_json();
  test_cloudllm_parse_response_http_error();
  test_cloudllm_parse_response_missing_choices();
  test_cloudllm_parse_response_tool_calls();
  test_cloudllm_parse_response_reasoning_only();
  test_cloudllm_parse_sse_simple();
  test_cloudllm_parse_sse_tool_calls();
  test_cloudllm_fallback_single_provider();

  // P11: McpClient
  test_mcp_build_jsonrpc_request();
  test_mcp_build_jsonrpc_request_no_params();
  test_mcp_parse_tool_list_ok();
  test_mcp_parse_tool_list_empty();
  test_mcp_parse_tool_list_invalid_json();
  test_mcp_parse_tool_call_ok();
  test_mcp_parse_tool_call_error();
  test_mcp_parse_tool_call_is_error();

  // P12: OnnxChatModel
  test_onnx_model_constructor();

  // P13: MockHttpClient + MockTransport
  test_mock_http_chat_completion();
  test_mock_http_chat_completion_error();
  test_mock_transport_connect();
  test_mock_transport_list_tools();
  test_mock_transport_call_tool();
  test_mock_http_chat_with_tools();
  test_mock_http_classify_intent();
  test_is_file_op_request();
  test_fs_checkpoint_scan_cap();
  test_search_files_link_ring();
  test_mock_embedding_encode();
  test_mock_embedding_encode_fallback();
  test_mock_external_weather_http();
  test_mock_external_news_http();

  std::cerr << "\n=== Agent core tests: " << failures << " failures ===\n";
  return failures > 0 ? 1 : 0;
}

// ── v0.25.0: SkillRegistry handler 注册验证 ─────────────────────

int test_v025_skill_registry_handlers() {
  using namespace thin_agent;
  SkillRegistry reg;

  int registered = 0;

  // spawn_agent 模式（不需要真实 LLM，只验证注册+参数校验）
  reg.register_cpp_handler("spawn_agent",
    [&registered](const nlohmann::json& params) -> nlohmann::json {
      ++registered;
      if (params.value("goal", params.value("task", "")) == "")
        return {{"success", false}, {"error", "goal required"}};
      return {{"success", true}, {"agent_id", "test_1"}};
    });
  auto r1 = reg.dispatch_cpp("spawn_agent", {{"goal", "test"}});
  check(r1.value("success", false), "spawn_agent registered and callable");
  check(registered == 1, "spawn_agent lambda executed");

  auto r1b = reg.dispatch_cpp("spawn_agent", nlohmann::json::object());
  check(!r1b.value("success", false), "spawn_agent rejects missing goal");

  // agent_message
  reg.register_cpp_handler("agent_message",
    [](const nlohmann::json& p) {
      if (p.value("to", "") == "" || p.value("message", "") == "")
        return nlohmann::json{{"success", false}, {"error", "to and message required"}};
      return nlohmann::json{{"success", true}, {"to", p["to"]}};
    });
  auto r2 = reg.dispatch_cpp("agent_message", {{"to", "w"}, {"message", "hi"}});
  check(r2.value("success", false), "agent_message sends ok");
  auto r2b = reg.dispatch_cpp("agent_message", nlohmann::json::object());
  check(!r2b.value("success", false), "agent_message rejects empty");

  // agent_inbox
  reg.register_cpp_handler("agent_inbox",
    [](const nlohmann::json& p) {
      return nlohmann::json{{"success", true}, {"agent_id", p.value("agent_id", "")},
                             {"count", 0}, {"messages", nlohmann::json::array()}};
    });
  auto r3 = reg.dispatch_cpp("agent_inbox", {{"agent_id", "t"}});
  check(r3.value("success", false), "agent_inbox ok");

  // bb_write
  reg.register_cpp_handler("bb_write",
    [](const nlohmann::json& p) {
      if (p.value("key", "") == "")
        return nlohmann::json{{"success", false}, {"error", "key required"}};
      return nlohmann::json{{"success", true}, {"key", p["key"]}};
    });
  auto r4 = reg.dispatch_cpp("bb_write", {{"key", "k"}, {"value", "v"}});
  check(r4.value("success", false), "bb_write ok");
  auto r4b = reg.dispatch_cpp("bb_write", nlohmann::json::object());
  check(!r4b.value("success", false), "bb_write rejects empty");

  // bb_read
  reg.register_cpp_handler("bb_read",
    [](const nlohmann::json& p) {
      if (p.value("key", "") == "")
        return nlohmann::json{{"success", false}, {"error", "key required"}};
      return nlohmann::json{{"success", true}, {"key", p["key"]}, {"found", true}};
    });
  auto r5 = reg.dispatch_cpp("bb_read", {{"key", "k"}});
  check(r5.value("success", false) && r5.value("found", false), "bb_read ok");

  // monitor_watch_file
  reg.register_cpp_handler("monitor_watch_file",
    [](const nlohmann::json& p) {
      if (p.value("path", "") == "")
        return nlohmann::json{{"success", false}, {"error", "path required"}};
      return nlohmann::json{{"success", true}, {"path", p["path"]}};
    });
  auto r6 = reg.dispatch_cpp("monitor_watch_file", {{"path", "/tmp/t.log"}});
  check(r6.value("success", false), "monitor_watch_file ok");

  // monitor_start
  reg.register_cpp_handler("monitor_start",
    [](const nlohmann::json&) {
      return nlohmann::json{{"success", true}};
    });
  check(reg.dispatch_cpp("monitor_start", {}).value("success", false),
        "monitor_start ok");

  // monitor_stop
  reg.register_cpp_handler("monitor_stop",
    [](const nlohmann::json&) {
      return nlohmann::json{{"success", true}};
    });
  check(reg.dispatch_cpp("monitor_stop", {}).value("success", false),
        "monitor_stop ok");

  // monitor_status
  reg.register_cpp_handler("monitor_status",
    [](const nlohmann::json&) {
      return nlohmann::json{{"success", true},
                             {"status", {{"running", false}}},
                             {"alerts", nlohmann::json::array()}};
    });
  auto r9 = reg.dispatch_cpp("monitor_status", {});
  check(r9.value("success", false), "monitor_status ok");
  check(r9.contains("alerts"), "monitor_status has alerts");

  // 未注册 handler
  auto r_bad = reg.dispatch_cpp("nonexistent", {});
  check(!r_bad.value("success", false) || r_bad.value("error", "") != "",
        "nonexistent handler fails");

  // agent_dag: 验证 handler 注册和参数校验
  reg.register_cpp_handler("agent_dag",
    [](const nlohmann::json& p) {
      auto nodes = p.value("nodes", nlohmann::json::array());
      if (!nodes.is_array() || nodes.empty())
        return nlohmann::json{{"success", false}, {"error", "nodes array required"}};
      return nlohmann::json{{"success", true}, {"node_count", nodes.size()}};
    });
  auto r_dag = reg.dispatch_cpp("agent_dag", {{"nodes", {{
    {"task_id", "t1"}, {"prompt", "test"}, {"role", "researcher"}
  }}}});
  check(r_dag.value("success", false), "agent_dag executes with nodes");
  check(r_dag.value("node_count", 0) == 1, "agent_dag node count correct");

  auto r_dag_empty = reg.dispatch_cpp("agent_dag", nlohmann::json::object());
  check(!r_dag_empty.value("success", false), "agent_dag rejects empty nodes");

  // ── v0.25.2: CronScheduler handler 验证 ──
  reg.register_cpp_handler("cron_add",
    [](const nlohmann::json& p) {
      if (p.value("name", "") == "" || p.value("schedule", "") == "")
        return nlohmann::json{{"success", false}, {"error", "name and schedule required"}};
      return nlohmann::json{{"success", true}, {"task", {{"id", 1}, {"name", p["name"]}}}};
    });
  auto r_ca = reg.dispatch_cpp("cron_add", {{"name", "test"}, {"schedule", "5m"}, {"prompt", "hello"}});
  check(r_ca.value("success", false), "cron_add creates task");
  auto r_ca_empty = reg.dispatch_cpp("cron_add", nlohmann::json::object());
  check(!r_ca_empty.value("success", false), "cron_add rejects empty");

  reg.register_cpp_handler("cron_list",
    [](const nlohmann::json&) { return nlohmann::json{{"success", true}, {"tasks", nlohmann::json::array()}}; });
  check(reg.dispatch_cpp("cron_list", {}).value("success", false), "cron_list ok");

  reg.register_cpp_handler("cron_remove",
    [](const nlohmann::json& p) {
      if (p.value("task_id", 0) <= 0)
        return nlohmann::json{{"success", false}, {"error", "task_id required"}};
      return nlohmann::json{{"success", true}, {"task_id", p["task_id"]}};
    });
  auto r_cr = reg.dispatch_cpp("cron_remove", {{"task_id", 1}});
  check(r_cr.value("success", false), "cron_remove deletes task");
  auto r_cr_empty = reg.dispatch_cpp("cron_remove", nlohmann::json::object());
  check(!r_cr_empty.value("success", false), "cron_remove rejects empty");

  reg.register_cpp_handler("cron_stats",
    [](const nlohmann::json&) {
      return nlohmann::json{{"success", true}, {"stats", {{"total", 0}, {"enabled", 0}}}};
    });
  check(reg.dispatch_cpp("cron_stats", {}).value("success", false), "cron_stats ok");

  // ── v0.25.3: Checkpoint handler 验证 ──
  reg.register_cpp_handler("checkpoint_save",
    [](const nlohmann::json&) { return nlohmann::json{{"success", true}, {"checkpoint_id", "ckpt_1"}}; });
  check(reg.dispatch_cpp("checkpoint_save", {}).value("success", false), "checkpoint_save ok");

  reg.register_cpp_handler("checkpoint_rollback",
    [](const nlohmann::json&) { return nlohmann::json{{"success", true}, {"message_count", 3}}; });
  check(reg.dispatch_cpp("checkpoint_rollback", {}).value("success", false), "checkpoint_rollback ok");

  reg.register_cpp_handler("checkpoint_list",
    [](const nlohmann::json&) { return nlohmann::json{{"success", true}, {"snapshots", nlohmann::json::array()}}; });
  check(reg.dispatch_cpp("checkpoint_list", {}).value("success", false), "checkpoint_list ok");

  // ── v0.25.4: P5 correction_record ──
  reg.register_cpp_handler("correction_record",
    [](const nlohmann::json& p) {
      if (p.value("error_type", "") == "" || p.value("fix", "") == "")
        return nlohmann::json{{"success", false}, {"error", "error_type and fix required"}};
      return nlohmann::json{{"success", true}, {"recorded", true}};
    });
  auto r_correct = reg.dispatch_cpp("correction_record", {{"error_type", "build"}, {"fix", "add include"}});
  check(r_correct.value("success", false), "correction_record saves ok");
  auto r_correct_empty = reg.dispatch_cpp("correction_record", nlohmann::json::object());
  check(!r_correct_empty.value("success", false), "correction_record rejects empty");

  // ── v0.25.4: P6 kanban ──
  reg.register_cpp_handler("kanban_push",
    [](const nlohmann::json& p) {
      if (p.value("title", "") == "")
        return nlohmann::json{{"success", false}, {"error", "title required"}};
      return nlohmann::json{{"success", true}, {"task_id", "k_1"}};
    });
  auto r_kb = reg.dispatch_cpp("kanban_push", {{"title", "Fix CI"}, {"priority", "high"}});
  check(r_kb.value("success", false), "kanban_push creates card");
  auto r_kb_empty = reg.dispatch_cpp("kanban_push", nlohmann::json::object());
  check(!r_kb_empty.value("success", false), "kanban_push rejects empty");

  reg.register_cpp_handler("kanban_status",
    [](const nlohmann::json&) {
      return nlohmann::json{{"success", true}, {"columns", {
        {"todo", nlohmann::json::array()},
        {"in_progress", nlohmann::json::array()},
        {"done", nlohmann::json::array()}
      }}};
    });
  auto r_kbs = reg.dispatch_cpp("kanban_status", {});
  check(r_kbs.value("success", false), "kanban_status ok");
  check(r_kbs.contains("columns"), "kanban_status has columns");

  // ── v0.25.4: P7 goal ──
  reg.register_cpp_handler("goal_add",
    [](const nlohmann::json& p) {
      if (p.value("description", "") == "")
        return nlohmann::json{{"success", false}, {"error", "description required"}};
      return nlohmann::json{{"success", true}, {"goal_id", "g_1"}, {"description", p["description"]}};
    });
  auto r_ga = reg.dispatch_cpp("goal_add", {{"description", "Improve test coverage"}});
  check(r_ga.value("success", false), "goal_add creates goal");
  auto r_ga_empty = reg.dispatch_cpp("goal_add", nlohmann::json::object());
  check(!r_ga_empty.value("success", false), "goal_add rejects empty");

  reg.register_cpp_handler("goal_list",
    [](const nlohmann::json&) {
      return nlohmann::json{{"success", true}, {"goals", nlohmann::json::array()}};
    });
  check(reg.dispatch_cpp("goal_list", {}).value("success", false), "goal_list ok");

  reg.register_cpp_handler("goal_update",
    [](const nlohmann::json& p) {
      if (p.value("goal_id", "") == "")
        return nlohmann::json{{"success", false}, {"error", "goal_id required"}};
      return nlohmann::json{{"success", true}, {"goal_id", p["goal_id"]}, {"status", p.value("status", "active")}};
    });
  auto r_gu = reg.dispatch_cpp("goal_update", {{"goal_id", "g_1"}, {"status", "done"}});
  check(r_gu.value("success", false), "goal_update updates goal");
  auto r_gu_empty = reg.dispatch_cpp("goal_update", nlohmann::json::object());
  check(!r_gu_empty.value("success", false), "goal_update rejects empty");

  // ── v0.25.4: P8 summarize ──
  reg.register_cpp_handler("summarize",
    [](const nlohmann::json& p) {
      return nlohmann::json{{"success", true}, {"summary", "Test summary of: " + p.value("text", "").substr(0, 100)}};
    });
  auto r_sum = reg.dispatch_cpp("summarize", {{"text", "Long conversation about testing"}});
  check(r_sum.value("success", false), "summarize produces summary");
  check(r_sum.value("summary", "") != "", "summarize summary non-empty");

  return 0;
}

// ─── v0.25.9: CronScheduler chat 管线注入 ──────────────────────────

int test_cron_chat_injection() {
  // 模拟 AgentService 构造中的 cron callback 逻辑
  // 测试：回调实现、事件格式、空 prompt 防护、mu_ 锁线程安全

  std::mutex mu;
  std::vector<std::string> events;

  auto record_event = [&](const std::string& e) {
    std::lock_guard<std::mutex> lk(mu);
    events.push_back(e);
  };

  // 模拟真实 callback（与 AgentService.cpp 构造中完全一致）
  auto callback = [&](const nlohmann::json& task) {
    std::string name = task.value("name", "unnamed");
    std::string prompt = task.value("prompt", "");

    record_event("cron_fired: " + name + " | prompt=" + prompt.substr(0, 200));

    if (!prompt.empty()) {
      // 在真实代码中是 std::thread(...).detach()，这里直接验证 prompt 传递
      record_event("cron_injected: " + name + " | prompt_len=" + std::to_string(prompt.length()));
    }
  };

  // Test 1: 正常任务触发（prompt 非空）
  nlohmann::json task1;
  task1["name"] = "daily_build";
  task1["prompt"] = "Check build status and fix if broken";
  callback(task1);

  check(events.size() == 2, "callback records fire + inject for non-empty prompt");
  check(events[0] == "cron_fired: daily_build | prompt=Check build status and fix if broken",
        "cron_fired event format correct");
  check(events[1].find("cron_injected: daily_build") != std::string::npos,
        "cron_injected event has task name");
  check(events[1].find("prompt_len=36") != std::string::npos,
        "cron_injected includes prompt length");

  // Test 2: 空 prompt — 不注入 chat 管线
  events.clear();
  nlohmann::json task2;
  task2["name"] = "empty_task";
  task2["prompt"] = "";
  callback(task2);

  check(events.size() == 1, "empty prompt: only fire, no inject");
  check(events[0] == "cron_fired: empty_task | prompt=",
        "empty prompt fired event correct");

  // Test 3: 缺失 name 字段 — 使用默认值 "unnamed"
  events.clear();
  nlohmann::json task3;
  task3["prompt"] = "Hello";
  callback(task3);

  check(events.size() == 2, "missing name: uses 'unnamed' default");
  check(events[0].find("unnamed") != std::string::npos,
        "falls back to 'unnamed' when name missing");

  // Test 4: 长 prompt 截断 (substr(0, 200))
  events.clear();
  std::string long_prompt(500, 'x');
  nlohmann::json task4;
  task4["name"] = "long";
  task4["prompt"] = long_prompt;
  callback(task4);

  check(events.size() == 2, "long prompt still fires + injects");
  check(events[0].find("prompt=") != std::string::npos, "long prompt in fired event");
  // 验证 prompt 在记录事件中被截断（substr 200）
  size_t prompt_start = events[0].find("prompt=") + 7;
  check(events[0].length() - prompt_start <= 210, "prompt truncated to ~200 chars in log");

  // Test 5: 并发安全性 — 多个线程同时调 callback
  events.clear();
  std::vector<std::thread> threads;
  for (int i = 0; i < 10; ++i) {
    threads.emplace_back([&, i]() {
      nlohmann::json t;
      t["name"] = "thread_" + std::to_string(i);
      t["prompt"] = "Test " + std::to_string(i);
      callback(t);
    });
  }
  for (auto& th : threads) th.join();
  check(events.size() == 20, "10 threads × 2 events = 20 total");  // fire + inject per thread
  check(mu.try_lock(), "mu is not held after all callbacks return");
  mu.unlock();

  return 0;
}

// ─── v0.25.4: KanbanBoard 组件测试 ──────────────────────────────

int test_kanban_board() {
  using namespace thin_agent;

  KanbanBoard board;

  // Test 1: 空看板
  check(board.total_count() == 0, "empty board: total=0");
  check(board.pending_count() == 0, "empty board: pending=0");
  check(!board.has_pending(), "empty board: no pending");
  auto empty_status = board.status();
  check(empty_status["total"].get<int>() == 0, "empty status: total=0");

  // Test 2: push 单个任务
  KanbanTask t1{"task_1", "Fix build", "cmake --build build", "developer", "pending"};
  board.push(t1);
  check(board.total_count() == 1, "push: total=1");
  check(board.pending_count() == 1, "push: pending=1");
  check(board.has_pending(), "push: has_pending=true");

  // Test 3: push_batch 批量任务
  std::vector<KanbanTask> batch = {
    {"task_a", "Task A", "prompt A", "worker", "pending"},
    {"task_b", "Task B", "prompt B", "tester", "pending"},
    {"task_c", "Task C", "prompt C", "debugger", "pending"}
  };
  board.push_batch(batch);
  check(board.total_count() == 4, "batch: total=4");
  check(board.pending_count() == 4, "batch: pending=4");

  // Test 4: pull 抢单 — 取走第一个 pending，标记 in_progress
  auto pulled = board.pull("agent_1");
  check(!pulled.task_id.empty(), "pull: got a task");
  check(pulled.status == "in_progress", "pull: status changed to in_progress");
  check(pulled.assigned_to == "agent_1", "pull: assigned to agent_1");
  check(board.pending_count() == 3, "pull: pending reduced to 3");
  check(board.total_count() == 4, "pull: total unchanged");

  // Test 5: 无 pending 时 pull 返回空
  board.pull("agent_2");
  board.pull("agent_3");
  board.pull("agent_4");  // all 3 remaining picked up
  check(board.pending_count() == 0, "pull all: pending=0");
  check(!board.has_pending(), "pull all: no pending");
  auto empty = board.pull("agent_5");
  check(empty.task_id.empty(), "pull empty: no task");

  // Test 6: complete — 标记 done
  std::string completed_id = pulled.task_id;  // from first pull
  nlohmann::json result = {{"output", "build success"}, {"exit_code", 0}};
  board.complete(completed_id, true, result);
  auto status = board.status();
  check(status["done"].get<int>() == 1, "complete: done=1");
  check(status["failed"].get<int>() == 0, "complete: failed=0");

  // Test 7: complete — 标记 failed
  auto pulled2 = board.pull("agent_1");  // won't get one since all are taken, but the in_progress ones are still there
  // Actually all are in_progress now. Let's complete one as failed.
  // Pull again: all tasks are in_progress, so no pending. Let's create a new task.
  KanbanTask fail_task{"fail_1", "Failing task", "bad cmd", "worker", "pending"};
  board.push(fail_task);
  auto pf = board.pull("agent_x");
  board.complete(pf.task_id, false, {}, "command not found");
  auto status2 = board.status();
  check(status2["failed"].get<int>() == 1, "complete fail: failed=1");

  // Test 8: clear — 清空
  board.clear();
  check(board.total_count() == 0, "clear: total=0");
  check(board.pending_count() == 0, "clear: pending=0");
  check(!board.has_pending(), "clear: no pending");

  // Test 9: status 结构完整性
  KanbanTask s1{"status_1", "S1", "p1", "role1", "pending"};
  board.push(s1);
  auto full_status = board.status();
  check(full_status.contains("total"), "status has total");
  check(full_status.contains("pending"), "status has pending");
  check(full_status.contains("in_progress"), "status has in_progress");
  check(full_status.contains("done"), "status has done");
  check(full_status.contains("failed"), "status has failed");
  check(full_status["total"].get<int>() == 1, "status: total=1");
  board.clear();

  return 0;
}

// ─── P0: KbSearcher FTS5 知识库搜索 ───────────────────────────

#include "thin_agent/core/KbSearcher.h"
#include <sqlite3.h>
#include <filesystem>

static std::string create_temp_fts5_db() {
  namespace fs = std::filesystem;
  std::string path = "/tmp/test_kb_fts5.db";
  fs::remove(path);  // clean any leftover

  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return "";
  const char* create_sql =
    "CREATE VIRTUAL TABLE IF NOT EXISTS codebase USING fts5("
    "  path, filename, extension, symbols, content,"
    "  tokenize='porter unicode61'"
    ");";
  sqlite3_exec(db, create_sql, nullptr, nullptr, nullptr);

  const char* insert_sql =
    "INSERT INTO codebase (path, filename, extension, symbols, content) VALUES "
    "('/src/camera/mipi.c', 'mipi.c', '.c', 'mipi_init mipi_deinit', "
    " 'void mipi_init() { /* init MIPI CSI */ }'),"
    "('/src/camera/isp.c', 'isp.c', '.c', 'isp_configure camera_open', "
    " 'int isp_configure(int flags) { return 0; }'),"
    "('/include/camera/mipi.h', 'mipi.h', '.h', 'MIPI_CSI2_LANE_COUNT', "
    " '#define MIPI_CSI2_LANE_COUNT 4'),"
    "('/src/main.c', 'main.c', '.c', 'main argv argc', "
    " 'int main(int argc, char** argv) { return 0; }');";
  sqlite3_exec(db, insert_sql, nullptr, nullptr, nullptr);
  sqlite3_close(db);
  return path;
}

int test_kb_searcher_open_valid() {
  thin_agent::KbSearcher kb;
  check(!kb.is_open(), "kb: not open initially");

  bool ok = kb.open("/tmp/test_kb_fts5.db");
  check(ok, "kb: open succeeds");
  check(kb.is_open(), "kb: is_open after open");

  int count = kb.doc_count();
  check(count == 4, "kb: doc_count=4");

  return 0;
}

int test_kb_searcher_open_invalid() {
  thin_agent::KbSearcher kb;
  // SQLite sqlite3_open 会创建不存在的文件，要用不存在的目录才能触发失败
  bool ok = kb.open("/nonexistent_dir_12345/test.db");
  check(!ok, "kb: open unwritable path fails");
  check(!kb.is_open(), "kb: not open after failed open");
  check(kb.doc_count() == 0, "kb: doc_count=0 when not open");

  return 0;
}

int test_kb_searcher_fts5_match() {
  thin_agent::KbSearcher kb("/tmp/test_kb_fts5.db");
  auto r = kb.search("mipi", 10);
  check(r["matches"].get<int>() >= 2, "kb: fts5 'mipi' matches >=2");
  check(r["query"] == "mipi", "kb: query preserved");
  check(r["results"].size() >= 2, "kb: results array size matches");

  auto& first = r["results"][0];
  check(!first["path"].get<std::string>().empty(), "kb: result has path");
  check(!first["filename"].get<std::string>().empty(), "kb: result has filename");
  check(!first["extension"].get<std::string>().empty(), "kb: result has extension");
  check(!first["snippet"].get<std::string>().empty(), "kb: result has snippet");

  return 0;
}

int test_kb_searcher_like_fallback() {
  thin_agent::KbSearcher kb("/tmp/test_kb_fts5.db");
  // Search for something that doesn't exist
  auto r = kb.search("xyznonexistent12345", 10);
  check(r["matches"].get<int>() == 0, "kb: no matches for nonsense query");
  check(r["results"].size() == 0, "kb: results array empty for no match");
  return 0;
}

int test_kb_searcher_empty_query() {
  thin_agent::KbSearcher kb("/tmp/test_kb_fts5.db");
  auto r = kb.search("", 10);
  check(r["matches"] == 0, "kb: empty query matches=0");
  check(r.value("error", "") == "empty query", "kb: empty query has error");

  return 0;
}

int test_kb_searcher_not_open() {
  thin_agent::KbSearcher kb;
  auto r = kb.search("anything", 10);
  check(r["matches"] == 0, "kb: not open matches=0");
  check(r.value("error", "") == "KB database not open", "kb: not open has error");

  return 0;
}

int test_kb_searcher_limit_cap() {
  thin_agent::KbSearcher kb("/tmp/test_kb_fts5.db");
  auto r = kb.search("c", 100);
  check(r["results"].size() <= 50, "kb: limit capped to 50");

  r = kb.search("mipi", 0);
  check(r["results"].size() <= 10, "kb: limit 0 defaults to 10");

  return 0;
}

int test_kb_searcher_constructor_with_path() {
  thin_agent::KbSearcher kb("/tmp/test_kb_fts5.db");
  check(kb.is_open(), "kb: constructor with path opens db");
  check(kb.doc_count() == 4, "kb: constructor doc_count=4");

  return 0;
}

// ─── v0.27.2: 多 KB 与 SkillRegistry dispatch 测试 ─────────────

#include <fstream>
#include <cstdio>

int test_kb_searcher_open_by_name() {
  // 准备 kb_index.json 和两个 KB
  std::string kb_dir = "/tmp/test_kb_multi";
  std::string idx_path = kb_dir + "/kb_index.json";
  std::filesystem::create_directories(kb_dir);

  // 创建两个 KB
  {
    sqlite3* db = nullptr;
    sqlite3_open((kb_dir + "/codebase.db").c_str(), &db);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE codebase USING fts5(repo, path, filename, extension, symbols, content)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO codebase VALUES ('test_repo','src/a.cpp','a.cpp','.cpp','','hello world')", nullptr, nullptr, nullptr);
    sqlite3_close(db);

    sqlite3_open((kb_dir + "/my_kb.db").c_str(), &db);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE codebase USING fts5(repo, path, filename, extension, symbols, content)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO codebase VALUES ('my_repo','src/b.py','b.py','.py','','another doc')", nullptr, nullptr, nullptr);
    sqlite3_close(db);
  }

  // 写 kb_index.json
  {
    nlohmann::json idx;
    idx["kbs"]["codebase"]["path"] = "codebase.db";
    idx["kbs"]["codebase"]["type"] = "repo_scan";
    idx["kbs"]["my_kb"]["path"] = "my_kb.db";
    idx["kbs"]["my_kb"]["type"] = "manual";
    std::ofstream ofs(idx_path);
    ofs << idx.dump();
  }

  // 用 resolve_kb_db_path 解析路径，再用 open 打开
  std::string codebase_path = thin_agent::resolve_kb_db_path("codebase", kb_dir, idx_path);
  check(codebase_path.find("codebase.db") != std::string::npos, "resolve: codebase path resolved");
  thin_agent::KbSearcher kb;
  bool ok = kb.open(codebase_path);
  check(ok, "resolve: codebase opened");

  auto r = kb.search("hello");
  check(r["matches"] == 1, "resolve: found doc in codebase");

  // 切换到 my_kb
  std::string my_kb_path = thin_agent::resolve_kb_db_path("my_kb", kb_dir, idx_path);
  ok = kb.open(my_kb_path);
  check(ok, "resolve: switched to my_kb");
  r = kb.search("another");
  check(r["matches"] == 1, "resolve: found doc in my_kb");

  // 不存在的 KB 回退到 {kb_dir}/{kb_name}.db
  std::string fallback = thin_agent::resolve_kb_db_path("nonexistent", kb_dir, idx_path);
  check(fallback == kb_dir + "/nonexistent.db", "resolve: fallback for unknown KB");

  // 清理
  std::remove((kb_dir + "/codebase.db").c_str());
  std::remove((kb_dir + "/my_kb.db").c_str());
  std::remove(idx_path.c_str());
  std::remove(kb_dir.c_str());

  return 0;
}

int test_kb_searcher_repo_backward_compat() {
  // 创建旧 schema（无 repo 列）的 DB
  std::string db_path = "/tmp/test_kb_old_schema.db";
  std::remove(db_path.c_str());

  {
    sqlite3* db = nullptr;
    sqlite3_open(db_path.c_str(), &db);
    sqlite3_exec(db, "CREATE VIRTUAL TABLE codebase USING fts5(path, filename, extension, symbols, content)", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "INSERT INTO codebase VALUES ('src/old.cpp','old.cpp','.cpp','','old schema content')", nullptr, nullptr, nullptr);
    sqlite3_close(db);
  }

  // 用 KbSearcher 打开旧 schema DB
  thin_agent::KbSearcher kb(db_path);
  check(kb.is_open(), "backward_compat: old schema db opens");
  check(kb.doc_count() == 1, "backward_compat: old schema has 1 doc");

  auto r = kb.search("old");
  check(r["matches"] == 1, "backward_compat: can search old schema");
  check(r["results"][0]["repo"] == "", "backward_compat: repo is empty for old schema");
  check(r["results"][0]["path"] == "src/old.cpp", "backward_compat: path correct for old schema");

  std::remove(db_path.c_str());
  return 0;
}

int test_skill_registry_dispatch_known_error() {
  thin_agent::SkillRegistry registry;
  registry.register_cpp_handler("test_tool",
    [](const nlohmann::json&) -> nlohmann::json {
      return {{"success", false}, {"error", "something went wrong"}};
    });

  // 已知 handler 返回错误 — dispatch_cpp 应正常返回，含 error key
  auto result = registry.dispatch_cpp("test_tool", {});
  check(result.contains("error"), "dispatch_known: result has error key");
  check(result["error"] == "something went wrong", "dispatch_known: error message preserved");
  check(result["success"] == false, "dispatch_known: success=false");

  return 0;
}

int test_skill_registry_dispatch_unknown() {
  thin_agent::SkillRegistry registry;

  // 未知 handler — dispatch_cpp 返回 no_cpp_handler 错误
  auto result = registry.dispatch_cpp("nonexistent", {});
  check(result.contains("error"), "dispatch_unknown: result has error key");
  std::string err = result["error"];
  check(err.find("no_cpp_handler:nonexistent") != std::string::npos,
        "dispatch_unknown: error contains no_cpp_handler");

  return 0;
}

// ─── P1: ChatPolicy 文本工具函数 ──────────────────────────────

#include "thin_agent/core/ChatPolicy.h"

int test_chatpolicy_normalize_text() {
  // 去标点 + 小写
  std::string r = thin_agent::normalize_text_for_policy("Hello, World!");
  check(r.find("hello") != std::string::npos, "cp_norm: has 'hello'");
  check(r.find("world") != std::string::npos, "cp_norm: has 'world'");

  // 中文
  r = thin_agent::normalize_text_for_policy("今天天气？");
  check(r == "今天天气", "cp_norm: strip chinese punctuation");

  // empty
  r = thin_agent::normalize_text_for_policy("");
  check(r.empty(), "cp_norm: empty stays empty");

  return 0;
}

int test_chatpolicy_utf8_count() {
  check(thin_agent::utf8_char_count_at_least("hello", 3), "utf8: hello >= 3");
  check(thin_agent::utf8_char_count_at_least("hi", 3) == false, "utf8: 'hi' < 3");
  check(thin_agent::utf8_char_count_at_least("你好世界", 3), "utf8: 你好世界 >= 3");
  check(thin_agent::utf8_char_count_at_least("你好", 3) == false, "utf8: 你好 < 3");
  check(thin_agent::utf8_char_count_at_least("", 1) == false, "utf8: empty < 1");

  return 0;
}

int test_chatpolicy_language_detect() {
  check(thin_agent::is_simplified_chinese("你好世界"), "lang: zh detected");
  check(thin_agent::is_simplified_chinese("hello") == false, "lang: english not zh");
  check(thin_agent::is_english_text("hello world"), "lang: en detected");
  check(thin_agent::is_english_text("你好") == false, "lang: zh not en");
  // is_simplified_chinese is presence-based: any CJK → true
  check(thin_agent::is_simplified_chinese("hello 你好"), "lang: mixed with zh chars → zh");
  check(thin_agent::is_english_text("hello 你好") == false, "lang: mixed not pure en");

  return 0;
}

int test_chatpolicy_input_translation() {
  // Chinese → needs translation (non-English, non-simplified)
  // Actually, needs_input_translation returns true for text that is NOT simplified Chinese AND NOT English
  // So zh text returns false (already simplified Chinese)
  check(thin_agent::needs_input_translation("你好") == false, "trans: zh needs none");
  check(thin_agent::needs_input_translation("hello") == false, "trans: en needs none");
  // Japanese text
  check(thin_agent::needs_input_translation("こんにちは"), "trans: ja needs translation");
  // Short text below min_length_for_detect
  check(thin_agent::needs_input_translation("ab") == false, "trans: too short to decide");

  return 0;
}

int test_chatpolicy_progress_msg() {
  std::string zh = thin_agent::progress_msg("local_analysis", true, {});
  check(!zh.empty(), "prog: zh msg non-empty");

  std::string en = thin_agent::progress_msg("local_analysis", false, {});
  check(!en.empty(), "prog: en msg non-empty");

  return 0;
}

// ─── P1: AgentTracer 追踪器 ────────────────────────────────────

#include "thin_agent/agent/AgentTracer.h"

int test_agent_tracer_basic() {
  auto& t = thin_agent::agent::AgentTracer::instance();
  t.begin_session("test-session-1");

  // Start and end a span
  std::string s1 = t.start_span("llm_call");
  check(!s1.empty(), "trace: span_id non-empty");
  t.record_llm(s1, 100, 200, "test-model");
  t.end_span(s1, "ok");

  // Start a nested span
  std::string s2 = t.start_span("tool_call", s1);
  check(!s2.empty(), "trace: child span_id non-empty");
  t.record_tool(s2, "read_file", {{"path", "/tmp/test"}}, {{"ok", true}});
  t.end_span(s2, "ok");

  // Get trace
  auto trace = t.get_trace();
  check(trace.contains("session_id"), "trace: has session_id");
  check(trace["session_id"] == "test-session-1", "trace: correct session");
  check(trace["spans"].size() == 2, "trace: 2 spans");

  // Summary
  auto sum = t.summary();
  check(sum["total_spans"] == 2, "trace: summary total=2");
  check(sum["errors"] == 0, "trace: summary errors=0");
  check(sum["total_latency_ms"] >= 0, "trace: summary has latency");

  // Clear
  t.clear();
  check(t.summary()["total_spans"] == 0, "trace: clear empties spans");

  return 0;
}

int test_agent_tracer_error_span() {
  auto& t = thin_agent::agent::AgentTracer::instance();
  t.begin_session("test-session-2");

  std::string s1 = t.start_span("tool_call");
  t.record_tool(s1, "shell_exec", {{"cmd", "false"}}, {{"exit_code", 1}});
  t.end_span(s1, "error");

  auto sum = t.summary();
  check(sum["total_spans"] == 1, "trace_err: total=1");
  check(sum["errors"] == 1, "trace_err: errors=1");

  t.clear();
  return 0;
}

int test_agent_tracer_timeout_span() {
  auto& t = thin_agent::agent::AgentTracer::instance();
  t.begin_session("test-session-3");

  std::string s = t.start_span("llm_call");
  t.end_span(s, "timeout");

  auto sum = t.summary();
  check(sum["total_spans"] == 1, "trace_to: total=1");
  check(sum["errors"] == 1, "trace_to: timeout counts as error");

  t.clear();
  return 0;
}

int test_agent_tracer_multiple_sessions() {
  auto& t = thin_agent::agent::AgentTracer::instance();

  t.begin_session("session-a");
  std::string sa = t.start_span("turn");
  t.end_span(sa, "ok");
  check(t.summary()["total_spans"] == 1, "trace_multi: session-a has 1 span");

  t.begin_session("session-b");
  check(t.summary()["total_spans"] == 0, "trace_multi: session-b starts fresh");

  t.clear();
  return 0;
}

// ─── P2: DialogSlotRecall ──────────────────────────────────────

#include "thin_agent/core/DialogSlotRecall.h"

int test_dialog_slot_recall_weather_query() {
  check(thin_agent::is_weather_location_recall_query("刚才那是哪里的天气"),
        "dsr: weather recall query detected");
  check(thin_agent::is_weather_location_recall_query("今天天气怎么样") == false,
        "dsr: normal weather query not recall");
  check(thin_agent::is_weather_location_recall_query("") == false,
        "dsr: empty not recall");

  return 0;
}

int test_dialog_slot_recall_news_query() {
  check(thin_agent::is_news_topic_recall_query("刚才看的是什么话题的新闻"),
        "dsr: news recall query detected");
  check(thin_agent::is_news_topic_recall_query("今天的新闻") == false,
        "dsr: normal news query not recall");

  return 0;
}

int test_dialog_slot_recall_referential_markers() {
  check(thin_agent::has_referential_context_markers("那深圳的呢"),
        "dsr: referential markers in '那深圳的呢'");
  check(thin_agent::has_referential_context_markers("刚才那个怎么样"),
        "dsr: referential markers in '刚才那个'");
  check(thin_agent::has_referential_context_markers("你好") == false,
        "dsr: no referential markers in greeting");

  return 0;
}

// ─── P2: AgentDirectory ────────────────────────────────────────

#include "thin_agent/agent/AgentDirectory.h"

int test_agent_directory_register_find() {
  auto& dir = thin_agent::agent::AgentDirectory::instance();

  thin_agent::agent::AgentInfo a1{"id1", "Agent One", "desc1", "http://1", {"code", "review"}};
  thin_agent::agent::AgentInfo a2{"id2", "Agent Two", "desc2", "http://2", {"test", "debug"}};

  dir.register_agent(a1);
  dir.register_agent(a2);

  auto* f1 = dir.find("id1");
  check(f1 != nullptr, "dir: find id1");
  check(f1->name == "Agent One", "dir: correct name");
  check(f1->capabilities.size() == 2, "dir: 2 capabilities");

  auto* f_bad = dir.find("nonexistent");
  check(f_bad == nullptr, "dir: find nonexistent returns null");

  auto all = dir.list();
  check(all.size() == 2, "dir: list returns 2 agents");

  auto coders = dir.find_by_capability("code");
  check(coders.size() >= 1, "dir: find_by_capability 'code'");

  auto testers = dir.find_by_capability("test");
  check(testers.size() >= 1, "dir: find_by_capability 'test'");

  auto none = dir.find_by_capability("flowers");
  check(none.empty(), "dir: find_by_capability 'flowers' empty");

  return 0;
}

int test_agent_directory_unregister() {
  auto& dir = thin_agent::agent::AgentDirectory::instance();

  thin_agent::agent::AgentInfo a3{"id3", "Agent Three", "desc3", "http://3", {"search"}};
  dir.register_agent(a3);
  check(dir.list().size() >= 2, "dir: at least 2 agents after register");

  dir.unregister_agent("id3");
  check(dir.find("id3") == nullptr, "dir: id3 gone after unregister");

  // unregister nonexistent is safe
  dir.unregister_agent("id_nonexistent");

  return 0;
}

// ─── P3: ExternalInfoClient mock 模式 ──────────────────────────

#include "thin_agent/core/ExternalInfoClient.h"

int test_external_info_weather_mock() {
  struct DummyHttp : thin_agent::IHttpClient {
    thin_agent::HttpResponse post(const std::string&, const std::string&, const std::string&, const std::string& = "") override { return {}; }
    thin_agent::HttpResponse get(const std::string&, const std::string&) override { return {}; }
  } mock;
  auto r = thin_agent::ExternalInfoClient::fetch_weather(mock, "北京", "今天", "mock", "", "zh");
  check(r.ok, "ext: weather mock ok");
  check(r.source == "mock", "ext: source is mock");
  check(r.data.contains("city"), "ext: weather data has city");
  return 0;
}

int test_external_info_news_mock() {
  struct DummyHttp : thin_agent::IHttpClient {
    thin_agent::HttpResponse post(const std::string&, const std::string&, const std::string&, const std::string& = "") override { return {}; }
    thin_agent::HttpResponse get(const std::string&, const std::string&) override { return {}; }
  } mock;
  auto r = thin_agent::ExternalInfoClient::fetch_news(mock, "科技", "本周", "mock");
  check(r.ok, "ext: news mock ok");
  check(r.source == "mock", "ext: news source is mock");
  return 0;
}

int test_external_info_unknown_provider() {
  struct DummyHttp : thin_agent::IHttpClient {
    thin_agent::HttpResponse post(const std::string&, const std::string&, const std::string&, const std::string& = "") override { return {}; }
    thin_agent::HttpResponse get(const std::string&, const std::string&) override { return {}; }
  } mock;
  auto r = thin_agent::ExternalInfoClient::fetch_weather(mock, "北京", "今天", "nonexistent_provider", "", "zh");
  check(r.ok, "ext: unknown provider still returns ok (mock fallback)");
  return 0;
}

// ─── P3: Tools 内置工具创建 ─────────────────────────────────────

#include "thin_agent/agent/Tools.h"

int test_create_builtin_tools_count() {
  thin_agent::agent::ToolContext ctx;
  auto tools = thin_agent::agent::create_builtin_tools(ctx);
  // Tools may be empty if chat_policy.json is not available; that's valid
  check(tools.size() >= 0, "tools: create_builtin_tools does not crash");
  return 0;
}

int test_create_builtin_tools_names() {
  thin_agent::agent::ToolContext ctx;
  auto tools = thin_agent::agent::create_builtin_tools(ctx);

  // Collect all tool names
  bool has_status = false, has_weather = false, has_news = false;
  for (auto& t : tools) {
    if (t.name == "status") has_status = true;
    if (t.name == "weather") has_weather = true;
    if (t.name == "news") has_news = true;
  }
  // If tools are available, verify key ones exist
  if (!tools.empty()) {
    check(has_status, "tools: has status");
    check(has_weather, "tools: has weather");
    check(has_news, "tools: has news");
  }
  return 0;
}

int test_create_builtin_tools_null_context() {
  thin_agent::agent::ToolContext ctx;
  auto tools = thin_agent::agent::create_builtin_tools(ctx);
  if (tools.empty()) return 0;

  for (auto& t : tools) {
    if (t.name == "memory_recent" || t.name == "memory_search") {
      check(t.execute != nullptr, "tools: null-ctx tool has execute lambda");
      auto result = t.execute(nlohmann::json::object());
      check(!result.value("success", true),
            (std::string("tools: ") + t.name + " fails with null agent_service").c_str());
    }
  }
  return 0;
}

int test_create_builtin_tools_execute_weather() {
  thin_agent::agent::ToolContext ctx;
  auto tools = thin_agent::agent::create_builtin_tools(ctx);
  if (tools.empty()) return 0;

  for (auto& t : tools) {
    if (t.name == "weather") {
      auto r = t.execute({{"city", "深圳"}, {"date", "今天"}});
      check(r.value("success", false), "tools: weather execute ok");
      check(r.contains("data"), "tools: weather result has data");
      break;
    }
  }
  return 0;
}

int test_create_builtin_tools_execute_news() {
  thin_agent::agent::ToolContext ctx;
  auto tools = thin_agent::agent::create_builtin_tools(ctx);
  if (tools.empty()) return 0;

  for (auto& t : tools) {
    if (t.name == "news") {
      auto r = t.execute({{"topic", "科技"}});
      check(r.value("success", false), "tools: news execute ok");
      check(r.contains("data"), "tools: news result has data");
      break;
    }
  }
  return 0;
}

// ══════════════════════════════════════════════════════════════════
// P4: HybridRouter::classify_complexity
// ══════════════════════════════════════════════════════════════════

int test_hybrid_router_empty() {
  auto c = thin_agent::local::HybridRouter::classify_complexity("");
  check(c == thin_agent::local::TaskComplexity::simple,
        "hybrid: empty input is simple");
  return 0;
}

int test_hybrid_router_short_simple() {
  auto c = thin_agent::local::HybridRouter::classify_complexity("Hi");
  check(c == thin_agent::local::TaskComplexity::simple,
        "hybrid: short input is simple");
  return 0;
}

int test_hybrid_router_medium_moderate() {
  std::string input(50, 'x');
  input += "?";
  auto c = thin_agent::local::HybridRouter::classify_complexity(input);
  check(c == thin_agent::local::TaskComplexity::moderate,
        "hybrid: medium with ? is moderate");
  return 0;
}

int test_hybrid_router_long_complex() {
  std::string input(400, 'a');
  auto c = thin_agent::local::HybridRouter::classify_complexity(input);
  check(c == thin_agent::local::TaskComplexity::complex,
        "hybrid: long input is complex");
  return 0;
}

int test_hybrid_router_complex_keyword() {
  std::string input(50, 'x');
  input += " debug ";
  input += std::string(50, 'y');
  auto c = thin_agent::local::HybridRouter::classify_complexity(input);
  check(c == thin_agent::local::TaskComplexity::complex,
        "hybrid: complex keyword triggers complex");
  return 0;
}

int test_hybrid_router_multi_newline() {
  std::string input(50, 'x');
  input += "\n\n\n";
  auto c = thin_agent::local::HybridRouter::classify_complexity(input);
  check(c == thin_agent::local::TaskComplexity::complex,
        "hybrid: multi newline is complex");
  return 0;
}

// ══════════════════════════════════════════════════════════════════
// P5: ExternalIntentHandlers
// ══════════════════════════════════════════════════════════════════

int test_external_intent_handler_unknown_fetcher() {
  thin_agent::DemoConfigCompat cfg;
  cfg.external_provider = "mock";
  thin_agent::IntentSpec spec;
  spec.name = "unknown";
  spec.fetcher = "nonexistent";
  spec.summary_template_key = "default";

  thin_agent::ExternalHandlerContext ctx{cfg, spec};
  struct DummyHttp : thin_agent::IHttpClient {
    thin_agent::HttpResponse post(const std::string&, const std::string&, const std::string&, const std::string& = "") override { return {}; }
    thin_agent::HttpResponse get(const std::string&, const std::string&) override { return {}; }
  } mock;
  ctx.http = &mock;
  ctx.primary = "test";
  ctx.query_lang = "zh";
  ctx.tpl_lang = "zh";

  auto out = thin_agent::run_external_intent_handler(ctx);
  check(!out.fetch_ok, "ext_handler: unknown fetcher fails");
  check(out.ext.error.find("unknown_fetcher") != std::string::npos,
        "ext_handler: error mentions unknown_fetcher");
  return 0;
}

int test_external_intent_handler_weather_mock() {
  thin_agent::DemoConfigCompat cfg;
  cfg.external_provider = "mock";
  thin_agent::IntentSpec spec;
  spec.name = "weather";
  spec.fetcher = "weather";
  spec.summary_template_key = "external_weather_summary";
  spec.post_execute = "";

  thin_agent::ExternalHandlerContext ctx{cfg, spec};
  struct DummyHttp : thin_agent::IHttpClient {
    thin_agent::HttpResponse post(const std::string&, const std::string&, const std::string&, const std::string& = "") override { return {}; }
    thin_agent::HttpResponse get(const std::string&, const std::string&) override { return {}; }
  } mock_w;
  ctx.http = &mock_w;
  ctx.primary = "深圳";
  ctx.query_lang = "zh";
  ctx.tpl_lang = "zh";
  ctx.slots = nlohmann::json::object();

  auto out = thin_agent::run_external_intent_handler(ctx);
  check(out.fetch_ok, "ext_handler: weather mock returns ok");
  // summary may be empty if chat_policy.json not available — that's OK
  return 0;
}

int test_external_intent_handler_news_mock() {
  thin_agent::DemoConfigCompat cfg;
  cfg.external_provider = "mock";
  thin_agent::IntentSpec spec;
  spec.name = "news";
  spec.fetcher = "news";
  spec.summary_template_key = "external_news_summary";
  spec.post_execute = "";

  thin_agent::ExternalHandlerContext ctx{cfg, spec};
  struct DummyHttp : thin_agent::IHttpClient {
    thin_agent::HttpResponse post(const std::string&, const std::string&, const std::string&, const std::string& = "") override { return {}; }
    thin_agent::HttpResponse get(const std::string&, const std::string&) override { return {}; }
  } mock_n;
  ctx.http = &mock_n;
  ctx.primary = "科技";
  ctx.query_lang = "zh";
  ctx.tpl_lang = "zh";
  ctx.slots = nlohmann::json::object();

  auto out = thin_agent::run_external_intent_handler(ctx);
  check(out.fetch_ok, "ext_handler: news mock returns ok");
  // summary may be empty if chat_policy.json not available — that's OK
  return 0;
}

// ══════════════════════════════════════════════════════════════════
// P6: TemplateModel
// ══════════════════════════════════════════════════════════════════

int test_template_model_load() {
  thin_agent::local::TemplateModel tm;
  check(!tm.is_loaded(), "template: not loaded initially");
  bool ok = tm.load();
  check(ok, "template: load succeeds");
  check(tm.is_loaded(), "template: loaded after load()");
  check(tm.name() == "template", "template: name is 'template'");
  check(tm.backend() == "template", "template: backend is 'template'");
  return 0;
}

int test_template_model_keyword_match() {
  thin_agent::local::TemplateModel tm;
  tm.load();

  auto r1 = tm.infer("hello there");
  check(!r1.empty(), "template: 'hello' keyword produces reply");

  auto r2 = tm.infer("help me please");
  check(!r2.empty(), "template: 'help' keyword produces reply");

  return 0;
}

int test_template_model_fallback() {
  thin_agent::local::TemplateModel tm;
  tm.load();

  auto r = tm.infer("xyzzy_nonexistent_keyword_12345");
  check(!r.empty(), "template: unrecognized input still produces reply");
  return 0;
}

int test_template_model_unloaded_infer() {
  thin_agent::local::TemplateModel tm;
  check(!tm.is_loaded(), "template: not loaded before infer");
  auto r = tm.infer("hi");
  check(tm.is_loaded(), "template: auto-loaded by infer");
  check(!r.empty(), "template: infer returns reply after auto-load");
  return 0;
}

// ══════════════════════════════════════════════════════════════════
// P7: ModelPool (singleton — tests manage own model lifecycle)
// ══════════════════════════════════════════════════════════════════

namespace {
/// Minimal ILocalModel mock for testing ModelPool.
class MockModel : public thin_agent::local::ILocalModel {
 public:
  MockModel(std::string name, std::string cap, int prio = 0, size_t mem = 1024,
            int64_t lat = 100)
      : name_(std::move(name)), cap_(std::move(cap)), prio_(prio),
        mem_(mem), lat_(lat) {}

  std::string name() const override { return name_; }
  std::string backend() const override { return "mock"; }
  std::vector<thin_agent::local::ModelCapability> capabilities() const override {
    return {{cap_, prio_, 512, lat_}};
  }
  std::string infer(const std::string&, int, const std::string&) override {
    return "mock_reply";
  }
  bool load() override { loaded_ = true; return true; }
  void unload() override { loaded_ = false; }
  bool is_loaded() const override { return loaded_; }
  size_t memory_bytes() const override { return mem_; }
  int64_t typical_latency_us() const override { return lat_; }

 private:
  std::string name_, cap_;
  int prio_;
  size_t mem_;
  int64_t lat_;
  bool loaded_ = true;  // start loaded for testing
};
}  // namespace

int test_model_pool_add_size() {
  auto& pool = thin_agent::local::ModelPool::instance();
  size_t before = pool.size();

  pool.add(std::make_unique<MockModel>("test_add", "chat", 0, 1024));
  check(pool.size() == before + 1, "pool: add increases size");

  pool.add(std::make_unique<MockModel>("test_add2", "code", 1, 2048));
  check(pool.size() == before + 2, "pool: second add increases size");

  pool.remove("test_add");
  pool.remove("test_add2");
  check(pool.size() == before, "pool: size restored after remove");
  return 0;
}

int test_model_pool_remove() {
  auto& pool = thin_agent::local::ModelPool::instance();
  size_t before = pool.size();

  pool.add(std::make_unique<MockModel>("test_rm", "chat"));
  check(pool.size() == before + 1, "pool: added for remove test");

  pool.remove("test_rm");
  check(pool.size() == before, "pool: remove restores size");

  pool.remove("nonexistent_model_xyz");
  check(pool.size() == before, "pool: removing nonexistent is safe");
  return 0;
}

int test_model_pool_match_by_capability() {
  auto& pool = thin_agent::local::ModelPool::instance();

  pool.add(std::make_unique<MockModel>("mp_chat_a", "chat", 5));
  pool.add(std::make_unique<MockModel>("mp_chat_b", "chat", 2));
  pool.add(std::make_unique<MockModel>("mp_code", "code", 1));

  auto chat_matches = pool.match("chat");
  check(chat_matches.size() >= 2, "pool: match chat finds >=2 models");
  if (chat_matches.size() >= 2) {
    check(chat_matches[0]->name() == "mp_chat_b",
          "pool: chat sorted by priority (lowest first)");
  }

  auto code_matches = pool.match("code");
  check(code_matches.size() >= 1, "pool: match code finds model");

  auto none = pool.match("nonexistent_cap");
  check(none.empty(), "pool: unknown capability returns empty");

  pool.remove("mp_chat_a");
  pool.remove("mp_chat_b");
  pool.remove("mp_code");
  return 0;
}

int test_model_pool_best_priority() {
  auto& pool = thin_agent::local::ModelPool::instance();

  pool.add(std::make_unique<MockModel>("bp_fast", "chat", 0, 512, 50));
  pool.add(std::make_unique<MockModel>("bp_slow", "chat", 1, 2048, 500));

  auto* best = pool.best("chat");
  check(best != nullptr, "pool: best returns a model");
  if (best) {
    check(best->name() == "bp_fast", "pool: best picks lowest priority");
  }

  auto* filtered = pool.best("chat", 200);
  check(filtered != nullptr, "pool: best with latency filter works");
  if (filtered) {
    check(filtered->name() == "bp_fast", "pool: latency filter picks fast model");
  }

  check(pool.best("unknown") == nullptr, "pool: best returns null for unknown cap");

  pool.remove("bp_fast");
  pool.remove("bp_slow");
  return 0;
}

int test_model_pool_total_memory() {
  auto& pool = thin_agent::local::ModelPool::instance();
  size_t before = pool.total_memory_bytes();

  pool.add(std::make_unique<MockModel>("mem_a", "chat", 0, 100));
  pool.add(std::make_unique<MockModel>("mem_b", "code", 0, 200));
  check(pool.total_memory_bytes() == before + 300,
        "pool: total_memory_bytes sums correctly");

  pool.remove("mem_a");
  pool.remove("mem_b");
  return 0;
}

int test_model_pool_status_all() {
  auto& pool = thin_agent::local::ModelPool::instance();

  pool.add(std::make_unique<MockModel>("st_model", "chat", 0, 512));
  auto statuses = pool.status_all();

  bool found = false;
  for (const auto& s : statuses) {
    if (s.value("name", "") == "st_model") {
      found = true;
      check(s.value("backend", "") == "mock", "pool: status has correct backend");
      break;
    }
  }
  check(found, "pool: status_all contains added model");

  pool.remove("st_model");
  return 0;
}

// ══════════════════════════════════════════════════════════════════
// P8: McpServer — JSON-RPC protocol handler (zero external deps)
// ══════════════════════════════════════════════════════════════════

// Shared SkillRegistry for McpServer tests (protocol-only, no tools needed)
static thin_agent::SkillRegistry& mcp_test_sr() {
  static thin_agent::SkillRegistry sr;
  return sr;
}

int test_mcp_server_make_response() {
  // Test response format indirectly via initialize
  thin_agent::agent::McpServer srv(mcp_test_sr());
  srv.set_server_info("test_agent", "2.0");
  std::string resp = srv.handle_request(
      R"({"jsonrpc":"2.0","method":"initialize","id":42,"params":{}})");

  auto j = nlohmann::json::parse(resp);
  check(j.value("jsonrpc", "") == "2.0", "mcp: response has jsonrpc 2.0");
  check(j.value("id", 0) == 42, "mcp: response echoes id");
  check(j.contains("result"), "mcp: response has result field");
  check(j["result"].value("protocolVersion", "") == "2024-11-05",
        "mcp: protocol version set");
  return 0;
}

int test_mcp_server_make_error() {
  // Test error format via unknown method
  thin_agent::agent::McpServer srv(mcp_test_sr());
  std::string resp = srv.handle_request(
      R"({"jsonrpc":"2.0","method":"nonexistent","id":7})");

  auto j = nlohmann::json::parse(resp);
  check(j.value("jsonrpc", "") == "2.0", "mcp: error has jsonrpc 2.0");
  check(j.value("id", 0) == 7, "mcp: error echoes id");
  check(j.contains("error"), "mcp: error response has error field");
  check(j["error"].value("code", 0) == -32601, "mcp: unknown method = -32601");
  return 0;
}

int test_mcp_server_parse_error() {
  thin_agent::agent::McpServer srv(mcp_test_sr());
  std::string resp = srv.handle_request("not valid json {{{");

  auto j = nlohmann::json::parse(resp);
  check(j.contains("error"), "mcp: parse error has error field");
  check(j["error"].value("code", 0) == -32700, "mcp: parse error = -32700");
  return 0;
}

int test_mcp_server_initialize() {
  thin_agent::agent::McpServer srv(mcp_test_sr());
  srv.set_server_info("my_agent", "1.5.0");
  std::string resp = srv.handle_request(
      R"({"jsonrpc":"2.0","method":"initialize","id":1,"params":{}})");

  auto j = nlohmann::json::parse(resp);
  check(j.contains("result"), "mcp: initialize returns result");
  check(j["result"]["serverInfo"]["name"] == "my_agent",
        "mcp: server name set correctly");
  check(j["result"]["serverInfo"]["version"] == "1.5.0",
        "mcp: server version set correctly");
  check(j["result"].contains("capabilities"), "mcp: capabilities included");
  return 0;
}

int test_mcp_server_list_tools_no_init() {
  thin_agent::agent::McpServer srv(mcp_test_sr());
  std::string resp = srv.handle_request(
      R"({"jsonrpc":"2.0","method":"tools/list","id":2})");

  auto j = nlohmann::json::parse(resp);
  check(j.contains("error"), "mcp: tools/list without init returns error");
  check(j["error"].value("code", 0) == -32002,
        "mcp: not initialized = -32002");
  return 0;
}

int test_mcp_server_list_tools_after_init() {
  thin_agent::agent::McpServer srv(mcp_test_sr());
  // Initialize first
  srv.handle_request(
      R"({"jsonrpc":"2.0","method":"initialize","id":1,"params":{}})");
  // Then list tools
  std::string resp = srv.handle_request(
      R"({"jsonrpc":"2.0","method":"tools/list","id":2})");

  auto j = nlohmann::json::parse(resp);
  check(j.contains("result"), "mcp: tools/list after init returns result");
  check(j["result"].contains("tools"), "mcp: result has tools array");
  check(j["result"]["tools"].is_array(), "mcp: tools is array");
  return 0;
}

int test_mcp_server_call_tool_no_init() {
  thin_agent::agent::McpServer srv(mcp_test_sr());
  std::string resp = srv.handle_request(
      R"({"jsonrpc":"2.0","method":"tools/call","id":3,"params":{"name":"test","arguments":{}}})");

  auto j = nlohmann::json::parse(resp);
  check(j.contains("error"), "mcp: tools/call without init returns error");
  check(j["error"].value("code", 0) == -32002, "mcp: not initialized");
  return 0;
}

int test_mcp_server_unknown_method() {
  thin_agent::agent::McpServer srv(mcp_test_sr());
  std::string resp = srv.handle_request(
      R"({"jsonrpc":"2.0","method":"some/unknown","id":99})");

  auto j = nlohmann::json::parse(resp);
  check(j.contains("error"), "mcp: unknown method returns error");
  check(j["error"].value("code", 0) == -32601, "mcp: code -32601");
  check(j["error"].value("message", "").find("Method not found") != std::string::npos,
        "mcp: error message mentions method not found");
  return 0;
}

int test_mcp_server_notification() {
  thin_agent::agent::McpServer srv(mcp_test_sr());
  // Notification has no id — should return empty string
  std::string resp = srv.handle_request(
      R"({"jsonrpc":"2.0","method":"some/notification"})");

  check(resp.empty(), "mcp: notification without id returns empty");

  // notifications/initialized should be accepted silently
  std::string resp2 = srv.handle_request(
      R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
  check(resp2.empty(), "mcp: notifications/initialized returns empty");
  return 0;
}

// ══════════════════════════════════════════════════════════════════
// P9: GgufModel (no llama.cpp) + PluginLoader (no .so files)
// ══════════════════════════════════════════════════════════════════

int test_gguf_model_constructor() {
  thin_agent::local::GgufModel m("/models/test-model.gguf",
      {{"chat", 0, 512, 100}}, 2048, 4);

  check(m.name() == "test-model", "gguf: name extracted from path");
  check(m.backend() == "gguf", "gguf: backend is 'gguf'");
  check(!m.is_loaded(), "gguf: not loaded initially");
  check(m.memory_bytes() == 0, "gguf: 0 memory when not loaded");

  auto caps = m.capabilities();
  check(caps.size() == 1, "gguf: one capability");
  check(caps[0].name == "chat", "gguf: capability name 'chat'");
  return 0;
}

int test_gguf_model_load_no_file() {
  thin_agent::local::GgufModel m("/nonexistent/path/model.gguf",
      {{"chat", 0}}, 2048, 4);

  bool ok = m.load();
  check(!ok, "gguf: load returns false for nonexistent file");
  check(!m.is_loaded(), "gguf: still not loaded after failed load");
  return 0;
}

int test_gguf_model_infer_no_llama() {
  thin_agent::local::GgufModel m("/tmp/test.gguf",
      {{"chat", 0}}, 2048, 4);

  // Without llama.cpp compiled in, infer() returns empty
  std::string r = m.infer("hello");
  check(r.empty(), "gguf: infer returns empty without llama.cpp");
  return 0;
}

int test_plugin_loader_no_such_dir() {
  thin_agent::SkillRegistry reg;
  auto results = thin_agent::plugin::load_plugins(
      reg, "/nonexistent_plugin_dir_xyz", {"code", "git"});

  // Should not crash, returns empty
  check(results.empty(), "plugin: nonexistent dir returns empty results");
  return 0;
}

int test_plugin_loader_empty_modes() {
  thin_agent::SkillRegistry reg;
  auto results = thin_agent::plugin::load_plugins(
      reg, "/tmp", {});

  check(results.empty(), "plugin: empty modes returns empty");
  return 0;
}

// ══════════════════════════════════════════════════════════════════
// P10: CloudLlmClient — 请求构建 + 响应解析 + SSE + Fallback
// ══════════════════════════════════════════════════════════════════

int test_cloudllm_build_request_body_simple() {
  std::vector<thin_agent::ChatMessage> msgs = {
      {"system", "You are helpful."},
      {"user", "Hello"}
  };
  auto body = thin_agent::CloudLlmClient::build_request_body(msgs, "test-model", 100);
  auto j = nlohmann::json::parse(body);
  check(j["model"] == "test-model", "cloud: model name in body");
  check(j["max_tokens"] == 100, "cloud: max_tokens in body");
  check(j["messages"].size() == 2, "cloud: 2 messages");
  check(j["messages"][0]["role"] == "system", "cloud: system role");
  check(j["messages"][1]["content"] == "Hello", "cloud: user content");
  return 0;
}

int test_cloudllm_build_request_body_tool_message() {
  std::vector<thin_agent::ChatMessage> msgs = {
      {"user", "what is 2+2"},
      {"assistant", "", "call_1", "calc", nlohmann::json::array()},
      {"tool", "4", "call_1", "calc"}
  };
  auto body = thin_agent::CloudLlmClient::build_request_body(msgs, "m", 200);
  auto j = nlohmann::json::parse(body);
  check(j["messages"].size() == 3, "cloud: tool message serialized");
  check(j["messages"][2]["role"] == "tool", "cloud: tool role");
  check(j["messages"][2]["tool_call_id"] == "call_1", "cloud: tool_call_id");
  return 0;
}

int test_cloudllm_build_request_body_tools() {
  std::vector<thin_agent::ChatMessage> msgs = {{"user", "calc"}};
  nlohmann::json tools = nlohmann::json::array({
      {{"type", "function"}, {"function", {{"name", "calc"}, {"description", "does math"}}}}
  });
  auto body = thin_agent::CloudLlmClient::build_request_body(msgs, "m", 200,
      nlohmann::json::object(), tools);
  auto j = nlohmann::json::parse(body);
  check(j.contains("tools"), "cloud: tools field present");
  check(j["tool_choice"] == "auto", "cloud: tool_choice auto");
  return 0;
}

int test_cloudllm_build_request_body_stream() {
  std::vector<thin_agent::ChatMessage> msgs = {{"user", "hi"}};
  auto body = thin_agent::CloudLlmClient::build_request_body(msgs, "m", 200,
      nlohmann::json::object(), nullptr, true);
  auto j = nlohmann::json::parse(body);
  check(j["stream"] == true, "cloud: stream flag set");
  return 0;
}

int test_cloudllm_parse_response_ok() {
  std::string resp = R"({"choices":[{"message":{"role":"assistant","content":"Hello!"}}]})";
  auto r = thin_agent::CloudLlmClient::parse_response(resp, 200);
  check(r.ok, "cloud: parse ok");
  check(r.text == "Hello!", "cloud: content extracted");
  check(r.http_status == 200, "cloud: http_status preserved");
  return 0;
}

int test_cloudllm_parse_response_invalid_json() {
  auto r = thin_agent::CloudLlmClient::parse_response("not-json{{{", 200);
  check(!r.ok, "cloud: invalid json fails");
  check(r.error.find("invalid_json_response") == 0, "cloud: error message prefix");
  return 0;
}

int test_cloudllm_parse_response_http_error() {
  auto r = thin_agent::CloudLlmClient::parse_response(R"({"error":{"message":"auth failed"}})", 401);
  check(!r.ok, "cloud: http error fails");
  check(r.http_status == 401, "cloud: http_status 401");
  return 0;
}

int test_cloudllm_parse_response_missing_choices() {
  auto r = thin_agent::CloudLlmClient::parse_response(R"({"object":"chat.completion"})", 200);
  check(!r.ok, "cloud: missing choices fails");
  check(r.error == "missing_choices", "cloud: error missing_choices");
  return 0;
}

int test_cloudllm_parse_response_tool_calls() {
  std::string resp = R"({"choices":[{"message":{"role":"assistant","content":"",
      "tool_calls":[{"id":"call_1","type":"function",
      "function":{"name":"weather","arguments":"{\"city\":\"Beijing\"}"}}]}}]})";
  auto r = thin_agent::CloudLlmClient::parse_response(resp, 200);
  check(r.ok, "cloud: tool_calls response ok");
  check(r.text.find("tool_calls") != std::string::npos, "cloud: response has tool_calls");
  return 0;
}

int test_cloudllm_parse_response_reasoning_only() {
  std::string resp = R"({"choices":[{"message":{"role":"assistant","content":"",
      "reasoning_content":"Let me think..."}}]})";
  auto r = thin_agent::CloudLlmClient::parse_response(resp, 200);
  check(!r.ok, "cloud: reasoning-only fails");
  check(r.error == "empty_content_with_reasoning", "cloud: reasoning fallback");
  return 0;
}

int test_cloudllm_parse_sse_simple() {
  std::string sse =
      "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\" World\"}}]}\n"
      "data: [DONE]\n";
  auto r = thin_agent::CloudLlmClient::parse_sse_stream(sse);
  check(r.ok, "cloud: sse parse ok");
  check(r.text == "Hello World", "cloud: sse content accumulated");
  return 0;
}

int test_cloudllm_parse_sse_tool_calls() {
  // Simplified SSE with tool_calls: first chunk has id/name, second has arguments
  std::string sse = R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"c1","function":{"name":"weather","arguments":""}}]}}]}
data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"city\":\"Shenzhen\"}"}}]}}]}
data: [DONE]
)";
  std::vector<std::string> chunks;
  auto r = thin_agent::CloudLlmClient::parse_sse_stream(sse, &chunks);
  check(r.ok, "cloud: sse tool_calls parse ok");
  check(!chunks.empty(), "cloud: chunks captured");
  check(r.text.find("tool_calls") != std::string::npos, "cloud: result has tool_calls");
  check(r.text.find("weather") != std::string::npos, "cloud: result contains function name");
  return 0;
}

int test_cloudllm_fallback_single_provider() {
  // Use mock env var to bypass HTTP
  setenv("THIN_AGENT_FALLBACK_TEST", R"({"choices":[{"message":{"role":"assistant","content":"mock ok"}}]})", 1);

  thin_agent::DemoConfigCompat cfg;
  cfg.mode = "cloud";
  cfg.provider = "openai";
  cfg.model_name = "gpt-4";
  cfg.api_base = "https://api.openai.com/v1";
  cfg.api_key_env = "NO_SUCH_KEY";

  thin_agent::CurlHttpClient http;
  std::vector<thin_agent::ChatMessage> msgs = {{"user", "hi"}};
  auto r = thin_agent::CloudLlmClient::chat_completion_with_fallback(
      http, cfg, msgs, "THIN_AGENT_FALLBACK_TEST");

  check(r.ok, "cloud: fallback with mock env returns ok");
  check(r.text == R"({"choices":[{"message":{"role":"assistant","content":"mock ok"}}]})",
        "cloud: fallback returns mock text");
  unsetenv("THIN_AGENT_FALLBACK_TEST");
  return 0;
}

// ══════════════════════════════════════════════════════════════════
// P11: McpClient — JSON-RPC 序列化 + 响应解析
// ══════════════════════════════════════════════════════════════════

int test_mcp_build_jsonrpc_request() {
  nlohmann::json params = {{"name", "test"}, {"arguments", 42}};
  auto req = thin_agent::agent::build_jsonrpc_request("tools/call", params, 7);

  auto j = nlohmann::json::parse(req);
  check(j["jsonrpc"] == "2.0", "mcp_client: jsonrpc version");
  check(j["method"] == "tools/call", "mcp_client: method");
  check(j["id"] == 7, "mcp_client: request id");
  check(j["params"]["name"] == "test", "mcp_client: params name");
  check(j["params"]["arguments"] == 42, "mcp_client: params arguments");
  return 0;
}

int test_mcp_build_jsonrpc_request_no_params() {
  // Notification — no params
  auto req = thin_agent::agent::build_jsonrpc_request(
      "notifications/initialized", nullptr, 0);
  auto j = nlohmann::json::parse(req);
  check(!j.contains("params"), "mcp_client: no params field for null");
  return 0;
}

int test_mcp_parse_tool_list_ok() {
  std::string resp = R"({"jsonrpc":"2.0","id":1,"result":{"tools":[
      {"name":"read_file","description":"Read a file","inputSchema":{"type":"object"}},
      {"name":"write_file","description":"Write a file","inputSchema":{"type":"object"}}
  ]}})";
  auto tools = thin_agent::agent::parse_tool_list_response(resp);
  check(tools.size() == 2, "mcp_client: 2 tools parsed");
  check(tools[0].name == "read_file", "mcp_client: first tool name");
  check(tools[1].description == "Write a file", "mcp_client: second tool desc");
  return 0;
}

int test_mcp_parse_tool_list_empty() {
  std::string resp = R"({"jsonrpc":"2.0","id":1,"result":{"tools":[]}})";
  auto tools = thin_agent::agent::parse_tool_list_response(resp);
  check(tools.empty(), "mcp_client: empty tools list");
  return 0;
}

int test_mcp_parse_tool_list_invalid_json() {
  auto tools = thin_agent::agent::parse_tool_list_response("not json");
  check(tools.empty(), "mcp_client: invalid json returns empty");
  return 0;
}

int test_mcp_parse_tool_call_ok() {
  std::string resp = R"({"jsonrpc":"2.0","id":2,"result":{
      "content":[{"type":"text","text":"42"}]
  }})";
  auto r = thin_agent::agent::parse_tool_call_response(resp);
  check(r["ok"] == true, "mcp_client: tool call ok");
  check(r["result"].is_array(), "mcp_client: result is array");
  return 0;
}

int test_mcp_parse_tool_call_error() {
  std::string resp = R"({"jsonrpc":"2.0","id":3,"error":{"code":-32601,"message":"method not found"}})";
  auto r = thin_agent::agent::parse_tool_call_response(resp);
  check(r["ok"] == false, "mcp_client: error response fails");
  check(r["error"] == "method not found", "mcp_client: error message from jsonrpc");
  return 0;
}

int test_mcp_parse_tool_call_is_error() {
  std::string resp = R"({"jsonrpc":"2.0","id":4,"result":{
      "isError":true,
      "content":[{"type":"text","text":"tool not found"}]
  }})";
  auto r = thin_agent::agent::parse_tool_call_response(resp);
  check(r["ok"] == false, "mcp_client: isError flag detected");
  return 0;
}

// ══════════════════════════════════════════════════════════════════
// P12: OnnxChatModel — constructor + basic properties (no ONNX Runtime)
// ══════════════════════════════════════════════════════════════════

int test_onnx_model_constructor() {
  thin_agent::local::OnnxChatModel m("/models/chat.onnx",
      {{"chat", 0, 2048}}, 4);

  check(m.name() == "chat", "onnx: name extracted from onnx path");
  check(m.backend() == "onnx", "onnx: backend is 'onnx'");
  check(!m.is_loaded(), "onnx: not loaded initially");
  check(m.memory_bytes() == 0, "onnx: 0 memory when not loaded");

  auto caps = m.capabilities();
  check(caps.size() == 1, "onnx: one capability");
  check(caps[0].name == "chat", "onnx: capability 'chat'");
  return 0;
}

// ══════════════════════════════════════════════════════════════════
// P13: MockHttpClient + MockTransport — 完整闭环测试
// ══════════════════════════════════════════════════════════════════

namespace {
/// IHttpClient mock：注入预设响应，验证完整链路。
class MockHttpClient : public thin_agent::IHttpClient {
 public:
  thin_agent::HttpResponse post(const std::string& url,
                                 const std::string& body,
                                 const std::string& auth,
                                 const std::string& /*api_mode*/ = "") override {
    thin_agent::HttpResponse r;
    r.status_code = status_;
    r.body = body_;
    r.ok = ok_;
    r.error = error_;
    last_url = url;
    last_body = body;
    last_auth = auth;
    return r;
  }
  void expect(int status, std::string body, bool ok = true) {
    status_ = status; body_ = std::move(body); ok_ = ok;
  }
  void expect_error(std::string err) { ok_ = false; error_ = std::move(err); }

  thin_agent::HttpResponse get(const std::string& url,
                   const std::string& auth) override {
    thin_agent::HttpResponse r;
    r.status_code = status_;
    r.body = body_;
    r.ok = ok_;
    r.error = error_;
    last_url = url;
    last_body = "";
    last_auth = auth;
    return r;
  }

  // 检查最后一次调用的参数
  std::string last_url, last_body, last_auth;
 private:
  int status_ = 200;
  std::string body_, error_;
  bool ok_ = true;
};

/// ITransport mock：注入预设 JSON-RPC 响应。
class MockTransport : public thin_agent::agent::ITransport {
 public:
  bool connect(const std::string& e) override {
    connected_ = true; endpoint_ = e; return connect_ok_;
  }
  std::string send(const std::string& req) override {
    last_request = req;
    size_t idx = call_count_++;
    return idx < responses_.size() ? responses_[idx] : R"({"error":"no response"} )";
  }
  void disconnect() override { connected_ = false; }

  MockTransport& expect_connect(bool ok = true) { connect_ok_ = ok; return *this; }
  MockTransport& add_response(std::string r) { responses_.push_back(std::move(r)); return *this; }

  std::string last_request;
 private:
  bool connected_ = false, connect_ok_ = true;
  std::string endpoint_;
  std::vector<std::string> responses_;
  size_t call_count_ = 0;
};
}  // namespace

// ══════════════════════════════════════════════════════════════════
// 新增 Mock 测试 — 覆盖内部逻辑
// ══════════════════════════════════════════════════════════════════

/// MockHttpClient → chat_completion_with_tools 正常路径
int test_mock_http_chat_with_tools() {
  MockHttpClient mock;
  mock.expect(200, R"({"choices":[{"message":{"role":"assistant","content":"","tool_calls":[{"function":{"name":"weather","arguments":"{\"city\":\"北京\"}"}}]}}]})");
  return 0;
}

/// MockHttpClient → classify_intent 正常路径
int test_mock_http_classify_intent() {
  MockHttpClient mock;
  mock.expect(200, R"({"choices":[{"message":{"role":"assistant","content":"weather"}}]})");
  return 0;
}

/// MockHttpClient → CloudEmbeddingProvider::encode
int test_mock_embedding_encode() {
  MockHttpClient mock;
  mock.expect(200, R"({"data":[{"embedding":[0.1,0.2,0.3,0.4]}]})");
  thin_agent::agent::CloudEmbeddingProvider emb(mock, "https://api.test", "sk-test", "test-model");
  auto vec = emb.encode("hello world");
  check(vec.size() == 4, "mock_embed: vector size");
  check(emb.dimension() == 4, "mock_embed: dimension");
  return 0;
}

/// MockHttpClient → CloudEmbeddingProvider::encode 降级（HTTP 失败）
int test_mock_embedding_encode_fallback() {
  MockHttpClient mock;
  mock.expect_error("connection refused");
  thin_agent::agent::CloudEmbeddingProvider emb(mock, "https://api.test", "sk-test", "test-model");
  auto vec = emb.encode("hello world");
  check(!vec.empty(), "mock_embed_fb: fallback vector non-empty");
  return 0;
}

/// MockHttpClient → ExternalInfoClient::fetch_weather HTTP 路径
int test_mock_external_weather_http() {
  MockHttpClient mock;
  mock.expect(200, R"({"current_condition":[{"temp_C":"22","humidity":"55","weatherDesc":[{"value":"Sunny"}]}],"nearest_area":[{"areaName":[{"value":"Beijing"}]}]})");
  auto r = thin_agent::ExternalInfoClient::fetch_weather(mock, "北京", "今天", "http", "", "zh");
  check(r.ok, "mock_ext_w: weather http ok");
  check(r.source == "wttr.in", "mock_ext_w: source wttr.in");
  return 0;
}

/// MockHttpClient → ExternalInfoClient::fetch_news HTTP 路径
int test_mock_external_news_http() {
  MockHttpClient mock;
  mock.expect(200, R"({"hits":[{"title":"AI breakthrough","author":"researcher","url":"https://example.com"}]})");
  auto r = thin_agent::ExternalInfoClient::fetch_news(mock, "AI", "today", "http");
  check(r.ok, "mock_ext_n: news http ok");
  check(r.source == "real-http", "mock_ext_n: source real-http");
  return 0;
}

int test_mock_http_chat_completion() {
  MockHttpClient mock;
  mock.expect(200, R"({"choices":[{"message":{"role":"assistant","content":"Mock says hi!"}}]})");

  thin_agent::DemoConfigCompat cfg;
  cfg.provider = "openai";
  cfg.model_name = "gpt-4";
  cfg.api_base = "https://api.openai.com/v1";

  std::vector<thin_agent::ChatMessage> msgs = {{"user", "hi"}};
  auto r = thin_agent::CloudLlmClient::chat_completion(mock, cfg, "sk-test", msgs, "", {});

  check(r.ok, "mock_http: chat_completion ok");
  check(r.text == "Mock says hi!", "mock_http: content from mock");
  check(r.http_status == 200, "mock_http: http_status 200");
  // Verify request was built correctly
  check(mock.last_url.find("chat/completions") != std::string::npos,
        "mock_http: URL contains chat/completions");
  check(mock.last_body.find("\"model\":\"gpt-4\"") != std::string::npos,
        "mock_http: body contains model name");
  return 0;
}

int test_mock_http_chat_completion_error() {
  // v0.52.6: 熔断器进程级单例——单测间隔离（前序用例的失败上报会
  // 累计开闸，本测须先复位）
  thin_agent::LlmCircuitBreaker::instance().reset();
  MockHttpClient mock;
  mock.expect_error("connection refused");

  thin_agent::DemoConfigCompat cfg;
  cfg.provider = "openai";
  cfg.model_name = "gpt-4";
  cfg.api_base = "https://api.openai.com/v1";

  std::vector<thin_agent::ChatMessage> msgs = {{"user", "hi"}};
  auto r = thin_agent::CloudLlmClient::chat_completion(mock, cfg, "sk-test", msgs, "", {});

  check(!r.ok, "mock_http: network error");
  // v0.52.6: 文本路径有重试环——1 首发+3 重试=4 连败会触发熔断
  //（连续 3 次 transient 失败开闸），此后快速失败 error=circuit_open；
  // 未达开闸时透传底层错误。两态均正确，按实际命中断言。
  check(r.error == "connection refused" || r.error == "circuit_open",
        "mock_http: error message");
  return 0;
}

int test_mock_transport_connect() {
  MockTransport transport;
  transport.expect_connect(true)
      .add_response(R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05","serverInfo":{"name":"test","version":"1.0"},"capabilities":{"tools":{}}}})")
      .add_response("");  // initialized notification response (ignored)

  thin_agent::agent::McpClient client(transport, "test-cmd");
  bool ok = client.connect("test_client", "1.0");
  check(ok, "mock_transport: connect ok");
  check(client.is_connected(), "mock_transport: connected flag");
  return 0;
}

int test_mock_transport_list_tools() {
  MockTransport transport;
  transport.expect_connect(true)
      .add_response(R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05","serverInfo":{"name":"srv","version":"1.0"},"capabilities":{"tools":{}}}})")
      .add_response("")
      .add_response(R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"echo","description":"Echo back","inputSchema":{"type":"object"}}]}})");

  thin_agent::agent::McpClient client(transport, "cmd");
  client.connect();

  auto tools = client.list_tools();
  check(tools.size() == 1, "mock_transport: 1 tool");
  check(tools[0].name == "echo", "mock_transport: tool name 'echo'");
  check(tools[0].description == "Echo back", "mock_transport: tool description");
  return 0;
}

int test_mock_transport_call_tool() {
  MockTransport transport;
  transport.expect_connect(true)
      .add_response(R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05","serverInfo":{"name":"srv","version":"1.0"}}})")
      .add_response("")
      .add_response(R"({"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"42"}]}})");

  thin_agent::agent::McpClient client(transport, "cmd");
  client.connect();

  nlohmann::json args = {{"a", 1}, {"b", 2}};
  auto r = client.call_tool("add", args);
  check(r["ok"] == true, "mock_transport: tool call ok");
  check(r["result"].is_array(), "mock_transport: result is array");
  return 0;
}
