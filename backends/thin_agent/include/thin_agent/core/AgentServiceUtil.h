#pragma once
// v0.53.3: AgentService 私有工具层——原单文件匿名 namespace 的跨域声明。
// 定义在 AgentServiceUtil.cpp 的 namespace svc_util（迭代生成声明）。
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/ChatPolicy.h"

namespace thin_agent {
namespace svc_util {
constexpr int kMaxActiveSubAgents = 16;

struct EvidenceRow {
  int priority = 99;
  long long order = 0;
  std::string created_at;
  long long audit_id = -1;
  std::string kind;
  std::string task_id;
  std::string source;
  std::string row_ref;
  std::string text;
};

std::string resolve_api_key(CredentialPool* pool, const std::string& api_key_env, const std::string& provider);
std::string next_auto_trace(uint64_t seq);
std::string next_auto_cmd(uint64_t seq);
std::string to_lower_copy(std::string s);
bool contains_any(const std::string& haystack, const std::vector<std::string>& needles);
bool contains_any_policy(const std::string& haystack, const std::string& key);
std::string trim_copy(std::string s);
std::string extract_query_after_keywords(const std::string& text, const std::vector<std::string>& keywords);
bool is_unsigned_integer_token(const std::string& s);
std::string policy_style_default(const std::string& fallback = "normal");
std::string policy_style_text(const std::string& style, const std::string& key, const std::string& fallback = "");
std::string apply_style_prefix(const std::string& scope, const std::string& key, const std::string& body, const std::string& fallback_style = "normal");
bool is_skills_only_query(const std::string& text);
std::string build_profile_body(const std::string& lang, const agent::AgentRoleManager* role_mgr, const SkillRegistry& skill_registry, bool detailed = false, bool skills_only = false);
std::string replace_all_copy(std::string s, const std::string& from, const std::string& to);
std::string render_template(std::string tpl, const std::vector<std::pair<std::string, std::string>>& kv);
int evidence_priority_for_row(const std::string& kind, const std::string& task_state, const std::string& audit_to_state);
bool evidence_is_more_recent(const EvidenceRow& a, const EvidenceRow& b);
bool evidence_better_for_same_task(const EvidenceRow& cand, const EvidenceRow& cur);
EvidenceRow build_evidence_row(const nlohmann::json& row, long long order);
nlohmann::json build_task_audit_evidence_render_from_results(const nlohmann::json& results, const std::string& query, int limit_applied);
std::string build_task_audit_evidence_text_from_results(const nlohmann::json& results, const std::string& query, int limit_applied);
nlohmann::json build_task_audit_evidence_render_from_source_counts(const nlohmann::json& source_counts, const std::string& query, int limit_applied, const nlohmann::json& latest_basis = nlohmann::json::object());
nlohmann::json build_memory_replay_hint(const nlohmann::json& latest_basis, const std::string& query, int limit_applied, const std::string& req_type);
std::string pipeline_error_code_from_class(const std::string& error_class);
std::string pipeline_decision_reason(bool failed, const std::string& pipeline_error_code);
nlohmann::json pipeline_error_object(const std::string& error_class, const std::string& error_code, int failed_at);
nlohmann::json pipeline_policy_input(const std::string& policy, bool failed, int completed, const std::string& pipeline_error_code);
nlohmann::json pipeline_outcome_object(bool failed, const std::string& error_class, int failed_at);
std::string normalize_text_for_intent(std::string text);
bool contains_token(const std::unordered_set<std::string>& tokens, const std::string& word);
std::string resolve_intent_model_path();
bool is_model_runtime_query(const std::string& norm);;
std::string normalize_cloud_strategy_text(const std::string& s);
bool needs_translation(const std::string& query_lang, const std::string& source_lang);
bool contains_ascii_alpha(const std::string& text);
std::string apply_reply_translation_if_needed(const DemoConfigCompat& cfg, std::string reply, const std::string& query_lang, const std::string& template_lang, const std::string& sample);
bool should_route_open_queries_to_cloud(const std::string& mode);
size_t estimate_tokens(const std::string& text);
int resolve_context_window(const DemoConfigCompat& cfg);
void trim_memory_by_token_budget(std::vector<ChatMessage>& mem, size_t budget_tokens, int full_context_turns, size_t truncated_max_chars);
std::string scan_project_structure(const std::string& root_path, int max_depth);
std::string build_cloud_user_payload(const std::string& text, const nlohmann::json& intent_info, const DialogContext& dialog_ctx, const std::vector<ChatMessage>& recent_turns);
std::string strip_cloud_model_artifacts(const std::string& text);
std::string extract_json_object(const std::string& text);
std::string resolve_api_key(CredentialPool* pool,
                            const std::string& api_key_env,
                            const std::string& provider);
extern thread_local nlohmann::json g_cron_filter;
extern thread_local bool g_fc_dangerous_denied;
extern std::atomic<int> g_spawn_nest_depth;
extern std::atomic<int> g_active_sub_agents;



std::string apply_reply_translation_if_needed(const DemoConfigCompat& cfg, std::string reply, const std::string& query_lang, const std::string& template_lang, const std::string& sample);

bool intent_triggered(const IntentSpec& spec, const std::string& classified, const std::string& text, const std::string& text_lower);

using ApprovalChecker = std::function<bool(const std::string& tool_name,
                                            const nlohmann::json& args)>;

CloudChatResult run_function_calling_loop(
    const DemoConfigCompat& cfg,
    const std::string& api_key,
    const std::vector<ChatMessage>& initial_messages,
    const nlohmann::json& tools,
    const std::string& /*session_id*/,
    const std::string& /*idem_token*/,
    EventCallback on_event,
    SkillRegistry& skill_registry,
    int min_iterations = 3,
    int max_iterations = 30,
    bool use_zh_progress = true,
    std::function<void(const std::string& action,
                       const std::string& params_summary,
                       const std::string& output_truncated)> on_tool_result = nullptr,
    std::function<bool()> abort_checker = nullptr,
    ApprovalChecker approval_checker = nullptr,
    std::function<void(int, const nlohmann::json&)> journal_append = nullptr,
    /// v0.53.30: 最终轮流式回放（SSE 攒批下的近似流式）
    StreamCallback on_chunk = nullptr);

struct ToolExecResult {
  std::string name;
  std::string id;
  std::string output;
  bool success = false;
  std::string args_snippet;
};;

std::string intent_trace_layer_name(const std::string& backend);

bool is_open_knowledge_query(const std::string& norm);

bool parse_hook_event(const std::string& name, thin_agent::HookEvent* out);

std::string resolve_cloud_reply_text(const nlohmann::json& policy, const std::string& cloud_raw_text);

nlohmann::json sanitize_json_slots(const nlohmann::json& slots);

std::string translate_local(const std::string& text, const std::string& target_desc);

std::vector<std::string> translate_texts(const DemoConfigCompat& cfg, const std::vector<std::string>& texts, const std::string& target_lang, const std::string& target_sample); /// 策略 B：按 query/template 语言决定是否把整段回复送 LLM 翻译。 /// 当 template_lang 与 reply_target 相同但 policy 实际回退到英文模板时，按渲染文本语言补翻译。 std::string apply_reply_translation_if_needed(const DemoConfigCompat& cfg, std::string reply, const std::string& query_lang, const std::string& template_lang, const std::string& sample);

std::string apply_dialog_slot_by_spec(const IntentSpec& spec, const std::string& text, const DialogContext& dialog_ctx, nlohmann::json& slots, const std::string& current);
nlohmann::json classify_local_intent(const std::string& text);
std::string detect_slot_by_spec(const IntentSpec& spec, const std::string& text, const std::string& norm, nlohmann::json& slots);
ToolExecResult execute_one_tool( const CloudLlmClient::ParsedToolCall& tc, SkillRegistry& skill_registry, EventCallback on_event, bool use_zh_progress);
void fill_optional_slots_by_spec(const IntentSpec& spec, const std::string& norm, nlohmann::json& slots);
bool intent_predicate_matched(const IntentSpec& spec, const std::string& text, const std::string& text_lower);
bool intent_route_excluded(const IntentSpec& spec, const std::string& text, const std::string& text_lower);
bool is_bare_conversation_ack(const std::string& norm);
bool is_local_runtime_model_query(const std::string& norm);
bool is_messaging_capability_query(const std::string& norm);
void maybe_boost_weather_intent_from_dialog(nlohmann::json& intent_info, const DialogContext& dialog_ctx, const std::string& text);
std::string normalize_cloud_route_hint(std::string hint);
nlohmann::json parse_cloud_strategy_json(const std::string& text, const std::string& fallback_intent);
std::string pick_cloud_visible_reply(const nlohmann::json& policy);

}  // namespace svc_util
}  // namespace thin_agent
