// v0.53.1: meta 插件 —— 元工具域（memory_save/find/forget + session_search/
// session_recent + summarize + correction_record + find_tool/show_tool）。
//
// 从 AgentService::register_meta_memory_tools 迁出（核心瘦身第二批）。
// 事实库/会话库/摘要器/纠错库实例留核心（引擎注入点：resume 快照、
// system prompt 组装、summary_prefix、correction 注入），经
// ctx.config("meta") 地址注入——同 state 插件协议。
// find_tool/show_tool 用 registry.build_tools_schema()（编译期可见，
// 无注入需求）。
// json_coerce_int 用核心头 JsonCoerce.h（header-only）。

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "thin_agent/agent/ConversationSummarizer.h"
#include "thin_agent/agent/ErrorCorrectionStore.h"
#include "thin_agent/core/FactStore.h"
#include "thin_agent/core/JsonCoerce.h"
#include "thin_agent/core/SessionStore.h"
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/plugin/PluginContext.h"

namespace thin_agent {
namespace meta {

static FactStore* g_facts = nullptr;
static SessionStore* g_sessions = nullptr;
static agent::ConversationSummarizer* g_summarizer = nullptr;
static agent::ErrorCorrectionStore* g_corr = nullptr;
/// v0.53.50: 注入方存活锚——多实例 svc 析构后上述四个静态指针悬垂
///（R35 cron/R38 collab 同款),注册表键查询防 UAF,失效即视为不可用
static thin_agent::PluginContext* g_meta_ctx = nullptr;

static bool meta_ptrs_alive() {
  return thin_agent::PluginContext::is_alive(g_meta_ctx);
}

// ── 记忆（FactStore）──

nlohmann::json handle_memory_save(const nlohmann::json& params) {
  if (!g_facts || !meta_ptrs_alive())
    return {{"success", false}, {"error", "memory store not available"}};
  std::string path = params.value("path", "");
  std::string content = params.value("content", "");
  if (path.empty() || content.empty())
    return {{"success", false}, {"error", "path and content required"}};
  if (path.find('/') == std::string::npos) path = "system/" + path;
  int64_t id = g_facts->save(path, content);
  if (id > 0) return {{"success", true}, {"id", id}, {"path", path}};
  return {{"success", false}, {"error", "save failed"}};
}

nlohmann::json handle_memory_find(const nlohmann::json& params) {
  if (!g_facts || !meta_ptrs_alive())
    return {{"success", false}, {"error", "memory store not available"}};
  std::string query = params.value("query", "");
  std::string prefix = params.value("prefix", "");
  int limit = json_coerce_int(params, "limit", 20);
  auto facts = g_facts->find(query, prefix, limit);
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& f : facts) {
    arr.push_back({{"id", f.id}, {"path", f.path}, {"content", f.content},
                   {"updated_at", f.updated_at}});
  }
  return {{"success", true}, {"results", arr}, {"count", facts.size()}};
}

nlohmann::json handle_memory_forget(const nlohmann::json& params) {
  if (!g_facts || !meta_ptrs_alive())
    return {{"success", false}, {"error", "memory store not available"}};
  int64_t id = params.value("id", 0);
  if (id <= 0) return {{"success", false}, {"error", "id required"}};
  bool ok = g_facts->forget(id);
  return {{"success", ok}, {"id", id}};
}

// ── 会话检索（SessionStore）──

nlohmann::json handle_session_search(const nlohmann::json& params) {
  if (!g_sessions || !meta_ptrs_alive())
    return {{"success", false}, {"error", "session store not available"}};
  std::string query = params.value("query", "");
  // v0.53.9: 空参（列全部）时 limit 默认 10→100——真网实测 GLM 用 find_tool
  // 断言"工具不存在"（load_skill 排 30+ 位被截断），误导后续决策
  int limit = json_coerce_int(params, "limit", query.empty() ? 100 : 10);
  auto results = g_sessions->search(query, limit);
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& m : results) {
    arr.push_back({{"id", m.id}, {"session_id", m.session_id},
                   {"role", m.role}, {"content", m.content},
                   {"created_at", m.created_at}});
  }
  return {{"success", true}, {"results", arr}, {"count", results.size()}};
}

nlohmann::json handle_session_recent(const nlohmann::json& params) {
  if (!g_sessions || !meta_ptrs_alive())
    return {{"success", false}, {"error", "session store not available"}};
  int limit = json_coerce_int(params, "limit", 20);
  auto results = g_sessions->recent(limit);
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& m : results) {
    arr.push_back({{"id", m.id}, {"session_id", m.session_id},
                   {"role", m.role}, {"content", m.content},
                   {"created_at", m.created_at}});
  }
  return {{"success", true}, {"results", arr}, {"count", results.size()}};
}

// ── 摘要（ConversationSummarizer）──

nlohmann::json handle_summarize(const nlohmann::json&) {
  if (!g_summarizer || !meta_ptrs_alive())
    return {{"success", false}, {"error", "summarizer not available"}};
  return {{"success", true}, {"summary", g_summarizer->summarize()}};
}

// ── 纠错（ErrorCorrectionStore）──

nlohmann::json handle_correction_record(const nlohmann::json& params) {
  if (!g_corr || !meta_ptrs_alive())
    return {{"success", false}, {"error", "correction store not available"}};
  std::string tool = params.value("tool", "");
  std::string error = params.value("error", "");
  std::string fix = params.value("fix", "");
  if (tool.empty() || error.empty())
    return {{"success", false}, {"error", "tool and error required"}};
  auto id = g_corr->record(tool, error, fix);
  return {{"success", true}, {"id", id}};
}

// ── 工具发现（registry 自身，无注入）──

static std::string to_lower(const std::string& s) {
  std::string r;
  for (char c : s) r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

nlohmann::json handle_find_tool(SkillRegistry& reg,
                                const nlohmann::json& params) {
  std::string query = params.value("query", "");
  // v0.53.9: 空参（列全部）时 limit 默认 10→100——真网实测 GLM 用 find_tool
  // 断言"工具不存在"（load_skill 排 30+ 位被截断），误导后续决策
  int limit = json_coerce_int(params, "limit", query.empty() ? 100 : 10);
  auto all_schema = reg.build_tools_schema();
  nlohmann::json results = nlohmann::json::array();
  std::string q_lower = to_lower(query);
  for (const auto& t : all_schema) {
    if (static_cast<int>(results.size()) >= limit) break;
    auto& fn = t["function"];
    std::string name = fn["name"];
    std::string desc = fn.value("description", "");
    std::string haystack = to_lower(name + " " + desc);
    bool match = false;
    std::istringstream ss(q_lower);
    std::string word;
    while (ss >> word) {
      if (haystack.find(word) != std::string::npos) { match = true; break; }
    }
    if (match || q_lower.empty()) {
      nlohmann::json r;
      r["name"] = name;
      r["description"] = desc;
      if (fn.contains("parameters") && fn["parameters"].contains("properties")) {
        nlohmann::json params_list = nlohmann::json::array();
        for (const auto& [k, v] : fn["parameters"]["properties"].items())
          params_list.push_back(k);
        r["params"] = params_list;
      }
      results.push_back(r);
    }
  }
  return {{"success", true}, {"results", results}, {"count", results.size()}};
}

nlohmann::json handle_show_tool(SkillRegistry& reg,
                                const nlohmann::json& params) {
  std::string name = params.value("name", "");
  if (name.empty())
    return {{"success", false}, {"error", "name_required"}};
  auto all_schema = reg.build_tools_schema();
  for (const auto& t : all_schema) {
    if (t["function"]["name"] == name)
      return {{"success", true}, {"tool", t["function"]}};
  }
  return {{"success", false}, {"error", "tool_not_found"}, {"name", name}};
}

}  // namespace meta
}  // namespace thin_agent

extern "C" const char* thin_agent_plugin_init2(
    thin_agent::SkillRegistry& registry, thin_agent::PluginContext& ctx) {
  using namespace thin_agent;
  using thin_agent::meta::g_facts;
  using thin_agent::meta::g_sessions;
  using thin_agent::meta::g_summarizer;
  using thin_agent::meta::g_corr;
  auto svc = ctx.config("meta");
  thin_agent::meta::g_meta_ctx = &ctx;  // v0.53.50: 存活锚(四指针共享)
  if (svc.is_object()) {
    auto p = svc.value("fact_store", static_cast<int64_t>(0));
    if (p) g_facts = reinterpret_cast<FactStore*>(p);
    p = svc.value("session_store", static_cast<int64_t>(0));
    if (p) g_sessions = reinterpret_cast<SessionStore*>(p);
    p = svc.value("summarizer", static_cast<int64_t>(0));
    if (p) g_summarizer = reinterpret_cast<agent::ConversationSummarizer*>(p);
    p = svc.value("correction_store", static_cast<int64_t>(0));
    if (p) g_corr = reinterpret_cast<agent::ErrorCorrectionStore*>(p);
  }

  using Handler = nlohmann::json (*)(const nlohmann::json&);
  registry.register_cpp_handler("memory_save", thin_agent::meta::handle_memory_save);
  registry.register_cpp_handler("memory_find", thin_agent::meta::handle_memory_find);
  registry.register_cpp_handler("memory_forget", thin_agent::meta::handle_memory_forget);
  registry.register_cpp_handler("session_search", thin_agent::meta::handle_session_search);
  registry.register_cpp_handler("session_recent", thin_agent::meta::handle_session_recent);
  registry.register_cpp_handler("summarize", thin_agent::meta::handle_summarize);
  registry.register_cpp_handler("correction_record", thin_agent::meta::handle_correction_record);
  registry.register_cpp_handler("find_tool",
      [&registry](const nlohmann::json& p) {
        return thin_agent::meta::handle_find_tool(registry, p);
      });
  registry.register_cpp_handler("show_tool",
      [&registry](const nlohmann::json& p) {
        return thin_agent::meta::handle_show_tool(registry, p);
      });
  return "meta";
}
