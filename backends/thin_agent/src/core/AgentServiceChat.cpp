#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/HookSystem.h"  // v0.52.29
#include "thin_agent/core/JsonCoerce.h"
#include "thin_agent/core/JsonExtract.h"

#include "thin_agent/Version.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <future>
#include <map>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <deque>
#include <cctype>
#include <regex>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <sstream>
#include <unordered_map>

#include "thin_agent/llm/ProviderFactory.h"
#include "thin_agent/llm/CloudLlmClient.h"
#include "thin_agent/llm/CurlHttpClient.h"
#include "thin_agent/core/CredentialPool.h"
#include "thin_agent/core/Utf8Util.h"
#include "thin_agent/core/ExternalInfoClient.h"
#include "thin_agent/core/ExternalIntentHandlers.h"
#include "thin_agent/core/PathValidator.h"
#include "thin_agent/core/DialogSlotRecall.h"
#include "thin_agent/core/IntentOnnx.h"
#include "thin_agent/core/IntentScorer.h"
#include "thin_agent/core/ChatPolicy.h"
#include "thin_agent/core/WorkflowManager.h"
#include "thin_agent/core/MediaPlan.h"
#include "thin_agent/RuntimePaths.h"
#include "thin_agent/local/HybridRouter.h"
#include "thin_agent/local/TemplateModel.h"
#include "thin_agent/local/GgufModel.h"
#include "thin_agent/local/ModelPool.h"
#include "thin_agent/agent/AgentLoop.h"
#include "thin_agent/agent/ToolCallContext.h"
#include "thin_agent/agent/AgentTracer.h"
#include "thin_agent/agent/EmbeddingProvider.h"
#include "thin_agent/agent/ToolRegistry.h"
#include "thin_agent/agent/AgentRole.h"
#include "thin_agent/plugin/PluginLoader.h"
#include "thin_agent/Version.h"
#include "thin_agent/agent/TokenBudget.h"
#include "thin_agent/agent/ConvergenceTracker.h"
#include "thin_agent/agent/McpClient.h"
#include "thin_agent/agent/StdioTransport.h"
#include "thin_agent/agent/HttpTransport.h"
#include "thin_agent/core/PseudoTerminal.h"
#include "thin_agent/core/BackgroundProcessManager.h"
#include "thin_agent/core/CommandValidator.h"
#include "thin_agent/core/SandboxExecutor.h"
#include "thin_agent/log/LogEvent.h"


#include "thin_agent/core/AgentServiceUtil.h"

namespace thin_agent {

using namespace thin_agent::svc_util;

bool AgentService::is_file_op_request(const std::string& norm) {
  // 必须有路径分隔符 + 文件操作动词。操作词列表覆盖中英文
  // （创建/写入/生成/删除/复制… + list_dir/read_file/write_file/code/patch）。
  // 若路径/内容含 thin_agent 等字样而缺少操作词，仍会被 profile
  // 规则捕获——这是预期行为（用户确实在谈论 agent 自身时）。
  if (norm.find('/') == std::string::npos) return false;
  return norm.find("列出") != std::string::npos ||
         norm.find("查看") != std::string::npos ||
         norm.find("读取") != std::string::npos ||
         norm.find("搜索") != std::string::npos ||
         norm.find("list_dir") != std::string::npos ||
         norm.find("read_file") != std::string::npos ||
         norm.find("search") != std::string::npos ||
         norm.find("write_file") != std::string::npos ||
         norm.find("code") != std::string::npos ||
         norm.find("patch") != std::string::npos ||
         norm.find("创建") != std::string::npos ||
         norm.find("写入") != std::string::npos ||
         norm.find("生成") != std::string::npos ||
         norm.find("新建") != std::string::npos ||
         norm.find("删除") != std::string::npos ||
         norm.find("移动") != std::string::npos ||
         norm.find("复制") != std::string::npos ||
         norm.find("追加") != std::string::npos ||
         norm.find("touch") != std::string::npos ||
         norm.find("mkdir") != std::string::npos ||
         norm.find("编辑") != std::string::npos ||
         norm.find("修改") != std::string::npos;
}

void AgentService::append_decision_audit(const std::string& session_id,
                                         const std::string& text,
                                         const nlohmann::json& result,
                                         std::chrono::steady_clock::time_point started_at) const {
  const char* enabled = std::getenv("THIN_AGENT_DECISION_AUDIT");
  if (!enabled || !*enabled || std::string(enabled) == "0") return;

  const char* path_env = std::getenv("THIN_AGENT_DECISION_AUDIT_PATH");
  const std::string path =
      (path_env && *path_env) ? std::string(path_env) : default_decision_audit_path();

  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started_at)
                              .count();
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

  nlohmann::json decision = nlohmann::json::object();
  if (result.contains("decision") && result["decision"].is_object()) {
    decision = result["decision"];
  }

  nlohmann::json row = {
      {"ts_ms", now_ms},
      {"session_id", session_id},
      {"text", text},
      {"type", result.value("type", "")},
      {"mode_used", result.value("mode_used", "")},
      {"intent_backend", result.value("intent_backend", "")},
      {"route", decision.value("route", "")},
      {"intent", decision.value("intent", "")},
      {"policy", decision.value("policy", "")},
      {"confidence", decision.value("confidence", 0.0)},
      {"latency_ms", elapsed_ms},
  };
  if (decision.contains("reason") && decision["reason"].is_string()) {
    row["reason"] = decision["reason"];
  }
  if (result.contains("cloud_http_status") && result["cloud_http_status"].is_number_integer()) {
    row["cloud_http_status"] = result["cloud_http_status"];
  }

  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream out(path, std::ios::app);
  if (!out.is_open()) return;
  out << row.dump() << "\n";
}

/// v0.50.0: 思考进度推送（原 handle_chat 内 push_thinking lambda）。
void AgentService::chat_push_thinking(const ChatContext& ctx, int tier,
                                      const std::string& key,
                                      const std::vector<std::pair<std::string, std::string>>& params) const {
  if (ctx.on_event) {
    ctx.on_event("thinking", {{"tier", tier},
                              {"msg", progress_msg(key, ctx.use_zh_progress, params)},
                              {"cmd_id", ctx.cmd_id}});
  }
}

/// v0.50.0: 输入翻译域（原 handle_chat 段 A）。
void AgentService::chat_translate_input(ChatContext& ctx) {
  if (!needs_input_translation(ctx.text)) return;
  bool local_ok = false;
  auto candidates = thin_agent::local::ModelPool::instance().match("translate");
  if (!candidates.empty() && candidates.front()->is_loaded()) {
    std::string translated = translate_local(ctx.text, "English");
    if (!translated.empty()) {
      ctx.text_lower = to_lower_copy(translated);
      local_ok = true;
    }
  }
  if (!local_ok) {
    DemoConfigCompat tr_cfg = cfg_;
    tr_cfg.model_name = translate_input_config().cloud_model;
    tr_cfg.request_timeout_ms = translate_input_config().cloud_timeout_ms;
    CurlHttpClient http(tr_cfg.request_timeout_ms);  // v0.52.3: 超时对齐配置
    auto tr_res = CloudLlmClient::chat_completion_with_fallback(
        http, tr_cfg,
        {ChatMessage{"system", translate_input_config().cloud_prompt},
         ChatMessage{"user", ctx.text}});
    if (tr_res.ok && !tr_res.text.empty()) {
      ctx.text_lower = to_lower_copy(tr_res.text);
    }
  }
}

/// v0.50.0: 统一本地意图回复组装（原 handle_chat 内 local_reply lambda）。
/// 捕获语义迁移：intent_info 调用时求值（域方法可能已改写 ctx.intent_info）。
nlohmann::json AgentService::make_local_reply(ChatContext& ctx,
                                              const std::string& route,
                                              const std::string& reason,
                                              const std::string& reply,
                                              nlohmann::json observation,
                                              nlohmann::json tool_calls,
                                              const std::string& intent,
                                              double confidence,
                                              nlohmann::json slots,
                                              const std::string& policy) {
  const auto& text = ctx.text;
  const auto& intent_info = ctx.intent_info;
  const auto& session_id = ctx.session_id;
  const auto& snapshot = ctx.snapshot;
  const auto& query_lang = ctx.query_lang;
  const auto& tpl_lang = ctx.tpl_lang;
  const auto& complex_intent_detected = ctx.complex_intent_detected;
  const auto& probe_markers = ctx.probe_markers;
  const auto& force_cloud_for_complex = ctx.force_cloud_for_complex;

  if (!observation.is_object()) {
    observation = nlohmann::json::object();
  }
  observation["complex_intent_probe"] = {
      {"detected", complex_intent_detected},
      {"markers", probe_markers},
  };
  observation["cloud_complex_intent_gate"] = {
      {"enabled", cfg_.complex_intent_force_cloud},
      {"detected", complex_intent_detected},
      {"forced", force_cloud_for_complex},
  };

  nlohmann::json decision = {
      {"route", route},
      {"reason", reason},
      {"policy", policy},
  };
  if (!intent.empty()) decision["intent"] = intent;
  decision["confidence"] = confidence;
  if (!slots.empty()) decision["slots"] = slots;

  const std::unordered_map<std::string, std::string> local_style_key_map = {
      {"local_status", "status_prefix"},
      {"local_events", "event_prefix"},
      {"local_memory_recent", "memory_recent_prefix"},
      {"local_memory_history", "memory_history_prefix"},
      {"local_memory_search", "memory_search_prefix"},
      {"local_memory_summary", "memory_summary_prefix"},
      {"local_task_inline", "task_prefix"},
      {"local_action", "action_prefix"},
      {"local_external_clarify", "external_clarify_prefix"},
      {"local_external_weather", "external_summary_prefix"},
      {"local_external_news", "external_summary_prefix"},
      {"local_external_search", "external_summary_prefix"},
  };
  std::string reply_text = reply;
  if (const auto it = local_style_key_map.find(route); it != local_style_key_map.end()) {
    reply_text = apply_style_prefix("local", it->second, reply_text, "normal");
  }

  std::string inferred_backend = "rules";
  if (intent_info.contains("backend") && intent_info["backend"].is_string()) {
    inferred_backend = intent_info["backend"].get<std::string>();
  }
  // 确定性本地媒体/任务/动作路由以 rules 为准（fuzzy media.* 仅作旁路增强）
  if (route == "local_task_inline" || route == "local_action" || route == "local_task_pipeline" ||
      route == "local_media_status" || route == "local_media_capability" || route == "local_media_clarify") {
    inferred_backend = "rules";
  }
  const std::string intent_layer = intent_trace_layer_name(inferred_backend);

  nlohmann::json decision_trace = nlohmann::json::array({
      {{"layer", intent_layer}, {"input", text}, {"output", nlohmann::json{{"intent", intent}, {"confidence", confidence}}}},
      {{"layer", "probe"}, {"input", text}, {"output", nlohmann::json{{"complex_intent_probe", {{"detected", complex_intent_detected}, {"markers", probe_markers}}}}}},
      {{"layer", "policy"},
       {"input", [&]() {
          nlohmann::json in = {{"intent", intent}, {"reason", reason}, {"route", route}, {"policy", policy}};
          if (!slots.empty()) {
            if (slots.contains("query") && slots["query"].is_string()) in["query"] = slots["query"];
            if (slots.contains("limit") && slots["limit"].is_number_integer()) in["limit"] = slots["limit"];
          }
          return in;
        }()},
       {"output", nlohmann::json{{"route", route}, {"policy", policy}}}}
  });

  observation["query_lang"] = query_lang;
  observation["template_lang"] = tpl_lang;
  const std::string translated_reply =
      apply_reply_translation_if_needed(cfg_, reply_text, query_lang, tpl_lang, text);
  if (translated_reply != reply_text) {
    reply_text = translated_reply;
    observation["reply_translation_applied"] = true;
    if (!observation.contains("translation_applied") || !observation["translation_applied"].get<bool>()) {
      observation["translation_applied"] = true;
    }
  }

  auto result = nlohmann::json{
      {"type", "chat_result"},
      {"mode_used", "local-agent"},
      {"intent_backend", inferred_backend},
      {"memory_size", snapshot.size()},
      {"text", reply_text},
      {"decision", std::move(decision)},
      {"tool_calls", std::move(tool_calls)},
      {"observation", std::move(observation)},
      {"decision_trace", std::move(decision_trace)},
  };
  if (policy == "execute" && !intent.empty()) {
    note_dialog_state(session_id, intent, route, slots);
  }

  return result;
}

/// v0.50.0: 意图分类域（原 handle_chat 段 B）。
/// rules → fuzzy → ONNX 三级级联 + 开放知识改写 + 复杂意图探测 +
/// 多轮槽位续接。槽位续接命中时返回最终回复（调用方直接 return），
/// 否则返回 null 继续后续路由域。
nlohmann::json AgentService::chat_classify_intent(ChatContext& ctx) {
  ctx.intent_info = classify_local_intent(ctx.text);

  const double rules_conf = ctx.intent_info.value("confidence", 0.0);
  const std::string rules_intent = ctx.intent_info.value("intent", "unknown");
  const bool media_keyword_reserved =
      contains_any_policy(ctx.text_lower, "task_capture_inline") ||
      contains_any_policy(ctx.text_lower, "action_capture") ||
      contains_any_policy(ctx.text_lower, "action_start_recording") ||
      contains_any_policy(ctx.text_lower, "task_start_recording_inline");
  if (rules_intent == "unknown" || rules_conf < intent_execute_threshold()) {
    const DialogContext* ctx_ptr = ctx.dialog_ctx.last_intent.empty() ? nullptr : &ctx.dialog_ctx;
    const auto fuzzy = classify_intent_fuzzy(ctx.text, ctx_ptr);
    const double fuzzy_conf = fuzzy.value("confidence", 0.0);
    const std::string fuzzy_intent = fuzzy.value("intent", "unknown");
    if (fuzzy_intent != "unknown" && fuzzy_conf > rules_conf && fuzzy_conf >= intent_clarify_threshold()) {
      // 媒体关键词已有确定性路由时，不把 ctx.intent_info 改成 fuzzy media.*（避免 backend 漂移）
      // media 族仍由后续 MediaPlan + refine_media_plan_with_fuzzy 消费。
      if (!(media_keyword_reserved && is_media_family_intent(fuzzy_intent))) {
        ctx.intent_info = fuzzy;
      }
    }
  }

  const std::string intent_after_fuzzy = ctx.intent_info.value("intent", "unknown");
  const double post_fuzzy_conf = ctx.intent_info.value("confidence", 0.0);
  const bool reserved_for_deterministic_route = media_keyword_reserved;
  const bool onnx_route_guard =
      !is_model_runtime_query(ctx.text_lower) && !is_messaging_capability_query(ctx.text_lower) &&
      !is_bare_conversation_ack(ctx.text_lower) && !is_open_knowledge_query(ctx.text_lower);
  if (!reserved_for_deterministic_route && onnx_route_guard) {
    const DialogContext* ctx_ptr = ctx.dialog_ctx.last_intent.empty() ? nullptr : &ctx.dialog_ctx;
    const auto onnx = classify_intent_onnx(ctx.text, ctx_ptr);
    const double onnx_conf = onnx.value("confidence", 0.0);
    const std::string onnx_intent = onnx.value("intent", "unknown");
    if (onnx_intent != "unknown" && onnx_intent != "general" && onnx_conf >= intent_clarify_threshold()) {
      static const std::unordered_set<std::string> kOnnxExecutableIntents = {
          "profile", "weather", "news", "status", "memory_recent", "event_recent"};
      if (kOnnxExecutableIntents.count(onnx_intent)) {
        const bool rules_unknown = intent_after_fuzzy == "unknown";
        const bool rules_borderline = post_fuzzy_conf < intent_execute_threshold();
        const bool profile_conflict =
            intent_after_fuzzy == "profile" && !is_profile_like_text(ctx.text) && onnx_intent != "profile";
        // 文件操作请求（含路径）不应被 ONNX 覆盖为 profile
        // v0.45.4: 复用 is_file_op_request（含中文操作词）
        const bool file_op_context = AgentService::is_file_op_request(ctx.text_lower);
        if ((rules_unknown || rules_borderline || profile_conflict) && !(file_op_context && onnx_intent == "profile")) {
          if (rules_unknown || profile_conflict || onnx_conf > post_fuzzy_conf) {
            ctx.intent_info = onnx;
            if (onnx.contains("scores")) ctx.intent_info["scores"] = onnx["scores"];
          }
        }
      }
    }
  }

  maybe_boost_weather_intent_from_dialog(ctx.intent_info, ctx.dialog_ctx, ctx.text);

  if (is_open_knowledge_query(ctx.text_lower)) {
    ctx.intent_info = {{"intent", "general"},
                   {"confidence", 0.92},
                   {"backend", "rules"},
                   {"slots", nlohmann::json::object()}};
  }

  const double kExecuteThreshold = intent_execute_threshold();
  const double kClarifyThreshold = intent_clarify_threshold();

  // 多轮槽位续接：有 pending intent 时优先尝试填槽
  {
    auto slot_resume = try_fill_slot_state(ctx.session_id, ctx.text);
    if (!slot_resume.is_null()) {
          return slot_resume;
    }
  }

  const auto complex_markers = complex_intent_markers();
  ctx.probe_markers = nlohmann::json::array();
  for (const auto& it : complex_markers) {
    if (ctx.text_lower.find(it.first) != std::string::npos) {
      ctx.probe_markers.push_back(it.second);
    }
  }
  ctx.complex_intent_detected = !ctx.probe_markers.empty();
  ctx.force_cloud_for_complex = (cfg_.mode == "cloud" && cfg_.complex_intent_force_cloud && ctx.complex_intent_detected);


  return nullptr;
}

/// v0.50.0: 本地意图路由域（原 handle_chat 段 C）。
/// 配置驱动的本地意图循环（profile / status / event / memory_* /
/// onnx_knowledge / supported_languages 等特殊 predicate）。
/// 命中返回最终回复，未命中返回 null（调用方继续外部/媒体/云端域）。
nlohmann::json AgentService::chat_route_local_intents(ChatContext& ctx) {
  const bool status_advice_like = contains_any_policy(ctx.text_lower, "status_advice_suggest") &&
                                  contains_any_policy(ctx.text_lower, "status_advice_status") &&
                                  contains_any_policy(ctx.text_lower, "status_advice_task_exec");

  // 配置驱动的本地意图（profile / status / event / memory_* / 特殊 predicate）
  if (!ctx.force_cloud_for_complex) {
    const auto local_names = local_intent_names();
    const bool classified_is_local =
        !ctx.classified_intent.empty() &&
        std::find(local_names.begin(), local_names.end(), ctx.classified_intent) != local_names.end();
    for (const auto& local_name : local_names) {
      IntentSpec local_spec;
      if (!load_intent_spec(local_name, &local_spec) || local_spec.kind != "local") continue;
      if (intent_route_excluded(local_spec, ctx.text, ctx.text_lower)) continue;

      bool triggered = intent_triggered(local_spec, ctx.classified_intent, ctx.text, ctx.text_lower);
      // v0.46.0: classified 为明确的非 local 意图（如 autonomy=cloud）时，
      // 禁止 local 意图靠关键词子串抢跑（实测："监控...运行情况" 含 status
      // 关键词 → status local 模板拦截，monitor 工具不可达）。仅接受
      // 精确意图命中。
      // v0.49.2: predicate 意图（onnx_knowledge/short_ack 等句式级判定）
      // 是强信号，不被 classified 关键词级结果覆盖——否则
      // "不是问你用的什么模型，ONNX模型是什么" 被 ONNX 分类为非 local
      // 后，onnx_knowledge 的精确命中也被抹掉，落回 status 抢跑。
      const bool predicate_matched =
          !local_spec.predicate.empty() &&
          intent_predicate_matched(local_spec, ctx.text, ctx.text_lower);
      // v0.49.2: autonomy 是"能力类别"（用户要自主行动）而非意图竞争者，
      // 不参与覆盖——否则"请做记忆摘要"被 ONNX 分为 autonomy 后
      // memory_summary 的 keyword 命中被抹掉，落入 offline 兜底。
      const bool classified_is_capability = (ctx.classified_intent == "autonomy");
      if (!classified_is_local && !ctx.classified_intent.empty() && !predicate_matched &&
          !classified_is_capability) {
        triggered = (ctx.classified_intent == local_spec.name);
      }

      if (local_spec.handler == "profile") {
        // 文件操作请求（含路径）不应被 profile 关键词子串误判
        // v0.45.4: 复用 is_file_op_request（含中文操作词）
        const bool file_op_context = AgentService::is_file_op_request(ctx.text_lower);
        if (file_op_context && !is_profile_like_text(ctx.text)) continue;
        triggered = triggered || is_profile_like_text(ctx.text) || is_skills_only_query(ctx.text);
        if (!triggered) continue;

        const auto profile_slots = ctx.intent_info.value("slots", nlohmann::json::object());
        const std::string profile_mode = profile_slots.value("profile_mode", "concise");
        const bool detailed_mode = (profile_mode == "detailed");
        const bool skill_query = is_skills_only_query(ctx.text);
        const std::string profile_body = build_profile_body(ctx.query_lang, role_mgr_.get(), skill_registry_, detailed_mode, skill_query);
        const std::string profile_text = apply_style_prefix(
            "profile",
            detailed_mode ? "detailed_prefix" : "concise_prefix",
            profile_body,
            "normal");
        const std::string reason =
            detailed_mode ? local_spec.reason_execute_detail : local_spec.reason_execute;
        return make_local_reply(ctx, 
            local_spec.route_execute.empty() ? "local_profile" : local_spec.route_execute,
            reason.empty() ? "user_asks_agent_identity" : reason,
            profile_text,
            {{"capabilities",
              nlohmann::json::array({"status", "event_recent", "metrics", "memory_recent",
                                     "memory_history", "action", "task_engine", "external_query"})},
             {"profile_mode", profile_mode}},
            nlohmann::json::array(),
            local_spec.name,
            ctx.intent_info.value("confidence", detailed_mode ? 0.95 : local_spec.default_confidence),
            profile_slots);
      }

      if (local_spec.handler == "status") {
        if (status_advice_like) continue;
        triggered = triggered || ctx.model_status_query || is_local_runtime_model_query(ctx.text_lower);
        if (!triggered) continue;

        auto st = status_payload();
        ctx.query_lang = detect_query_language(ctx.text);
        const std::string base_key =
            is_local_runtime_model_query(ctx.text_lower)
                ? "status.local_runtime"
                : (local_spec.summary_template_key.empty() ? "status.summary"
                                                          : local_spec.summary_template_key);
        const std::string status_text = render_template(
            policy_text_for_lang(base_key, ctx.query_lang, policy_text("status.summary", "")),
            {{"mode", cfg_.mode},
             {"model", cfg_.model_name},
             {"provider", normalize_provider(cfg_.provider)}});
        return make_local_reply(ctx, 
            local_spec.route_execute.empty() ? "local_status" : local_spec.route_execute,
            local_spec.reason_execute.empty() ? "status_intent" : local_spec.reason_execute,
            status_text,
            st,
            nlohmann::json::array({{{"tool", "status"}}}),
            local_spec.intent_label.empty() ? local_spec.name : local_spec.intent_label,
            local_spec.default_confidence);
      }

      if (local_spec.handler == "onnx_knowledge") {
        if (!triggered) continue;
        const std::string onnx_text = render_template(
            policy_text(local_spec.summary_template_key.empty() ? "status.onnx_explainer"
                                                              : local_spec.summary_template_key,
                        ""),
            {{"mode", cfg_.mode},
             {"model", cfg_.model_name},
             {"provider", normalize_provider(cfg_.provider)}});
        return make_local_reply(ctx, 
            local_spec.route_execute.empty() ? "local_status" : local_spec.route_execute,
            local_spec.reason_execute.empty() ? "onnx_knowledge_local" : local_spec.reason_execute,
            onnx_text,
            status_payload(),
            nlohmann::json::array({{{"tool", "status"}}}),
            local_spec.intent_label.empty() ? "status" : local_spec.intent_label,
            local_spec.default_confidence);
      }

      if (local_spec.handler == "supported_languages") {
        if (!triggered) continue;
        return make_local_reply(ctx, 
            local_spec.route_execute.empty() ? "local_supported_languages" : local_spec.route_execute,
            local_spec.reason_execute.empty() ? "supported_languages_query" : local_spec.reason_execute,
            render_supported_languages_answer(ctx.tpl_lang),
            {{"supported_languages_count", supported_language_count()}},
            nlohmann::json::array(),
            local_spec.intent_label.empty() ? local_spec.name : local_spec.intent_label,
            local_spec.default_confidence,
            nlohmann::json::object(),
            "execute");
      }

      if (!triggered) continue;

      if (local_spec.handler == "event_recent") {
        auto ev = event_recent_payload();
        int idx = 1;
        std::string event_text = render_policy_template(
            policy_text("event.list_header", ""),
            {{"count", std::to_string(ev.value("event_size", 0))}});
        if (ev.contains("events") && ev["events"].is_array()) {
          for (const auto& e : ev["events"]) {
            if (e.is_string()) {
              event_text += std::to_string(idx++) + ". " + e.get<std::string>() + "\n";
              if (idx > 10) break;
            }
          }
        }
        if (idx == 1) event_text += policy_text("event.empty", "");
        return make_local_reply(ctx, 
            local_spec.route_execute.empty() ? "local_events" : local_spec.route_execute,
            local_spec.reason_execute.empty() ? "event_intent" : local_spec.reason_execute,
            event_text,
            ev,
            nlohmann::json::array({{{"tool", "event_recent"}}}),
            local_spec.name,
            local_spec.default_confidence);
      }

      if (local_spec.handler == "memory_recent") {
        auto mr = memory_recent_payload(ctx.session_id);
        std::string mr_text = render_policy_template(
            policy_text("memory.recent.list_header", ""),
            {{"count", std::to_string(mr.value("memory_size", 0))}});
        if (mr.contains("memory") && mr["memory"].is_array()) {
          auto& mem = mr["memory"];
          for (size_t i = 0; i < mem.size() && i < 6; ++i) {
            if (mem[i].is_string()) {
              mr_text += std::to_string(i + 1) + ". " + mem[i].get<std::string>() + "\n";
            }
          }
        }
        if (mr.value("memory_size", 0) == 0) mr_text += policy_text("memory.recent.empty", "");
        return make_local_reply(ctx, 
            local_spec.route_execute.empty() ? "local_memory_recent" : local_spec.route_execute,
            local_spec.reason_execute.empty() ? "memory_recent_intent" : local_spec.reason_execute,
            mr_text,
            mr,
            nlohmann::json::array({{{"tool", "memory_recent"}}}),
            local_spec.name,
            local_spec.default_confidence);
      }

      if (local_spec.handler == "memory_history") {
        auto mh = memory_history_payload(20);
        std::string mh_text = render_policy_template(
            policy_text("memory.history.list_header", ""),
            {{"count", std::to_string(mh.value("history_size", 0))}});
        if (mh.contains("history") && mh["history"].is_array()) {
          auto& hist = mh["history"];
          for (size_t i = 0; i < hist.size() && i < 10; ++i) {
            if (hist[i].is_object()) {
              std::string hist_text = hist[i].value("text", hist[i].value("content", "?"));
              if (hist_text.size() > 60) hist_text = hist_text.substr(0, 60) + "...";
              mh_text += std::to_string(i + 1) + ". " + hist_text + "\n";
            }
          }
        }
        if (mh.value("history_size", 0) == 0) mh_text += policy_text("memory.history.empty", "");
        return make_local_reply(ctx, 
            local_spec.route_execute.empty() ? "local_memory_history" : local_spec.route_execute,
            local_spec.reason_execute.empty() ? "memory_history_intent" : local_spec.reason_execute,
            mh_text,
            mh,
            nlohmann::json::array({{{"tool", "memory_history"}, {"limit", 20}}}),
            local_spec.name,
            local_spec.default_confidence,
            {{"limit", 20}});
      }

      if (local_spec.handler == "memory_search") {
        if (!triggered) continue;
        const auto slots = ctx.intent_info.value("slots", nlohmann::json::object());
        const std::string query = slots.value("query", "");
        const int limit = json_coerce_int(slots, "limit", 20);
        auto ms = memory_search_payload(query, limit);
        std::string reply = policy_text(
            local_spec.summary_template_key.empty() ? "memory.search_summary"
                                                    : local_spec.summary_template_key,
            "");
        auto evidence_render = build_task_audit_evidence_render_from_results(
            ms.value("results", nlohmann::json::array()),
            query,
            ms.value("limit_applied", limit));
        const std::string evidence = evidence_render.value("text", std::string(""));
        if (!evidence.empty()) {
          reply = evidence;
          if (evidence_render.contains("latest_basis") &&
              evidence_render["latest_basis"].is_object()) {
            ms["evidence_latest_basis"] = evidence_render["latest_basis"];
          }
        }
        if (ms.contains("evidence_latest_basis") && ms["evidence_latest_basis"].is_object()) {
          ms["memory_replay_hint"] = build_memory_replay_hint(
              ms["evidence_latest_basis"],
              query,
              ms.value("limit_applied", limit),
              "memory_search");
        }
        return make_local_reply(ctx, 
            local_spec.route_execute.empty() ? "local_memory_search" : local_spec.route_execute,
            local_spec.reason_execute.empty() ? "memory_search_intent" : local_spec.reason_execute,
            reply,
            ms,
            nlohmann::json::array(
                {{{"tool", "memory_search"},
                  {"query", query},
                  {"limit", ms.value("limit_applied", limit)}}}),
            local_spec.name,
            local_spec.default_confidence,
            {{"query", query}, {"limit", ms.value("limit_applied", limit)}});
      }

      if (local_spec.handler == "memory_summary") {
        if (!triggered) continue;
        const auto slots = ctx.intent_info.value("slots", nlohmann::json::object());
        const std::string query = slots.value("query", "");
        const int limit = json_coerce_int(slots, "limit", 20);
        auto msu = memory_summary_payload(query, limit);
        std::string reply = policy_text(
            local_spec.summary_template_key.empty() ? "memory.summary_summary"
                                                    : local_spec.summary_template_key,
            "");
        auto evidence_render = build_task_audit_evidence_render_from_source_counts(
            msu.value("source_counts", nlohmann::json::object()),
            query,
            msu.value("limit_applied", limit),
            msu.value("evidence_latest_basis", nlohmann::json::object()));
        const std::string evidence = evidence_render.value("text", std::string(""));
        if (!evidence.empty()) {
          reply = evidence;
          if (evidence_render.contains("latest_basis") &&
              evidence_render["latest_basis"].is_object()) {
            msu["evidence_latest_basis"] = evidence_render["latest_basis"];
          }
        }
        if (msu.contains("evidence_latest_basis") && msu["evidence_latest_basis"].is_object()) {
          msu["memory_replay_hint"] = build_memory_replay_hint(
              msu["evidence_latest_basis"],
              query,
              msu.value("limit_applied", limit),
              "memory_summary");
        }
        return make_local_reply(ctx, 
            local_spec.route_execute.empty() ? "local_memory_summary" : local_spec.route_execute,
            local_spec.reason_execute.empty() ? "memory_summary_intent" : local_spec.reason_execute,
            reply,
            msu,
            nlohmann::json::array(
                {{{"tool", "memory_summary"},
                  {"query", query},
                  {"limit", msu.value("limit_applied", limit)}}}),
            local_spec.name,
            local_spec.default_confidence,
            {{"query", query}, {"limit", msu.value("limit_applied", limit)}});
      }
    }
  }

  if (is_weather_location_recall_query(ctx.text) && !ctx.force_cloud_for_complex) {
    const std::string recalled_city = recalled_slot_from_dialog(ctx.dialog_ctx, "city");
    if (!recalled_city.empty()) {
      const std::string reply = render_template(
          policy_text_for_lang("weather.location_recall", ctx.tpl_lang, ""),
          {{"city", localize_city_name(recalled_city, ctx.tpl_lang)}});
      return make_local_reply(ctx, 
          "local_memory_recent",
          "weather_location_recall",
          reply,
          {{"recalled_city", recalled_city},
           {"dialog_last_intent", ctx.dialog_ctx.last_intent},
           {"dialog_last_route", ctx.dialog_ctx.last_route}},
          nlohmann::json::array({{{"tool", "memory_recent"}}}),
          "memory_recent",
          0.95);
    }
  }

  if (is_news_topic_recall_query(ctx.text) && !ctx.force_cloud_for_complex) {
    const std::string recalled_topic = recalled_slot_from_dialog(ctx.dialog_ctx, "topic_or_scope");
    if (!recalled_topic.empty()) {
      const std::string reply = render_template(
          policy_text_for_lang("news.topic_recall", ctx.tpl_lang, ""),
          {{"topic", recalled_topic}});
      return make_local_reply(ctx, 
          "local_memory_recent",
          "news_topic_recall",
          reply,
          {{"recalled_topic", recalled_topic},
           {"dialog_last_intent", ctx.dialog_ctx.last_intent},
           {"dialog_last_route", ctx.dialog_ctx.last_route}},
          nlohmann::json::array({{{"tool", "memory_recent"}}}),
          "memory_recent",
          0.95);
    }
  }

  return nullptr;
}

/// v0.50.0: 外部意图路由域（原 handle_chat 段 D）。
/// 配置驱动的外部意图执行（fill slots → validate → fetch → render →
/// optional post_execute）+ external_clarify（general 泛检索澄清）。
/// 命中返回最终回复，未命中返回 null。
nlohmann::json AgentService::chat_route_external_intents(ChatContext& ctx) {
  // v0.50.0: snapshot → JSON 数组（原 handle_chat 内 snapshot_json lambda）
  auto snapshot_json = [&]() {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& item : ctx.snapshot) arr.push_back(item.content);
    return arr;
  };
  const double kExecuteThreshold = intent_execute_threshold();
  const double kClarifyThreshold = intent_clarify_threshold();
  const bool status_advice_like = contains_any_policy(ctx.text_lower, "status_advice_suggest") &&
                                  contains_any_policy(ctx.text_lower, "status_advice_status") &&
                                  contains_any_policy(ctx.text_lower, "status_advice_task_exec");
  const bool general_info_query_like =
      contains_any_policy(ctx.text_lower, "general_info_query_include") &&
      !contains_any_policy(ctx.text_lower, "general_info_query_exclude");

  // 配置驱动的外部意图执行：fill slots → validate → fetch → render → optional post_execute
  {
    IntentSpec ext_spec;
    const std::string intent_name = ctx.intent_info.value("intent", "");
    if (!ctx.force_cloud_for_complex && load_intent_spec(intent_name, &ext_spec) &&
        ext_spec.kind == "external") {
      auto slots = sanitize_json_slots(ctx.intent_info.value("slots", nlohmann::json::object()));
      const std::string norm_ext = normalize_text_for_intent(ctx.text);
      std::string primary =
          detect_slot_by_spec(ext_spec, ctx.text, norm_ext, slots);
      primary = apply_dialog_slot_by_spec(ext_spec, ctx.text, ctx.dialog_ctx, slots, primary);
      fill_optional_slots_by_spec(ext_spec, norm_ext, slots);

      double conf = ctx.intent_info.value("confidence", 0.0);
      if (ext_spec.boost_on_slot_filled && !primary.empty()) {
        conf = std::max(conf, kExecuteThreshold);
      }

      // v0.53.16: query 质量闸——词表截取的 primary 过长（>50 字符）说明
      // 用户句式复杂（词后跟着完整指令句而非干净搜索词），local 直答会用
      // 脏 query 搜出无关结果（真 e2e 实测："请用 web_search 工具确认一下
      // 2026年GCC…"整段成了 query）。放弃 local external，落 FC 让 LLM
      // 自行提炼 query+调用 web_search 工具（同源同质量）。
      // 仅闸 search 意图（fetcher=="search"）——weather/news 的 slot 是
      // 结构化提取不受词后截取影响。
      const bool dirty_search_query =
          ext_spec.fetcher == "search" && primary.size() > 50;

      const bool need_clarify =
          primary.empty() || (ext_spec.require_execute_threshold && conf < kExecuteThreshold);
      if (need_clarify) {
        const std::string why =
            primary.empty() ? ext_spec.reason_missing : ext_spec.reason_conf_below;
        slot_states_[ctx.session_id] = {
            ext_spec.name,
            nlohmann::json::object(),
            ext_spec.required_slots.empty()
                ? std::vector<std::string>{ext_spec.primary_slot}
                : ext_spec.required_slots,
            snapshot_json(),
            0};
        return make_local_reply(ctx, 
            ext_spec.route_clarify.empty() ? "local_external_clarify" : ext_spec.route_clarify,
            why,
            policy_text_for_lang(ext_spec.clarify_template_key, ctx.tpl_lang, ""),
            {{"external_intent", ext_spec.name},
             {"required_slot", ext_spec.primary_slot},
             {"thresholds", {{"execute", kExecuteThreshold}, {"clarify", kClarifyThreshold}}}},
            nlohmann::json::array(),
            ext_spec.name,
            conf,
            slots,
            "clarify");
      }

      // v0.53.16: 脏 query 放行 FC——不 local 直答也不 clarify（用户意图明确，
      // 只是句式复杂），落 FC 由 LLM 提炼 query+调工具。
      if (dirty_search_query) {
        log_event("intent", LogLevel::Info, "search: dirty query, deferring to FC",
                  {{"primary_len", primary.size()}, {"head", primary.substr(0, 40)}});
      } else {
      ExternalHandlerContext hctx{
          cfg_,
          ext_spec,
          ctx.text,
          ctx.query_lang,
          ctx.tpl_lang,
          primary,
          slots,
          [&](const std::vector<std::string>& texts,
              const std::string& target_lang,
              const std::string& sample) {
            return translate_texts(cfg_, texts, target_lang, sample);
          },
          http_client_.get()};
      const auto rendered = run_external_intent_handler(hctx);
      if (!rendered.fetch_ok) {
        return make_local_reply(ctx, 
            ext_spec.route_clarify.empty() ? "local_external_clarify" : ext_spec.route_clarify,
            ext_spec.reason_fetch_failed,
            policy_text_for_lang(ext_spec.fetch_failed_template_key, ctx.tpl_lang, ""),
            {{"external_intent", ext_spec.name},
             {"slots", slots},
             {"fetch_ok", false},
             {"error", rendered.ext.error}},
            nlohmann::json::array(),
            ext_spec.name,
            conf,
            slots,
            "clarify");
      }

      auto result = make_local_reply(ctx, 
          ext_spec.route_execute,
          ext_spec.reason_fetch_ok,
          rendered.summary,
          {{"external_intent", ext_spec.name},
           {"slots", slots},
           {"external_result", rendered.ext.data},
           {"external_source", rendered.ext.source},
           {"external_http_status", rendered.ext.http_status},
           {"query_lang", ctx.query_lang},
           {"template_lang", ctx.tpl_lang},
           {"translation_applied", rendered.field_translated},
           {"thresholds", {{"execute", kExecuteThreshold}, {"clarify", kClarifyThreshold}}}},
          nlohmann::json::array({rendered.tool_call}),
          ext_spec.name,
          conf,
          slots,
          "execute");

      if (ext_spec.post_execute == "weather_advice" && rendered.advice_slots.is_object() &&
          !rendered.advice_slots.empty()) {
        slot_states_[ctx.session_id] = {
            "weather_advice",
            rendered.advice_slots,
            {},
            snapshot_json(),
            0};
      }
      return result;
      }  // v0.53.16: !dirty_search_query 配对（脏 query 落 FC 不 local 直答）
    }
  }

  // 配置驱动的 external_clarify（general 泛检索澄清）
  if (!ctx.force_cloud_for_complex && !should_route_open_queries_to_cloud(cfg_.mode)) {
    for (const auto& clarify_name : intent_names_by_kind("external_clarify")) {
      IntentSpec ext_clarify;
      if (!load_intent_spec(clarify_name, &ext_clarify) || ext_clarify.kind != "external_clarify") {
        continue;
      }
      if (!intent_triggered(ext_clarify, ctx.classified_intent, ctx.text, ctx.text_lower)) continue;
      const double conf = ctx.intent_info.value("confidence", 0.0);
      if (status_advice_like) continue;
      const bool need_clarify = conf >= kClarifyThreshold && conf < kExecuteThreshold;
      if (!need_clarify) continue;
      slot_states_[ctx.session_id] = {
          ext_clarify.name,
          nlohmann::json::object(),
          {},
          snapshot_json(),
          0};
      const std::string label =
          ext_clarify.intent_label.empty() ? ext_clarify.name : ext_clarify.intent_label;
      return make_local_reply(ctx, 
          ext_clarify.route_clarify.empty() ? "local_external_clarify" : ext_clarify.route_clarify,
          ext_clarify.reason_missing.empty() ? ext_clarify.name + "_needs_scope" : ext_clarify.reason_missing,
          policy_text(ext_clarify.clarify_template_key, ""),
          {{"external_intent", ext_clarify.name},
           {"thresholds", {{"execute", kExecuteThreshold}, {"clarify", kClarifyThreshold}}}},
          nlohmann::json::array(),
          label,
          conf,
          ctx.intent_info.value("slots", nlohmann::json::object()),
          "clarify");
    }
  }

  return nullptr;
}

/// v0.50.0: 媒体计划域（原 handle_chat 段 E）。
/// 话轮门控（问 vs 令）+ 连拍/时长/复合步骤（复用 task pipeline）+
/// 媒体状态查询/能力澄清。命中返回最终回复，未命中返回 null。
nlohmann::json AgentService::chat_route_media_plan(ChatContext& ctx) {
  const bool media_keyword_reserved =
      contains_any_policy(ctx.text_lower, "task_capture_inline") ||
      contains_any_policy(ctx.text_lower, "action_capture") ||
      contains_any_policy(ctx.text_lower, "action_start_recording") ||
      contains_any_policy(ctx.text_lower, "task_start_recording_inline");
  const bool reserved_for_deterministic_route = media_keyword_reserved;
  const bool ambiguous_media = is_ambiguous_media_utterance(ctx.text, &ctx.dialog_ctx);
  // 媒体计划：话轮门控（问 vs 令）+ 连拍/时长/复合步骤（复用 task pipeline）
  {
    MediaPlan media_plan = parse_media_plan(ctx.text, &ctx.dialog_ctx);
    // 阶段2：用 media.* 模糊意图增强（短追问「完成了吗」等）
    {
      nlohmann::json media_fuzzy = ctx.intent_info;
      if (!is_media_family_intent(media_fuzzy.value("intent", ""))) {
        media_fuzzy = classify_intent_fuzzy(ctx.text, &ctx.dialog_ctx);
      }
      refine_media_plan_with_fuzzy(media_plan, media_fuzzy, &ctx.dialog_ctx);
    }
    // 阶段4：cloud classify 已移除，media_router_applied 恒为 false
    const bool media_router_applied = false;

    const auto media_obs_base = [&]() {
      return nlohmann::json{
          {"dialogue_act", dialogue_act_name(media_plan.dialogue_act)},
          {"ambiguous_media", ambiguous_media},
          {"media_router_applied", media_router_applied},
          {"media_plan",
           {{"parse_reason", media_plan.parse_reason},
            {"status_query", media_plan.status_query},
            {"capability_question", media_plan.capability_question},
            {"capture_count", media_plan.capture_count},
            {"record_duration_sec", media_plan.record_duration_sec},
            {"step_count", static_cast<int>(media_plan.steps.size())},
            {"fuzzy_intent", media_plan.fuzzy_intent},
            {"fuzzy_confidence", media_plan.fuzzy_confidence}}},
      };
    };

    if (media_plan.capability_question) {
      return make_local_reply(ctx, "local_media_capability", "media_capability_question",
                         policy_text("media.capability", ""), media_obs_base(), nlohmann::json::array(),
                         "media_capability", 0.95, nlohmann::json::object(), "clarify");
    }

    // 阶段4：歧义媒体句在无 Router 裁决时一律澄清，禁止关键词直执行
    if (ambiguous_media && !media_router_applied) {
      return make_local_reply(ctx, "local_media_clarify", "media_ambiguous_need_router",
                         policy_text("media.ambiguous_clarify",
                                     policy_text("media.question_blocked", "")),
                         media_obs_base(), nlohmann::json::array(), "media_ambiguous", 0.85,
                         {{"dialogue_act", dialogue_act_name(media_plan.dialogue_act)}}, "clarify");
    }


    if (media_plan.status_query) {
      std::string task_id;
      std::string action;
      std::string state = "unknown";
      nlohmann::json task = nlohmann::json::object();
      if (ctx.dialog_ctx.last_slots.is_object()) {
        if (ctx.dialog_ctx.last_slots.contains("task_id") && ctx.dialog_ctx.last_slots["task_id"].is_string()) {
          task_id = ctx.dialog_ctx.last_slots["task_id"].get<std::string>();
        }
        if (ctx.dialog_ctx.last_slots.contains("action") && ctx.dialog_ctx.last_slots["action"].is_string()) {
          action = ctx.dialog_ctx.last_slots["action"].get<std::string>();
        }
      }
      if (task_engine_) {
        if (!task_id.empty()) {
          task = task_engine_->get_task(task_id);
          if (task.value("exists", false)) {
            state = task.value("state", "unknown");
            if (action.empty()) action = task.value("action", "");
          }
        }
        if (task_id.empty() || !task.value("exists", false)) {
          const auto listed = task_engine_->list_tasks(5, "", "");
          if (listed.is_array()) {
            for (const auto& t : listed) {
              const std::string a = t.value("action", "");
              if (a == "capture_photo" || a == "start_recording" || a == "stop_recording") {
                task = t;
                task["exists"] = true;
                task_id = t.value("task_id", "");
                action = a;
                state = t.value("state", "unknown");
                break;
              }
            }
          }
        }
      }
      nlohmann::json obs = media_obs_base();
      obs["task"] = task;
      if (!task_id.empty() && task.value("exists", true)) {
        const std::string msg = render_template(
            policy_text("media.status_ok", ""),
            {{"action", action.empty() ? "media" : action},
             {"task_id", task_id},
             {"state", state}});
        return make_local_reply(ctx, "local_media_status", "media_status_query", msg, obs,
                           nlohmann::json::array({{{"tool", "task_get"}, {"task_id", task_id}}}),
                           "media_capture_status", 0.96,
                           {{"action", action}, {"task_id", task_id}, {"dialogue_act", "question"}});
      }
      // 无任务时尝试 fetch_capture_results
      const auto fetch = handle_action("fetch_capture_results", nlohmann::json::object());
      obs["fetch_capture_results"] = fetch;
      const int code = fetch.value("result", nlohmann::json::object()).value("code", 5004);
      if (code == 0) {
        return make_local_reply(ctx, "local_media_status", "media_status_fetch_results",
                           policy_text("media.status_ok", "最近一次拍照结果可用。"), obs,
                           nlohmann::json::array({{{"tool", "action"}, {"action", "fetch_capture_results"}}}),
                           "media_capture_status", 0.9,
                           {{"action", "fetch_capture_results"}, {"dialogue_act", "question"}});
      }
      return make_local_reply(ctx, "local_media_status", "media_status_empty",
                         policy_text("media.status_empty", ""), obs, nlohmann::json::array(),
                         "media_capture_status", 0.9, {{"dialogue_act", "question"}}, "clarify");
    }

    if (media_plan.media_related && should_block_side_effect_for_act(media_plan.dialogue_act)) {
      // 配置表兜底：任意 side_effect 媒体意图在 question 下澄清
      const auto gate = evaluate_intent_gate("media_capture_execute", dialogue_act_name(media_plan.dialogue_act),
                                             dialog_has_media_execute_context(&ctx.dialog_ctx));
      return make_local_reply(ctx, "local_media_clarify",
                         gate.reason.empty() ? "media_question_blocked" : gate.reason,
                         policy_text("media.question_blocked", ""), media_obs_base(),
                         nlohmann::json::array(), "media_question", 0.9,
                         {{"dialogue_act", "question"}, {"gate_action", gate.action}}, "clarify");
    }

    const bool run_local_media_plan =
        media_plan.dialogue_act == DialogueAct::Command && !media_plan.steps.empty() &&
        (media_plan_needs_pipeline(media_plan) || !reserved_for_deterministic_route || media_router_applied);

    bool has_conditional_complex = false;
    for (const auto& m : ctx.probe_markers) {
      if (!m.is_string()) continue;
      const std::string tag = m.get<std::string>();
      if (tag == "cn_if" || tag == "cn_else" || tag == "en_if" || tag == "en_else") {
        has_conditional_complex = true;
        break;
      }
    }

    // 顺序复合（再/然后）走本地 plan；「如果/否则」条件句仍交给云 pipeline
    if (run_local_media_plan && !has_conditional_complex) {
      if (!task_engine_) {
        return make_local_reply(ctx, "local_task_pipeline", "task_engine_unavailable",
                           policy_text("task.engine_unavailable", ""), media_obs_base(),
                           nlohmann::json::array({{{"tool", "task_submit"}, {"status", "skipped"}}}),
                           "media_plan", 0.9, {{"dialogue_act", "command"}});
      }

      // 参数校验：改用 SkillRegistry（替代硬编码 allowed_param_keys），无 skills 时 fallback
      const std::unordered_set<std::string> allowed_recording_modes = {"normal", "video", "audio"};

      auto validate_media_params = [&](const std::string& action, const nlohmann::json& p) -> std::string {
        const auto* ad = skill_registry_.find_action(action);
        if (!ad) {
          // fallback: 无 skills 配置时允许所有已知 action，仅校验 mode/count 范围
          if (action != "capture_photo" && action != "start_recording" && action != "stop_recording")
            return "invalid_action:" + action;
        } else if (!ad->params.empty()) {
          for (auto it = p.begin(); it != p.end(); ++it) {
            if (ad->params.find(it.key()) == ad->params.end()) return "param_not_allowed:" + it.key();
          }
        }
        if (action == "start_recording" && p.contains("mode") && p["mode"].is_string()) {
          const std::string mode = p["mode"].get<std::string>();
          if (allowed_recording_modes.find(mode) == allowed_recording_modes.end()) return "invalid_mode:" + mode;
        }
        if (p.contains("count") && p["count"].is_number_integer()) {
          int c = p["count"].get<int>();
          if (c < 1 || c > 9) return "invalid_count";
        }
        return "";
      };

      nlohmann::json pipeline_steps = nlohmann::json::array();
      nlohmann::json tool_calls = nlohmann::json::array();
      size_t completed = 0;
      bool failed = false;
      int failed_at = -1;
      std::string last_task_id;
      std::string last_action;

      for (size_t i = 0; i < media_plan.steps.size(); ++i) {
        const auto& step = media_plan.steps[i];
        const std::string& action = step.action;
        nlohmann::json params = step.params.is_object() ? step.params : nlohmann::json::object();

        nlohmann::json step_result = {
            {"index", static_cast<int>(i + 1)},
            {"action", action},
            {"submitted", false},
            {"success", false},
            {"params", params},
        };

        bool params_ok = true;
        std::string param_error;
        std::string validate_err = validate_media_params(action, params);
        if (!validate_err.empty()) {
          params_ok = false;
          param_error = validate_err;
        }

        if (!params_ok) {
          failed = true;
          failed_at = static_cast<int>(i + 1);
          step_result["error"] = param_error;
          pipeline_steps.push_back(step_result);
          break;
        }

        const std::string idem = "media-plan-" + ctx.session_id + "-" + std::to_string(ctx.snapshot.size()) + "-" +
                                 std::to_string(i + 1) + "-" + action + "-" + ctx.idem_token;
        const std::string task_id = task_engine_->submit_task(action, params, idem);
        const auto task = task_engine_->get_task(task_id);
        const std::string state = task.value("state", "unknown");
        const bool ok = (state == "success");
        step_result["submitted"] = true;
        step_result["success"] = ok;
        step_result["task_id"] = task_id;
        step_result["state"] = state;
        tool_calls.push_back({{"tool", "task_submit"}, {"action", action}, {"idempotency_key", idem}});
        tool_calls.push_back({{"tool", "task_get"}, {"task_id", task_id}});
        last_task_id = task_id;
        last_action = action;
        pipeline_steps.push_back(step_result);
        if (!ok) {
          failed = true;
          failed_at = static_cast<int>(i + 1);
          break;
        }
        ++completed;

        // 录像时长：start_recording 成功后按 duration_sec 等待，再执行后续 stop
        // 单测可设 THIN_AGENT_FAST_MEDIA=1 跳过真实等待
        if (action == "start_recording" && params.contains("duration_sec") &&
            params["duration_sec"].is_number_integer()) {
          const char* fast = std::getenv("THIN_AGENT_FAST_MEDIA");
          const bool skip_wait = (fast && *fast && std::string(fast) != "0");
          const int dur = params["duration_sec"].get<int>();
          if (!skip_wait && dur > 0) {
            step_result["duration_wait_sec"] = dur;
            pipeline_steps.back()["duration_wait_sec"] = dur;
            std::this_thread::sleep_for(std::chrono::seconds(dur));
          } else if (skip_wait) {
            pipeline_steps.back()["duration_wait_skipped"] = true;
          }
        }
      }

      nlohmann::json obs = media_obs_base();
      obs["pipeline_steps"] = pipeline_steps;
      obs["pipeline_outcome"] = {{"failed", failed},
                                 {"failed_at", failed_at},
                                 {"completed", static_cast<int>(completed)},
                                 {"step_count", static_cast<int>(media_plan.steps.size())}};

      nlohmann::json slots = {
          {"dialogue_act", "command"},
          {"pipeline", true},
          {"action", last_action},
          {"task_id", last_task_id},
          {"capture_count", media_plan.capture_count},
          {"record_duration_sec", media_plan.record_duration_sec},
      };

      if (failed) {
        const std::string msg = render_template(
            policy_text("media.pipeline_fail", ""),
            {{"failed_at", std::to_string(failed_at)}});
        return make_local_reply(ctx, "local_task_pipeline", "media_plan_failed", msg, obs, tool_calls, "media_plan",
                           0.85, slots);
      }
      const std::string msg = render_template(
          policy_text("media.pipeline_ok", ""),
          {{"step_count", std::to_string(media_plan.steps.size())},
           {"completed", std::to_string(completed)}});
      const std::string intent_label =
          media_plan.steps.size() > 1 ? "media_plan" : "media_capture_execute";
      return make_local_reply(ctx, "local_task_pipeline", "media_plan_executed", msg, obs, tool_calls, intent_label,
                         0.97, slots);
    }
  }

  return nullptr;
}

/// v0.50.0: 任务/动作/gate 意图域（原 handle_chat 段 F）。
/// 配置驱动的 task 意图（内联任务提交）+ action 意图（直接执行动作）+
/// gate 意图（budget 超预算澄清等，budget 变量随域内声明）。
/// 命中返回最终回复，未命中返回 null。
nlohmann::json AgentService::chat_route_task_action_gate(ChatContext& ctx) {
  const bool ambiguous_media = is_ambiguous_media_utterance(ctx.text, &ctx.dialog_ctx);
  // 配置驱动的 task 意图（内联任务提交）
  // 阶段4：歧义媒体句已在上方澄清或由 Router/media plan 处理，禁止关键词直执行
  if (!ctx.force_cloud_for_complex && !ambiguous_media) {
    for (const auto& task_name : intent_names_by_kind("task")) {
      IntentSpec task_spec;
      if (!load_intent_spec(task_name, &task_spec) || task_spec.kind != "task") continue;
      if (!intent_triggered(task_spec, ctx.classified_intent, ctx.text, ctx.text_lower)) continue;
      // 阶段3：intent_policy 表驱动副作用门控
      {
        const std::string act_name = dialogue_act_name(classify_dialogue_act(ctx.text));
        const auto gate = evaluate_intent_gate(task_spec.name, act_name,
                                               dialog_has_media_execute_context(&ctx.dialog_ctx));
        if (!gate.allow_execute) {
          return make_local_reply(ctx, "local_media_clarify",
                             gate.reason.empty() ? "media_question_blocked_task" : gate.reason,
                             policy_text("media.question_blocked", ""),
                             {{"dialogue_act", act_name},
                              {"blocked_intent", task_spec.name},
                              {"gate_action", gate.action},
                              {"side_effect", intent_has_side_effect(task_spec.name)}},
                             nlohmann::json::array(), "media_question", 0.9,
                             {{"dialogue_act", act_name}}, "clarify");
        }
      }
      const std::string action = task_spec.action_name;
      const std::string label =
          task_spec.intent_label.empty() ? task_spec.name : task_spec.intent_label;
      if (!task_engine_) {
        return make_local_reply(ctx, 
            task_spec.route_execute.empty() ? "local_task_inline" : task_spec.route_execute,
            task_spec.reason_fetch_failed.empty() ? "task_engine_unavailable" : task_spec.reason_fetch_failed,
            policy_text("task.engine_unavailable", ""),
            {{"task_engine_ready", false}},
            nlohmann::json::array({{{"tool", "task_submit"}, {"status", "skipped"}}}),
            label,
            0.90,
            {{"action", action}});
      }
      const std::string idem =
          "chat-inline-" + ctx.session_id + "-" + std::to_string(ctx.snapshot.size()) + "-" + action + "-" +
          ctx.idem_token;
      const std::string task_id = task_engine_->submit_task(action, nlohmann::json::object(), idem);
      const auto task = task_engine_->get_task(task_id);
      const std::string state = task.value("state", "unknown");
      const std::string msg = render_template(
          policy_text(task_spec.summary_template_key.empty() ? "task.capture_submitted"
                                                           : task_spec.summary_template_key,
                      ""),
          {{"task_id", task_id}, {"state", state}});
      return make_local_reply(ctx, 
          task_spec.route_execute.empty() ? "local_task_inline" : task_spec.route_execute,
          task_spec.reason_execute.empty() ? "task_expression" : task_spec.reason_execute,
          msg,
          {{"task", task}, {"inline_task", {{"action", action}, {"task_id", task_id}, {"state", state}}}},
          nlohmann::json::array({
              {{"tool", "task_submit"}, {"action", action}, {"idempotency_key", idem}},
              {{"tool", "task_get"}, {"task_id", task_id}}}),
          label,
          task_spec.default_confidence,
          {{"action", action}, {"task_id", task_id}});
    }
  }

  // 配置驱动的 action 意图（直接执行动作）
  if (!ctx.force_cloud_for_complex && !ambiguous_media) {
    for (const auto& action_name : intent_names_by_kind("action")) {
      IntentSpec action_spec;
      if (!load_intent_spec(action_name, &action_spec) || action_spec.kind != "action") continue;
      if (!intent_triggered(action_spec, ctx.classified_intent, ctx.text, ctx.text_lower)) continue;
      {
        const std::string act_name = dialogue_act_name(classify_dialogue_act(ctx.text));
        const auto gate = evaluate_intent_gate(action_spec.name, act_name,
                                               dialog_has_media_execute_context(&ctx.dialog_ctx));
        if (!gate.allow_execute) {
          return make_local_reply(ctx, "local_media_clarify",
                             gate.reason.empty() ? "media_question_blocked_action" : gate.reason,
                             policy_text("media.question_blocked", ""),
                             {{"dialogue_act", act_name},
                              {"blocked_intent", action_spec.name},
                              {"gate_action", gate.action},
                              {"side_effect", intent_has_side_effect(action_spec.name)}},
                             nlohmann::json::array(), "media_question", 0.9,
                             {{"dialogue_act", act_name}}, "clarify");
        }
      }
      const std::string action = action_spec.action_name;
      const std::string label =
          action_spec.intent_label.empty() ? action_spec.name : action_spec.intent_label;
      const auto action_result = handle_action(action, nlohmann::json::object());
      if (action == "capture_photo") {
        const int code = action_result["result"]["code"].get<int>();
        const bool ok = code == 0;
        const std::string msg = ok ? policy_text("action.capture_ok", "")
                                   : policy_text("action.capture_fail", "");
        return make_local_reply(ctx, 
            action_spec.route_execute.empty() ? "local_action" : action_spec.route_execute,
            action_spec.reason_execute.empty() ? "action_intent_capture_photo" : action_spec.reason_execute,
            msg,
            action_result,
            nlohmann::json::array({{{"tool", "action"}, {"action", action}}}),
            label,
            ok ? action_spec.default_confidence : 0.70,
            {{"action", action}});
      }
      return make_local_reply(ctx, 
          action_spec.route_execute.empty() ? "local_action" : action_spec.route_execute,
          action_spec.reason_execute.empty() ? "action_intent_" + action : action_spec.reason_execute,
          policy_text("action.start_recording_ok", ""),
          action_result,
          nlohmann::json::array({{{"tool", "action"}, {"action", action}}}),
          label,
          action_spec.default_confidence,
          {{"action", action}});
    }
  }

  return nullptr;
}

/// v0.50.0: 云端执行域（原 handle_chat 段 G2）。
/// tools schema + 简单文件操作 fast FC + 缓存 + checkpoint + 主 FC 循环
/// （含 HITL 审批）+ cloud_done 后处理。命中终态返回 final；云失败
/// need_fallback=true；成功但需 pipeline JSON 继续时 continue_pipeline=true。
AgentService::ChatCloudResult AgentService::chat_run_cloud(ChatContext& ctx,
                                                           const std::string& api_key,
                                                           const std::string& key_state,
                                                           const std::string& provider) {
  ChatCloudResult out;
  // ── v0.52.3: 复杂任务自动分解（L1 接线）────────────────────
  // 意图层命中 complex_task（keywords.complex_task 保守词表）→
  // 转 agent_decompose_and_run（LLM 分解→依赖波次→并行子代理→合成），
  // 结果落 GoalManager 任务树（父目标+子任务，跨会话可查进度）。
  // 失败（分解失败/无 orchestrator）回退单层 FC 循环——复杂路径是
  // 增强而非依赖。
  if (ctx.intent_info.value("intent", "") == "complex_task" && orchestrator_) {
    log_event("chat", LogLevel::Info, "complex task routed to decomposition",
              {{"text_len", ctx.text.size()}});
    nlohmann::json dreq;
    dreq["type"] = "agent_decompose_and_run";
    dreq["goal"] = ctx.text;
    dreq["use_goal_tree"] = true;  // v0.52.3: 分解结果落任务树
    nlohmann::json dresp = handle_request(ctx.session_id, dreq);
    if (dresp.value("ok", false)) {
      out.cloud.ok = true;
      out.cloud.http_status = 200;
      out.cloud.text = dresp.value("summary", dresp.dump());
      out.cloud.tool_names_used = {"agent_decompose_and_run"};
      out.need_fallback = false;
      return out;
    }
    log_event("chat", LogLevel::Warn, "decomposition failed, fallback to FC loop",
              {{"error", dresp.value("error", "unknown")},
               {"stage", dresp.value("stage", "")}});
    // 落到下方单层 FC 循环（增强失败不阻塞任务）
  }
  // ── 构建 function calling tools schema（fast 和 main 共用）──
  // v0.41.2: 只发送核心工具 → 其余通过 find_tool/show_tool 按需发现
  // v0.42.1: cron 任务的 skills/toolsets 过滤
  const nlohmann::json tools_schema = [&]() -> nlohmann::json {
    auto all_tools = skill_registry_.build_tools_schema();
    // cron 任务指定了 skills/toolsets → 使用扩展过滤
    if (!g_cron_filter.is_null() && g_cron_filter.is_object()
        && (g_cron_filter.contains("skills") || g_cron_filter.contains("toolsets"))) {
      nlohmann::json filtered = nlohmann::json::array();
      for (const auto& t : all_tools) {
        std::string name = t.value("name", "");
        if (name.empty()) continue;
        bool match = false;
        // skills: 工具名前缀匹配（git_ → git, code_ → code_dev）
        if (g_cron_filter.contains("skills")) {
          for (const auto& sk : g_cron_filter["skills"]) {
            std::string s = sk;
            if (!s.empty() && s.back() != '_') s += '_';
            if (name.rfind(s, 0) == 0) { match = true; break; }
          }
        }
        // toolsets: 简单分类匹配
        if (!match && g_cron_filter.contains("toolsets")) {
          for (const auto& ts : g_cron_filter["toolsets"]) {
            // v0.54.6: 改名 ts_name——此前与外层工具名变量 `t` 同名（-Wshadow=local 判官命中）
            std::string ts_name = ts;
            if (ts_name == "file" && (name == "read_file" || name == "write_file"
                || name == "list_dir" || name.rfind("code_", 0) == 0
                || name == "execute_code"))
              { match = true; break; }
            if (ts_name == "terminal" && (name == "shell_exec" || name == "process"))
              { match = true; break; }
            if (ts_name == "git" && name.rfind("git_", 0) == 0) { match = true; break; }
            if (ts_name == "cron" && name.rfind("cron_", 0) == 0) { match = true; break; }
            if (ts_name == "session" && (name.rfind("session_", 0) == 0
                || name == "session_search")) { match = true; break; }
            if (ts_name == "memory" && name.rfind("memory_", 0) == 0) { match = true; break; }
            if (ts_name == "all") { match = true; break; }
          }
        }
        if (match) filtered.push_back(t);
      }
      // 确保发现工具始终可用
      nlohmann::json discover = {{"name", "find_tool"}, {"description", "..."},
                                 {"parameters", nlohmann::json::object()}};
      nlohmann::json show = {{"name", "show_tool"}, {"description", "..."},
                             {"parameters", nlohmann::json::object()}};
      filtered.push_back(discover);
      filtered.push_back(show);
      return filtered;
    }
    // 默认：核心工具子集
    static const std::unordered_set<std::string> core_tools = {
      "read_file", "write_file", "list_dir",
      "code_read_file", "code_write_file", "code_patch", "code_search",
      "shell_exec", "session_search", "session_recent", "execute_code",
      "find_tool", "show_tool",
      "memory_save", "memory_find", "memory_forget",
      "git_status", "git_diff", "git_log", "git_add", "git_commit",
      "cron_add", "cron_list", "cron_remove", "cron_stats",
      "cron_update", "cron_pause", "cron_resume", "cron_trigger",
      "skill_list", "skill_view", "skill_manage",
      "spawn_agent", "delegate_task"
    };
    nlohmann::json filtered = nlohmann::json::array();
    for (const auto& t : all_tools) {
      auto name = t["function"]["name"].get<std::string>();
      if (core_tools.find(name) != core_tools.end())
        filtered.push_back(t);
    }
    return filtered;
  }();
  const char* mock_env = std::getenv("THIN_AGENT_TEST_CLOUD_RESPONSE");
  const bool use_native_fc = (tools_schema.is_array() && !tools_schema.empty() && !mock_env);

  // v0.51.2: HITL 审批回调 — 定义于 fast/main 两分支之前（两通道共用，
  // 修复 fast 通道危险工具无审批直通的漏洞；原 v0.49.0 仅主通道接入）
  auto fc_approval_cb = [this, &ctx](
      const std::string& tool_name, const nlohmann::json& args) -> bool {
    // ── v0.52.1: cron 无人值守语义 ──
    // 定时任务触发危险工具时无人在场，原行为=FC 卡在暂停态干等 5 分钟
    // 被 TTL 清扫，任务静默烂掉。改为：自动拒绝 + 结果注明跳过（保守
    // 方向——宁可任务失败也不无人执行危险操作）。判据复用 cron 线程
    // 的 thread_local g_cron_filter 标记（与 skills/toolsets 过滤同源）。
    if (!g_cron_filter.is_null()) {
      log_event("hitl", LogLevel::Warn,
                "cron dangerous tool auto-denied (unattended)",
                {{"tool", tool_name}, {"session", ctx.session_id}});
      g_fc_dangerous_denied = true;  // FC 循环读到后剔除并喂回错误结果
      return false;
    }
    PendingApproval pa;
    pa.tool_name = tool_name;
    pa.tool_args = args.dump();
    pa.created_at = std::chrono::steady_clock::now();
    pa.user_text = ctx.text;
    {
      std::lock_guard<std::mutex> lk(mu_);
      fc_pending_approvals_[ctx.session_id] = std::move(pa);
    }
    // 推送审批事件给前端
    if (ctx.on_event) {
      ctx.on_event("approval_request", {
          {"tool", tool_name},
          {"args", utf8_truncate(args.dump(), 300)},
          {"session_id", ctx.session_id}});
    }
    log_event("hitl", LogLevel::Warn, "FC paused, dangerous tool",
              {{"tool", tool_name}, {"session", ctx.session_id}});
    return false;  // 暂停，等 chat_approve
  };

  // ── 简单文件操作提前判定：命中关键词时直接走 fast FC，跳过云策略 ──
  {
    const auto fast_cfg_ex = fast_path_config();
    const bool is_simple_file_op_pre = [&]() {
      bool hit = false;
      for (const auto& kw : fast_cfg_ex.file_keywords) {
        if (ctx.text_lower.find(kw) != std::string::npos) { hit = true; break; }
      }
      if (!hit) return false;
      for (const auto& kw : fast_cfg_ex.complex_exclude) {
        if (ctx.text_lower.find(kw) != std::string::npos) return false;
      }
      for (const auto& kw : fast_cfg_ex.multi_step_markers) {
        if (ctx.text_lower.find(kw) != std::string::npos) return false;
      }
      return true;
    }();

    if (is_simple_file_op_pre && use_native_fc) {
      chat_push_thinking(ctx, 2, "fast");

      DemoConfigCompat fast_cfg = cfg_;
      fast_cfg.model_name = fast_cfg_ex.model;
      fast_cfg.request_timeout_ms = fast_cfg_ex.timeout_ms;

      const std::string fast_prompt =
          (ctx.query_lang == "en") ? fast_cfg_ex.prompt_en : fast_cfg_ex.prompt_zh;

      const std::vector<ChatMessage> fast_messages = {
          ChatMessage{"system", fast_prompt},
          ChatMessage{"user", ctx.text},
      };

      auto fast_cloud = run_function_calling_loop(
          fast_cfg, api_key, fast_messages, tools_schema,
          ctx.session_id, ctx.idem_token, ctx.on_event, skill_registry_,
          2, fast_cfg_ex.max_tool_rounds, ctx.use_zh_progress,
          /*on_tool_result=*/nullptr,  // fast path: no tool cache
          [this, &ctx]() {  // v0.47.3: 流式中断
            return check_and_clear_abort(ctx.session_id);
          },
          fc_approval_cb,  // v0.51.2: fast 通道同样 HITL 审批
          /*journal_append=*/nullptr,
          ctx.on_chunk);  // v0.53.30: fast 通道流式回放

      if (fast_cloud.ok && !fast_cloud.text.empty()) {
        // v0.47.1: fast 通道 usage 累加
        if (fast_cloud.usage.total_tokens > 0) {
          total_prompt_tokens_.fetch_add(fast_cloud.usage.prompt_tokens,
                                         std::memory_order_relaxed);
          total_completion_tokens_.fetch_add(fast_cloud.usage.completion_tokens,
                                             std::memory_order_relaxed);
          total_api_calls_.fetch_add(1, std::memory_order_relaxed);
        }
        log_event("timing", LogLevel::Debug, "fc-loop-done, entering post-process");
        std::string reply_text = fast_cloud.text;
        log_event("timing", LogLevel::Debug, "reply_text preview",
                  {{"preview", utf8_safe_truncate(reply_text, 200)}});
        const std::string reply_lang = detect_query_language(reply_text);
        log_event("timing", LogLevel::Debug, "detect_lang done",
                  {{"lang", reply_lang}, {"len", reply_text.size()}});
        const std::string translated =
            apply_reply_translation_if_needed(cfg_, reply_text, ctx.query_lang, reply_lang, ctx.text);
        log_event("timing", LogLevel::Debug, "translate check done",
                  {{"translated_len", translated.size()},
                   {"same", translated == reply_text}});
        if (translated != reply_text) reply_text = translated;
        log_event("timing", LogLevel::Debug, "building return JSON");
        out.final = {
            {"type", "chat_result"},
            {"mode_used", "cloud-fast-fc"},
            {"intent_backend", "cloud-fast-function-calling"},
            {"provider", provider},
            {"model", fast_cfg_ex.model},
            {"api_base", cfg_.api_base},
            {"api_key_env", cfg_.api_key_env},
            {"api_key_state", key_state},
            {"cloud_http_status", fast_cloud.http_status},
            {"memory_size", ctx.snapshot.size()},
            {"decision", {{"route", "cloud_fast_fc"}, {"reason", "simple_file_operation"}, {"policy", "execute"}}},
            {"tool_calls", nlohmann::json::array({{{"tool", "fast_fc"}}})},
            {"text", reply_text},
        };
        // v0.53.28: fast 通道审批暂停透传（v0.51.2 fast 也走 HITL——
        // shell_exec 删除类恰命中 fast_fc，暂停帧从本块出）
        if (fast_cloud.needs_approval) {
          out.final["needs_approval"] = true;
          out.final["pending_tool_name"] = fast_cloud.pending_tool_name;
          out.final["pending_tool_args"] = fast_cloud.pending_tool_args;
          out.final["decision"] = {{"route", "fc_loop"},
                                   {"reason", "hitl_pending"},
                                   {"policy", "pause"}};
        }
        return out;
      }
      // fast hit max rounds but executed tools → summarize in one shot
      if (!fast_cloud.ok && fast_cloud.error == "max_function_calling_iterations_reached") {
        log_event("fast", LogLevel::Info, "max rounds reached, doing one-shot summary");
        if (ctx.on_event) {
          ctx.on_event("thinking", {{"tier", 2}, {"msg", progress_msg("summarizing", ctx.use_zh_progress)}});
        }
        const bool is_zh = (ctx.query_lang == "zh");
        const bool is_en = (ctx.query_lang == "en");
        std::string summary_prompt;
        if (is_zh) {
          summary_prompt = "以下是文件操作的部分结果，请用中文简洁汇总给用户。\n\n执行结果:\n" + fast_cloud.text;
        } else if (is_en) {
          summary_prompt = "Here are partial results of file operations. Summarize concisely for the user in English.\n\nResults:\n" + fast_cloud.text;
        } else {
          summary_prompt = "Here are partial results of file operations. Summarize concisely for the user, "
                           "replying in the SAME language as shown in the execution results.\n\nResults:\n" + fast_cloud.text;
        }
        std::string summary_sys;
        if (is_zh) {
          summary_sys = "用中文简洁回复。";
        } else if (is_en) {
          summary_sys = "Reply concisely in English.";
        } else {
          summary_sys = "Reply concisely in the same language as the user's request.";
        }
        const std::vector<ChatMessage> summary_msgs = {
            ChatMessage{"system", summary_sys},
            ChatMessage{"user", summary_prompt},
        };
        CurlHttpClient http(fast_cfg.request_timeout_ms);  // v0.52.3: 超时对齐配置
        auto summary = CloudLlmClient::chat_completion_with_fallback(http, fast_cfg, summary_msgs);
        if (summary.ok && !summary.text.empty()) {
          std::string reply_text = summary.text;
          out.final = {
              {"type", "chat_result"},
              {"mode_used", "cloud-fast-fc"},
              {"intent_backend", "cloud-fast-function-calling"},
              {"provider", provider},
              {"model", fast_cfg_ex.model},
              {"api_base", cfg_.api_base},
              {"api_key_env", cfg_.api_key_env},
              {"api_key_state", key_state},
              {"cloud_http_status", summary.http_status},
              {"memory_size", ctx.snapshot.size()},
              {"decision", {{"route", "cloud_fast_fc"}, {"reason", "simple_file_operation"}, {"policy", "execute"}}},
              {"tool_calls", nlohmann::json::array({{{"tool", "fast_fc"}, {"tool", "one_shot_summary"}}})},
              {"text", reply_text},
          }; return out;
        }
      }
      // fast 失败 → 继续走 main 模型
    }
  }

  // ── 构建 function calling tools schema（fast 和 main 共用）──
  // 注入可用 skills 到 system prompt
  std::string system_prompt = cloud_strategist_system_prompt();
  matched_skill_ids_.clear();  // v0.38.1: 每次请求重置匹配列表

  // 注入运行时信息（避免 LLM 对自身状态瞎猜）
  {
    std::string cwd = "unknown";
    try { cwd = std::filesystem::current_path().string(); } catch (...) {}
    system_prompt += "\n\n=== YOUR RUNTIME ENVIRONMENT (authoritative) ===";
    system_prompt += "\n- Working directory: " + cwd;
    system_prompt += "\n- Running mode: " + cfg_.mode;
    system_prompt += "\n- Provider: " + normalize_provider(cfg_.provider) + ", Model: " + cfg_.model_name;
    system_prompt += "\n=== END RUNTIME INFO ===";

    // v0.40.1: 项目上下文自动注入
    // 从当前工作目录向上查找 AGENTS.md / CLAUDE.md / .cursorrules，
    // 注入前 2000 字符到 system_prompt，让模型了解项目约定。
    {
      auto try_inject = [&](const std::string& fname) -> bool {
        std::ifstream f(fname);
        if (!f.is_open()) return false;
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        if (content.size() > 2000)
          content = utf8_truncate(content, 2000) + "\n...(truncated)";
        system_prompt += "\n\n=== PROJECT CONTEXT (" + fname + ") ===\n"
                        + content + "\n=== END PROJECT CONTEXT ===";
        return true;
      };
      // 查找优先级：AGENTS.md > CLAUDE.md > .cursorrules
      for (const auto& name : {"AGENTS.md", "CLAUDE.md", ".cursorrules"}) {
        if (try_inject(name)) break;
      }
    }
  }

  if (use_native_fc) {
    // 原生 function calling 模式：角色驱动 system prompt
    const bool is_dev = (std::getenv("THIN_AGENT_DEV_MODE") != nullptr &&
                         std::string(std::getenv("THIN_AGENT_DEV_MODE")) == "1");

    // 模式 → 主 agent 角色名
    std::string default_role_name = is_dev ? "developer" : "worker";
    const agent::AgentRole* default_role = role_mgr_ ? role_mgr_->find(default_role_name) : nullptr;

    if (default_role && !default_role->system_prompt.empty()) {
      system_prompt = default_role->system_prompt;
    } else {
      // 兜底（不应该走到这里）
      if (ctx.use_zh_progress)
        system_prompt = "你是 thin_agent，用中文回复，简洁专业。";
      else
        system_prompt = "You are thin_agent. Reply in the same language as the user. Be concise and professional.";
    }

    // 注入当前工作流（dev 模式下替换默认构建步骤）
    if (is_dev) {
      auto wf = WorkflowManager::instance().current();
      system_prompt = WorkflowManager::instance().inject_into_system_prompt(system_prompt, wf);
    }

    // 注入技能信息
    std::string skill_info = skill_registry_.build_skill_prompt_text();

    // v0.27.5: 注入持久记忆冷冻快照（热路径：system/* + user/* + role/<current_role>/*）
    if (fact_store_) {
      std::string current_role = default_role ? default_role->name : "";
      std::string mem_block = fact_store_->hot_snapshot(current_role, 4000);
      if (!mem_block.empty()) {
        system_prompt += "\n\n" + mem_block;
      }
    }
    // v0.29.0: 自动 KB 注入 — 从用户输入提取关键词查询知识库
    if (kb_searcher_ && !ctx.text.empty()) {
      auto kb_results = kb_searcher_->search(ctx.text, 3);
      if (kb_results.is_array() && !kb_results.empty()) {
        std::string kb_context;
        for (const auto& r : kb_results) {
          std::string key = r.value("key", r.value("title", ""));
          std::string val = r.value("content", r.value("snippet", r.dump()));
          kb_context += "- " + key + ": " + utf8_truncate(val, 300) + "\n";
        }
        if (!kb_context.empty()) {
          system_prompt += "\n\n=== KNOWLEDGE BASE CONTEXT ===\n"
                          + kb_context + "=== END KB CONTEXT ===";
        }
      }
    }
    // v0.38.0: 语义记忆自动注入 — 用当前用户输入召回相关历史记忆
    if (memory_manager_ && !ctx.text.empty()) {
      std::string mem_ctx = memory_manager_->build_context(ctx.text, 3);
      if (!mem_ctx.empty()) {
        system_prompt += "\n\n" + mem_ctx;
      }
    }
    // v0.38.1: Skill 自进化注入 — 语义匹配历史成功技能
    if (skill_manager_ && !ctx.text.empty()) {
      auto matched = skill_manager_->match_skills(ctx.text, 3, 0.3f);
      if (!matched.empty()) {
        std::string skill_inj = agent::SkillManager::to_prompt_injection(matched);
        if (!skill_inj.empty()) {
          system_prompt += "\n\n" + skill_inj;
          // 记住匹配到的技能 ID，任务成功后 increment_use
          for (const auto& s : matched) matched_skill_ids_.push_back(s.id);
        }
      }
    }
    // v0.38.2: 注入 GoalManager + ErrorCorrectionStore（对齐子 Agent 路径）
    if (goal_mgr_) {
      std::string goal_ctx = goal_mgr_->to_prompt_injection();
      if (!goal_ctx.empty()) system_prompt += "\n\n" + goal_ctx;
    }
    if (correction_store_) {
      std::vector<std::string> known_tools;
      for (const auto& t : agent::ToolRegistry::instance().all_tools())
        known_tools.push_back(t.name);
      std::string corr_ctx = correction_store_->to_prompt_injection(known_tools);
      if (!corr_ctx.empty()) system_prompt += "\n\n" + corr_ctx;
    }
    if (!skill_info.empty()) {
      system_prompt += "\n\nAVAILABLE SKILLS:\n" + skill_info;
    }
  } else {
    // 传统 pipeline JSON 模式：注入文本 skills
    std::string skill_info = skill_registry_.build_skill_prompt_text();
    if (!skill_info.empty()) {
      system_prompt += "\n\nAVAILABLE SKILLS (you may plan multi-step tasks):\n" + skill_info;
      system_prompt += "\nTo plan a multi-step task, include a \"task_pipeline\" array: "
                       "{\"action\":\"tool_name\",\"params\":{...}, optional \"output\":\"var_name\"}. "
                       "Reference upstream results as $var_name[N].field.";
    }
  }

  // ── ③ 项目结构注入（dev 模式 + 已扫描）──
  if (!project_structure_.empty()) {
    system_prompt += "\n\n=== PROJECT STRUCTURE ===\n" + project_structure_ + "=== END PROJECT STRUCTURE ===\n";
  }

  // ── 构建 messages：标准格式，历史作为独立 user/assistant 对 ──
  std::vector<ChatMessage> messages;
  messages.push_back(ChatMessage{"system", system_prompt});

  // v0.29.0: 注入对话摘要（ConversationSummarizer 自动压缩后的缓存）
  if (summarizer_) {
    std::string summary_prefix = summarizer_->summary_prefix();
    if (!summary_prefix.empty()) {
      messages.push_back(ChatMessage{"system", summary_prefix});
    }
  }

  // ① 会话历史（最近的 user/assistant 对，不包括当前消息）
  // ctx.snapshot 的最后一条是当前用户消息，前面的才是历史
  for (size_t i = 0; i + 1 < ctx.snapshot.size(); ++i) {
    const auto& m = ctx.snapshot[i];
    if (m.role == "user" || m.role == "assistant") {
      messages.push_back(m);
    }
  }

  // ② 工具结果缓存（最近 K 次调用）
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!tool_result_cache_.empty()) {
      std::string tool_ctx = "Recent tool results:\n";
      for (const auto& e : tool_result_cache_) {
        tool_ctx += "[" + e.action + "] " + e.params_summary + ": " + e.result_truncated + "\n";
      }
      messages.push_back(ChatMessage{"user", tool_ctx});
    }
  }

  // 当前用户消息
  {
    ChatMessage umsg{"user", ctx.text};
    if (!ctx.image_base64.empty()) {
      umsg.image_base64 = ctx.image_base64;
    }
    messages.push_back(umsg);
  }

  chat_push_thinking(ctx, 3, "deep_reasoning");

  CloudChatResult cloud;

  // v0.47.5: 缓存查询（FC 和非 FC 路径统一，跳过已缓存的纯文本回复）
  // 只缓存不含工具调用的简单问答。带 chat_memory 的多轮对话不查缓存
  // （ctx.snapshot > 1 表示有历史消息 = 多轮上下文，结果会随历史变化）。
  std::string cache_key;
  if (ctx.snapshot.size() <= 1) {
    cache_key = make_cache_key(cfg_.model_name, ctx.text);
    std::string cached = response_cache_->get(cache_key);
    if (!cached.empty()) {
      cloud.ok = true;
      cloud.text = cached;
      cloud.http_status = 200;
      log_event("cache", LogLevel::Info, "HIT",
                {{"key", cache_key.substr(0, 20)}});
      // 缓存命中时直接跳到回复构建（跳过 FC/非FC 分支）
      goto cloud_done;
    }
  }

  if (use_native_fc) {
    auto tool_cache_cb = [this](const std::string& action, const std::string& params_summary, const std::string& output_truncated) {
      std::lock_guard<std::mutex> lk(mu_);
      tool_result_cache_.push_back({action, params_summary, output_truncated, std::chrono::steady_clock::now()});
      while (tool_result_cache_.size() > static_cast<size_t>(ctx_cfg_.tool_cache_size))
        tool_result_cache_.pop_front();
    };
    // ── 自适应迭代预算：dev 模式对齐 Claude Code 动态范围 25-100+ ──
    const bool fc_is_dev = (std::getenv("THIN_AGENT_DEV_MODE") != nullptr &&
                             std::string(std::getenv("THIN_AGENT_DEV_MODE")) == "1");
    int adaptive_iterations = fc_is_dev ? 10 : 5;  // dev 基线 10，标准 5
    if (ctx.complex_intent_detected) adaptive_iterations += 5;
    if (ctx.text.size() > 100) adaptive_iterations += 5;   // 长描述通常是多步任务
    if (tools_schema.is_array() && tools_schema.size() > 5) adaptive_iterations += 5;  // 编程环境
    int hard_cap = fc_is_dev ? 50 : 25;
    if (adaptive_iterations > hard_cap) adaptive_iterations = hard_cap;

    // v0.29.0: 自动 checkpoint — 复杂任务前保存快照，支持回滚
    if (checkpoint_mgr_ && adaptive_iterations > 5 && !ctx.session_id.empty()) {
      nlohmann::json snap;
      snap["messages"] = nlohmann::json::array();
      for (const auto& m : messages) {
        nlohmann::json jm;
        jm["role"] = m.role;
        jm["content"] = m.content;
        snap["messages"].push_back(jm);
      }
      snap["iterations"] = adaptive_iterations;
      // save() 第二参数是 chat_memory (vector<string>)，snap 作为 extra 传入
      std::string ckpt = checkpoint_mgr_->save(ctx.session_id, {}, snap, "auto_fc_pre");
      log_event("checkpoint", LogLevel::Info, "auto-saved pre-FC",
                {{"id", ckpt}});
    }

    // v0.31.0: 文件系统级 checkpoint — FC 前自动保存，支持代码变更回滚
    if (fs_checkpoint_mgr_ && fc_is_dev && !ctx.session_id.empty()) {
      fs_checkpoint_mgr_->reset_turn();
      fs_checkpoint_mgr_->mark_dirty();
      std::string fs_ckpt = fs_checkpoint_mgr_->auto_save("/root/code", "auto_fc_pre");
      if (!fs_ckpt.empty()) {
        log_event("fs-checkpoint", LogLevel::Info, "auto-saved pre-FC",
                  {{"id", fs_ckpt}});
      }
    }

    // v0.49.0: HITL 审批回调 — 已提升至 fast 分支前定义（v0.51.2 两通道共用）

    // v0.52.26: 断点续跑——run 创建 + journal 绑定（工具边界增量落盘）
    std::string fc_run_id;
    if (fc_run_store_) {
      fc_run_id = "fc_" + std::to_string(std::chrono::duration_cast<
          std::chrono::milliseconds>(std::chrono::system_clock::now()
              .time_since_epoch()).count()) + "_" + ctx.session_id;
      FcRun jr;
      jr.run_id = fc_run_id;
      jr.session_id = ctx.session_id;
      jr.user_text = ctx.effective_text;
      jr.status = "running";
      jr.messages = nlohmann::json::array();  // 初始消息不重复存（resume
      // 时由会话历史重建 system+user 前缀，journal 只存增量轮次）
      fc_run_store_->create_run(jr);
      fc_active_runs_[ctx.session_id] = fc_run_id;
    }
    auto fc_journal_cb = [this, &ctx](int iter, const nlohmann::json& msgs) {
      auto it = fc_active_runs_.find(ctx.session_id);
      if (it != fc_active_runs_.end() && fc_run_store_)
        fc_run_store_->append_messages(it->second, iter, msgs);
    };
    cloud = run_function_calling_loop(cfg_, api_key, messages, tools_schema,
                                      ctx.session_id, ctx.idem_token, ctx.on_event, skill_registry_,
                                      3, adaptive_iterations, ctx.use_zh_progress, tool_cache_cb,
                                      [this, &ctx]() {  // v0.47.3: 流式中断
                                        return check_and_clear_abort(ctx.session_id);
                                      },
                                      fc_approval_cb,  // v0.49.0: HITL 审批
                                      fc_journal_cb,  // v0.52.26: journal
                                      ctx.on_chunk);  // v0.53.30: main FC 流式回放
  } else {
    CurlHttpClient http(cfg_.request_timeout_ms);  // v0.52.3: 超时对齐配置
    cloud = CloudLlmClient::chat_completion_with_fallback(
        http, cfg_, messages, "THIN_AGENT_TEST_CLOUD_RESPONSE", {{"type", "json_object"}});
  }

cloud_done:
  // v0.52.2: 方案 C — FC 暂停响应携带的消息快照回填 pending approval
  //（回调创建 pa 时拿不到 messages；快照在 FC 循环暂停分支生成，
  // 此处是主路径统一的暂停响应出口）
  if (cloud.needs_approval && !cloud.fc_messages_snapshot.empty()) {
    std::lock_guard<std::mutex> lk(mu_);
    auto pit = fc_pending_approvals_.find(ctx.session_id);
    if (pit != fc_pending_approvals_.end()) {
      pit->second.fc_messages = cloud.fc_messages_snapshot;
      pit->second.fc_iterations_used = cloud.fc_iterations_used;
      // v0.52.26: 暂停=可续跑点——审批连带 run 快照落盘（重启后
      // take_approval 恢复推送；批准/拒绝走既有内存续跑路径不变）
      if (fc_run_store_) {
        auto rit = fc_active_runs_.find(ctx.session_id);
        if (rit != fc_active_runs_.end()) {
          FcPendingApproval jpa;
          jpa.run_id = rit->second;
          jpa.session_id = ctx.session_id;
          jpa.tool_name = pit->second.tool_name;
          jpa.tool_args = pit->second.tool_args;
          jpa.fc_iterations_used = cloud.fc_iterations_used;
          jpa.user_text = pit->second.user_text;
          fc_run_store_->save_approval(jpa);
        }
      }
    }
  }
  // v0.52.26: run 终态——needs_approval=暂停（保持 running，等待审批/
  // 断点续跑），否则按结果收敛 done/failed
  if (fc_run_store_ && !cloud.needs_approval) {
    std::lock_guard<std::mutex> lk(mu_);
    finalize_fc_run_locked(ctx.session_id, cloud.ok ? "done" : "failed");
  }
  // v0.47.5: 缓存写入 — 仅当无工具调用（纯文本回复）且首次未命中缓存时
  // v0.51.2: HITL 审批暂停的响应禁止缓存——审批提示被缓存后，同话术重发
  // 会直接命中缓存返回"⏸️"文本，但 fc_pending_approvals_ 无记录，
  // chat_approve 断路（实测：fv1 暂停被缓存 → fv2 命中 → 审批不可达）
  if (!cache_key.empty() && cloud.ok && !cloud.text.empty() &&
      cloud.tool_names_used.empty() && !cloud.needs_approval) {
    response_cache_->put(cache_key, cloud.text);
    log_event("cache", LogLevel::Debug, "MISS stored",
              {{"key", cache_key.substr(0, 20)}});
  }

  // v0.47.1: 累加 token 用量到全局原子计数器（FC 和非 FC 路径统一处理）
  // FC 路径：cloud.usage 由 run_function_calling_loop 内部逐轮累积
  // 非FC路径：cloud.usage 由 parse_response 直接解析
  if (cloud.usage.total_tokens > 0) {
    total_prompt_tokens_.fetch_add(cloud.usage.prompt_tokens,
                                   std::memory_order_relaxed);
    total_completion_tokens_.fetch_add(cloud.usage.completion_tokens,
                                       std::memory_order_relaxed);
    total_api_calls_.fetch_add(1, std::memory_order_relaxed);
  }

  if (!cloud.ok) {
    // 云失败 → 调用方离线回退（携带错误详情供 cloud_error 字段）
    out.need_fallback = true;
    out.cloud = cloud;
    return out;
  }

  // ── 原生 function calling 返回直接答案 ──
  if (use_native_fc) {
    // v0.51.4: 空回复守卫——上游 4xx/5xx 错误体（如坏 key 的 401 JSON）
    // 被 parse_response 解析为 ok=true + text="" 时，不能当成功直接返回
    // （实测坏 key 场景 44s 后返回空文本伪装成功，fallback=offline 未触发）。
    // fast 分支早有 !text.empty() 守卫，主分支补齐——同类不一致修复。
    if (cloud.text.empty()) {
      log_event("fc-loop", LogLevel::Warn, "empty cloud text, falling back",
                {{"http_status", cloud.http_status},
                 {"error", cloud.error.empty() ? "empty_response_body" : cloud.error}});
      out.need_fallback = true;
      out.cloud = cloud;
      return out;
    }
    std::string reply_text = cloud.text;
    const std::string reply_lang = detect_query_language(reply_text);
    const std::string translated =
        apply_reply_translation_if_needed(cfg_, reply_text, ctx.query_lang, reply_lang, ctx.text);
    if (translated != reply_text) reply_text = translated;

    // v0.38.1: Skill 自进化 — 成功完成复杂任务后自动提取技能
    if (skill_manager_ && cloud.ok && cloud.tool_names_used.size() >= 3) {
      // name: 用户输入前 30 字
      std::string skill_name = utf8_truncate(ctx.text, 30);
      if (ctx.text.size() > 30) skill_name += "...";

      // description: 工具调用序列
      std::string skill_desc;
      for (size_t i = 0; i < cloud.tool_names_used.size(); ++i) {
        if (i > 0) skill_desc += " → ";
        skill_desc += cloud.tool_names_used[i];
      }

      // prompt: 工具序列 + 回复摘要（≤500 字）
      std::string skill_prompt = "Tools: " + skill_desc +
          "\nResult: " + utf8_truncate(reply_text, 500);

      // tags: 去重的工具名列表
      std::vector<std::string> skill_tags;
      for (const auto& tn : cloud.tool_names_used) {
        bool dup = false;
        for (const auto& e : skill_tags) { if (e == tn) { dup = true; break; } }
        if (!dup) skill_tags.push_back(tn);
      }

      std::string skill_id = skill_manager_->save_skill(
          ctx.session_id, skill_name, skill_desc, skill_prompt, skill_tags);
      log_event("skill", LogLevel::Info, "auto-saved",
                {{"id", skill_id}, {"desc", skill_desc}});
    }

    // v0.38.1: Step 3 — 匹配到的旧技能 increment_use（高频技能排名靠前）
    for (const auto& sid : matched_skill_ids_) {
      skill_manager_->increment_use(sid);
    }

    out.final = {
        {"type", "chat_result"},
        {"mode_used", "cloud-fc"},
        {"intent_backend", "cloud-function-calling"},
        {"provider", provider},
        {"model", cfg_.model_name},
        {"api_base", cfg_.api_base},
        {"api_key_env", cfg_.api_key_env},
        {"api_key_state", key_state},
        {"cloud_http_status", cloud.http_status},
        {"memory_size", ctx.snapshot.size()},
        {"decision", {{"route", "cloud_fc_direct"}, {"reason", "function_calling_native"}, {"policy", "execute"}}},
        {"tool_calls", nlohmann::json::array({{{"tool", "cloud_fc"}}})},
        {"text", reply_text},
    };
    // v0.53.28: FC 审批暂停——结构化字段透传（客户端审批卡依赖；
    // 此前仅 text 带"⏸️"文案，needs_approval/pending_tool_name 丢失，
    // 客户端无法渲染审批卡、无法结构化判断暂停态）
    if (cloud.needs_approval) {
      out.final["needs_approval"] = true;
      out.final["pending_tool_name"] = cloud.pending_tool_name;
      out.final["pending_tool_args"] = cloud.pending_tool_args;
      out.final["decision"] = {{"route", "fc_loop"},
                               {"reason", "hitl_pending"},
                               {"policy", "pause"}};
    }
    return out;
  }

  // ── 成功但未直接作答 → 传统 pipeline JSON 路径继续（调用方处理）──
  out.continue_pipeline = true;
  out.cloud = cloud;
  return out;
}

/// v0.50.0: 云失败离线回退域（原 handle_chat 段 I / cloud_fallback 标签）。
/// 云调用失败时：fallback=offline → 本地模型级联回退；否则返回 cloud-error。
nlohmann::json AgentService::chat_offline_fallback(ChatContext& ctx,
                                                   const CloudChatResult& cloud,
                                                   const std::string& key_state,
                                                   const std::string& provider) {
  if (cfg_.fallback == "offline") {
    // 云调用失败 → 尝试本地模型级联，不再只回显模板
    std::string fallback_text;
    std::string fallback_model;
    thin_agent::local::HybridRouter local_router;
    auto local_result = local_router.route("chat", ctx.text, 256);
    if (local_result.ok && !local_result.output.empty()) {
      fallback_text = local_result.output;
      fallback_model = local_result.model_used;
    } else {
      fallback_text = render_template(
          policy_text("cloud.fallback_call_failed",
                      policy_text("cloud.call_failed_fallback",
                                  "[offline-fallback] 云调用失败，已回退离线模式。输入回显：{ctx.text}")),
          {{"text", ctx.text}});
      fallback_model = "template_fallback";
    }
    return {
        {"type", "chat_result"},
        {"mode_used", local_result.ok ? "local-agent" : "offline-fallback"},
        {"intent_backend", "cloud"},
        {"provider", provider},
        {"model", local_result.ok ? fallback_model : cfg_.model_name},
        {"api_base", cfg_.api_base},
        {"api_key_env", cfg_.api_key_env},
        {"api_key_state", key_state},
        {"fallback_reason", "cloud_call_failed"},
        {"cloud_http_status", cloud.http_status},
        {"cloud_error", cloud.error},
        {"memory_size", ctx.snapshot.size()},
        {"decision", {{"route", local_result.ok ? fallback_model : "cloud_fallback"},
                      {"reason", "cloud_call_failed"},
                      {"policy", "fallback_cloud"}}},
        {"tool_calls", nlohmann::json::array({{{"tool", "cloud_llm"}, {"provider", provider}, {"model", cfg_.model_name}}})},
        {"decision_trace", nlohmann::json::array({
            {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", "general_query"}, {"confidence", 0.40}}}},
            {{"layer", "policy"}, {"input", nlohmann::json{{"reason", "cloud_call_failed"}, {"risk", "unknown"}, {"confidence", 0.0}, {"cloud_http_status", cloud.http_status}, {"cloud_error", cloud.error}, {"route", local_result.ok ? fallback_model : "cloud_fallback"}, {"policy", "fallback_cloud"}}}, {"output", nlohmann::json{{"route", local_result.ok ? fallback_model : "cloud_fallback"}, {"policy", "fallback_cloud"}}}},
            {{"layer", "llm"}, {"input", "cloud_chat"}, {"output", local_result.ok ? fallback_model : "error"}}
        })},
        {"text", fallback_text},
    };
  }

  return {
      {"type", "chat_result"},
      {"mode_used", "cloud-error"},
      {"intent_backend", "cloud"},
      {"provider", provider},
      {"model", cfg_.model_name},
      {"api_base", cfg_.api_base},
      {"api_key_env", cfg_.api_key_env},
      {"api_key_state", key_state},
      {"cloud_http_status", cloud.http_status},
      {"cloud_error", cloud.error},
      {"memory_size", ctx.snapshot.size()},
      {"decision", {{"route", "cloud_error"}, {"reason", "cloud_call_failed_no_fallback"}, {"policy", "reject"}}},
      {"tool_calls", nlohmann::json::array({{{"tool", "cloud_llm"}, {"provider", provider}, {"model", cfg_.model_name}}})},
      {"decision_trace", nlohmann::json::array({
          {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", "general_query"}, {"confidence", 0.40}}}},
          {{"layer", "policy"}, {"input", nlohmann::json{{"reason", "cloud_call_failed_no_fallback"}, {"risk", "unknown"}, {"confidence", 0.0}, {"cloud_http_status", cloud.http_status}, {"cloud_error", cloud.error}, {"route", "cloud_error"}, {"policy", "reject"}}}, {"output", nlohmann::json{{"route", "cloud_error"}, {"policy", "reject"}}}},
          {{"layer", "llm"}, {"input", "cloud_chat"}, {"output", "error"}}
      })},
      {"text", policy_text("cloud.error_call_failed_no_fallback",
                           policy_text("cloud.call_failed_no_fallback",
                                       "[cloud-error] 云调用失败且未允许离线回退。"))},
  };
}

/// v0.50.0: 云策略 pipeline 路由域（原 handle_chat 段 H）。
/// 云端返回的 strategy JSON 处理：reject/clarify 降级、task pipeline 执行
/// （含回滚）、本地 route_hint 落地（profile/status/task_inline）、
/// answer_direct 兜底。命中终态返回 final，未命中返回 null。
nlohmann::json AgentService::chat_route_cloud_strategy(ChatContext& ctx,
                                                        const CloudChatResult& cloud,
                                                        const std::string& key_state,
                                                        const std::string& provider) {
  const bool general_info_query_like =
      contains_any_policy(ctx.text_lower, "general_info_query_include") &&
      !contains_any_policy(ctx.text_lower, "general_info_query_exclude");
  // ── 传统 pipeline JSON 路径 ──
  if (cloud.ok) {
    const auto cloud_policy = parse_cloud_strategy_json(cloud.text, ctx.intent_info.value("intent", "general_query"));
    const std::string strategy = cloud_policy.value("strategy", "answer_direct");
    const std::string local_route_hint =
        normalize_cloud_route_hint(cloud_policy.value("local_route_hint", std::string()));
    const bool need_clarify = cloud_policy.value("need_clarify", false);
    const std::string clarify_question = cloud_policy.value("clarify_question", "");
    const std::string cloud_intent = cloud_policy.value("intent", "general_query");
    const double cloud_confidence = cloud_policy.value("confidence", 0.40);
    const std::string cloud_risk = cloud_policy.value("risk", "medium");

    auto cloud_tool_calls = nlohmann::json::array({{{"tool", "cloud_llm"}, {"provider", provider}, {"model", cfg_.model_name}}});

    const std::unordered_set<std::string> allowed_strategy = {"answer_direct", "clarify", "reject"};
    const std::unordered_set<std::string> allowed_risk = {"none", "low", "medium", "high"};
    const auto& allowed_route_hint = cloud_allowed_route_hints();

    // 从 skill_registry 动态构建允许的 pipeline action 列表（合并内置 media actions）
    std::unordered_set<std::string> allowed_pipeline_actions = {
        "capture_photo",
        "start_recording",
        "stop_recording",
    };
    // 从 skills 中加载声明的 action
    for (const auto* ad : skill_registry_.get_enabled_actions()) {
      allowed_pipeline_actions.insert(ad->name);
    }
    // 同时纳入通过 register_cpp_handler 注册的 action（如 write_file）
    for (const auto& [name, ad] : skill_registry_.action_map()) {
      allowed_pipeline_actions.insert(name);
    }

    nlohmann::json contract_errors = nlohmann::json::array();
    nlohmann::json pipeline_errors = nlohmann::json::array();
    bool pipeline_valid = true;
    const auto task_pipeline = cloud_policy.value("task_pipeline", nlohmann::json::array());
    if (!task_pipeline.is_array()) {
      pipeline_valid = false;
      pipeline_errors.push_back("task_pipeline_not_array");
    } else {
      for (size_t i = 0; i < task_pipeline.size(); ++i) {
        const auto& step = task_pipeline[i];
        if (!step.is_object()) {
          pipeline_valid = false;
          pipeline_errors.push_back("step_not_object:" + std::to_string(i));
          continue;
        }
        if (!step.contains("action") || !step["action"].is_string()) {
          pipeline_valid = false;
          pipeline_errors.push_back("missing_action:" + std::to_string(i));
          continue;
        }
        const std::string action = step.value("action", "");
        if (allowed_pipeline_actions.find(action) == allowed_pipeline_actions.end()) {
          pipeline_valid = false;
          pipeline_errors.push_back("invalid_action:" + action);
        }
        if (step.contains("params") && !step["params"].is_object()) {
          pipeline_valid = false;
          pipeline_errors.push_back("params_not_object:" + std::to_string(i));
        }
      }
      // v0.54.32: 禁止 media 与非 media 混管线——混跑会进 skill pipeline，
      // capture_* 仅 stub 假成功；纯媒体走 TaskEngine，纯技能走 skill pipeline。
      if (pipeline_valid && task_pipeline.is_array() && !task_pipeline.empty()) {
        static const std::unordered_set<std::string> media_only = {
            "capture_photo", "start_recording", "stop_recording", "fetch_capture_results"};
        bool has_media = false;
        bool has_non_media = false;
        for (const auto& step : task_pipeline) {
          if (!step.is_object()) continue;
          const std::string a = step.value("action", "");
          if (a.empty()) continue;
          if (media_only.count(a)) has_media = true;
          else has_non_media = true;
        }
        if (has_media && has_non_media) {
          pipeline_valid = false;
          pipeline_errors.push_back("mixed_media_and_skill_pipeline");
        }
      }
    }

    nlohmann::json pipeline_contract_obs = {
        {"valid", pipeline_valid},
        {"step_count", task_pipeline.is_array() ? task_pipeline.size() : 0},
        {"errors", pipeline_errors},
    };
    if (allowed_strategy.find(strategy) == allowed_strategy.end()) {
      contract_errors.push_back("invalid_strategy");
    }
    if (allowed_risk.find(cloud_risk) == allowed_risk.end()) {
      contract_errors.push_back("invalid_risk");
    }
    if (allowed_route_hint.find(local_route_hint) == allowed_route_hint.end()) {
      // 如果有有效 pipeline，local_route_hint 可为任意值（pipeline 本身就是路由）
      if (!pipeline_valid || task_pipeline.empty()) {
        contract_errors.push_back("invalid_local_route_hint");
      }
    }
    if (cloud_confidence < 0.0 || cloud_confidence > 1.0) {
      contract_errors.push_back("invalid_confidence_range");
    }
    if (!pipeline_valid) {
      contract_errors.push_back("invalid_task_pipeline");
    }

    if (!contract_errors.empty() &&
        cloud_policy.value("parser_mode", "") == "fallback" &&
        strategy == "answer_direct" &&
        cloud_policy.contains("response_draft") && cloud_policy["response_draft"].is_string() &&
        !cloud_policy["response_draft"].get<std::string>().empty()) {
      // v0.54.33: fallback 只放行「软字段」错误（strategy/risk/hint/confidence），
      // **绝不**清空 invalid_task_pipeline（含 mixed_media / 未知 action）。
      nlohmann::json soft_only = nlohmann::json::array();
      for (const auto& e : contract_errors) {
        if (!e.is_string()) {
          soft_only.push_back(e);
          continue;
        }
        const std::string es = e.get<std::string>();
        if (es == "invalid_task_pipeline" || es.find("pipeline") != std::string::npos ||
            es.find("mixed_media") != std::string::npos || es.find("invalid_action") != std::string::npos) {
          soft_only.push_back(e);
        }
      }
      // soft_only 非空 = 仍有硬错误 → 保留；否则整表清空（原 fallback 语义）
      contract_errors = soft_only;
    }

    if (!contract_errors.empty()) {
      fprintf(stderr, "[agent] cloud contract failed: %s hint=%s strategy=%s pipeline_valid=%d\n",
              contract_errors.dump().c_str(), local_route_hint.c_str(),
              strategy.c_str(), pipeline_valid ? 1 : 0);
      return make_local_reply(ctx, 
          "local_clarify",
          "cloud_policy_contract_violation",
          policy_text("cloud.policy_contract_violation", ""),
          {{"cloud_policy", cloud_policy},
           {"cloud_policy_contract", {{"valid", false}, {"errors", contract_errors}, {"strategy", strategy}, {"risk", cloud_risk}, {"local_route_hint", local_route_hint}, {"confidence", cloud_confidence}}},
           {"cloud_task_pipeline_contract", pipeline_contract_obs}},
          cloud_tool_calls,
          "general",
          0.50,
          nlohmann::json::object(),
          "clarify");
    }

    if (strategy == "reject" || cloud_risk == "high") {
      const std::string reject_reason = cloud_policy.value("reason", "cloud_strategy_reject");
      const bool downgrade_to_clarify = general_info_query_like && cloud_intent == "general_query";
      const std::string safer = downgrade_to_clarify
          ? policy_text("cloud.reject_downgrade_clarify", "")
          : policy_text("cloud.reject", "");
      return {
          {"type", "chat_result"},
          {"mode_used", "local-agent"},
          {"intent_backend", "cloud-strategy"},
          {"provider", provider},
          {"model", cfg_.model_name},
          {"api_base", cfg_.api_base},
          {"api_key_env", cfg_.api_key_env},
          {"api_key_state", key_state},
          {"cloud_http_status", cloud.http_status},
          {"memory_size", ctx.snapshot.size()},
          {"decision", {{"route", downgrade_to_clarify ? "local_clarify" : "local_reject"}, {"reason", reject_reason}, {"policy", downgrade_to_clarify ? "clarify" : "reject"}, {"intent", cloud_intent}, {"confidence", cloud_confidence}}},
          {"tool_calls", cloud_tool_calls},
          {"observation", {{"cloud_policy", cloud_policy}, {"risk_gate", {{"risk", cloud_risk}, {"blocked", !downgrade_to_clarify}}}, {"reject_downgraded", downgrade_to_clarify}, {"cloud_complex_intent_gate", {{"enabled", cfg_.complex_intent_force_cloud}, {"detected", ctx.complex_intent_detected}, {"forced", ctx.force_cloud_for_complex}}}}},
          {"decision_trace", nlohmann::json::array({
              {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", ctx.intent_info.value("intent", "unknown")}, {"confidence", ctx.intent_info.value("confidence", 0.0)}}}},
              {{"layer", "llm"}, {"input", "cloud_strategy"}, {"output", cloud_policy}},
              {{"layer", "policy"}, {"input", nlohmann::json{{"risk", cloud_risk}, {"strategy", strategy}, {"reason", reject_reason}, {"general_info_query_like", general_info_query_like}, {"route", downgrade_to_clarify ? "local_clarify" : "local_reject"}, {"policy", downgrade_to_clarify ? "clarify" : "reject"}}}, {"output", nlohmann::json{{"route", downgrade_to_clarify ? "local_clarify" : "local_reject"}, {"policy", downgrade_to_clarify ? "clarify" : "reject"}}}}
          })},
          {"text", safer},
      };
    }

    if ((need_clarify || strategy == "clarify") && !is_open_knowledge_query(ctx.text_lower)) {
      std::string ask = pick_cloud_visible_reply(cloud_policy);
      if (ask.empty()) {
        ask = clarify_question.empty()
            ? policy_text("cloud.clarify_default_question", "")
            : clarify_question;
      }
      const std::string style_name = policy_style_default("normal");
      const std::string clarify_prefix = policy_style_text(
          style_name,
          "cloud.clarify_prefix",
          policy_text("cloud.clarify_prefix", ""));
      return {
          {"type", "chat_result"},
          {"mode_used", "local-agent"},
          {"intent_backend", "cloud-strategy"},
          {"provider", provider},
          {"model", cfg_.model_name},
          {"api_base", cfg_.api_base},
          {"api_key_env", cfg_.api_key_env},
          {"api_key_state", key_state},
          {"cloud_http_status", cloud.http_status},
          {"memory_size", ctx.snapshot.size()},
          {"decision", {{"route", "local_clarify"}, {"reason", "cloud_strategy_requests_clarify"}, {"policy", "clarify"}, {"intent", cloud_intent}, {"confidence", cloud_confidence}}},
          {"tool_calls", cloud_tool_calls},
          {"observation", {{"cloud_policy", cloud_policy}, {"cloud_task_pipeline_contract", pipeline_contract_obs}, {"cloud_complex_intent_gate", {{"enabled", cfg_.complex_intent_force_cloud}, {"detected", ctx.complex_intent_detected}, {"forced", ctx.force_cloud_for_complex}}}}},
          {"decision_trace", nlohmann::json::array({
              {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", ctx.intent_info.value("intent", "unknown")}, {"confidence", ctx.intent_info.value("confidence", 0.0)}}}},
              {{"layer", "llm"}, {"input", "cloud_strategy"}, {"output", cloud_policy}},
              {{"layer", "policy"}, {"input", nlohmann::json{{"risk", cloud_risk}, {"strategy", strategy}, {"route", "local_clarify"}, {"policy", "clarify"}}}, {"output", nlohmann::json{{"route", "local_clarify"}, {"policy", "clarify"}}}}
          })},
          {"text", clarify_prefix + ask},
      };
    }

    if (task_pipeline.is_array() && !task_pipeline.empty()) {
      // 非媒体 action → 通用 Skill Pipeline 执行 + 端云协作汇总
      std::string first_action = task_pipeline[0].is_object() ? task_pipeline[0].value("action", "") : "";
      static const std::unordered_set<std::string> media_actions = {"capture_photo", "start_recording", "stop_recording", "fetch_capture_results"};
      if (media_actions.find(first_action) == media_actions.end()) {
        auto exec_result = execute_skill_pipeline(task_pipeline, ctx.session_id, ctx.idem_token, ctx.on_event);
        std::string reply_text = summarize_pipeline_result(exec_result, ctx.text, ctx.idem_token, ctx.on_event, ctx.use_zh_progress);
        return {
            {"type", "chat_result"},
            {"mode_used", "local-agent"},
            {"intent_backend", "cloud-strategy"},
            {"provider", provider},
            {"model", cfg_.model_name},
            {"api_base", cfg_.api_base},
            {"api_key_env", cfg_.api_key_env},
            {"api_key_state", key_state},
            {"cloud_http_status", cloud.http_status},
            {"memory_size", ctx.snapshot.size()},
            {"decision", {{"route", "local_task_pipeline"}, {"reason", "skill_pipeline_executed"}, {"policy", "execute"}, {"intent", cloud_intent}, {"confidence", cloud_confidence}}},
            {"tool_calls", cloud_tool_calls},
            {"observation", {{"cloud_policy", cloud_policy}, {"pipeline_execution", exec_result}, {"cloud_task_pipeline_contract", pipeline_contract_obs}}},
            {"decision_trace", nlohmann::json::array({
                {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", ctx.intent_info.value("intent", "unknown")}, {"confidence", ctx.intent_info.value("confidence", 0.0)}}}},
                {{"layer", "llm"}, {"input", "cloud_strategy"}, {"output", cloud_policy}},
                {{"layer", "policy"}, {"input", {{"route", "local_task_pipeline"}, {"policy", "execute"}}}, {"output", {{"route", "local_task_pipeline"}, {"policy", "execute"}}}}
            })},
            {"text", reply_text},
        };
      }

      if (!task_engine_) {
        const std::string no_engine_error_class = "engine";
        const std::string no_engine_error_code = pipeline_error_code_from_class(no_engine_error_class);
        const auto no_engine_outcome = pipeline_outcome_object(true, no_engine_error_class, -1);
        return {
            {"type", "chat_result"},
            {"mode_used", "local-agent"},
            {"intent_backend", "cloud-strategy"},
            {"provider", provider},
            {"model", cfg_.model_name},
            {"api_base", cfg_.api_base},
            {"api_key_env", cfg_.api_key_env},
            {"api_key_state", key_state},
            {"cloud_http_status", cloud.http_status},
            {"memory_size", ctx.snapshot.size()},
            {"decision", {{"route", "local_task_pipeline"}, {"reason", no_engine_outcome["reason"]}, {"policy", "reject"}, {"intent", cloud_intent}, {"confidence", cloud_confidence}}},
            {"tool_calls", cloud_tool_calls},
            {"observation", {{"cloud_policy", cloud_policy}, {"task_engine_ready", false}, {"pipeline_error_class", no_engine_error_class}, {"pipeline_error_code", no_engine_error_code}, {"pipeline_error", no_engine_outcome["error"]}, {"pipeline_outcome", no_engine_outcome}, {"cloud_task_pipeline_contract", pipeline_contract_obs}, {"cloud_complex_intent_gate", {{"enabled", cfg_.complex_intent_force_cloud}, {"detected", ctx.complex_intent_detected}, {"forced", ctx.force_cloud_for_complex}}}}},
            {"decision_trace", nlohmann::json::array({
                {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", ctx.intent_info.value("intent", "unknown")}, {"confidence", ctx.intent_info.value("confidence", 0.0)}}}},
                {{"layer", "llm"}, {"input", "cloud_strategy"}, {"output", cloud_policy}},
                {{"layer", "policy"}, {"input", pipeline_policy_input("reject", true, 0, no_engine_error_code)}, {"output", nlohmann::json{{"route", "local_task_pipeline"}, {"policy", "reject"}}}}
            })},
            {"text", policy_text("cloud.task_pipeline_reject", "")},
        };
      }

      nlohmann::json pipeline_steps = nlohmann::json::array();
      size_t completed = 0;
      bool failed = false;
      int failed_at = -1;
      std::string pipeline_error_class;

      // 参数校验：改用 SkillRegistry，无 skills 时 fallback
      auto validate_media_params_cp = [&](const std::string& action, const nlohmann::json& p) -> std::string {
        const auto* ad = skill_registry_.find_action(action);
        if (!ad) {
          if (action != "capture_photo" && action != "start_recording" && action != "stop_recording")
            return "invalid_action:" + action;
        } else if (!ad->params.empty()) {
          // 仅当 skill 声明了 params 时才做白名单校验
          for (auto it = p.begin(); it != p.end(); ++it) {
            if (ad->params.find(it.key()) == ad->params.end()) return "param_not_allowed:" + it.key();
          }
        }
        if (action == "start_recording" && p.contains("mode") && p["mode"].is_string()) {
          const std::string mode = p["mode"].get<std::string>();
          static const std::unordered_set<std::string> modes = {"normal", "video", "audio"};
          if (modes.find(mode) == modes.end()) return "invalid_mode:" + mode;
        }
        if (p.contains("count") && p["count"].is_number_integer()) {
          int c = p["count"].get<int>();
          if (c < 1 || c > 9) return "invalid_count";
        }
        return "";
      };

      for (size_t i = 0; i < task_pipeline.size(); ++i) {
        const auto& step = task_pipeline[i];
        const std::string action = step.value("action", "");
        nlohmann::json params = nlohmann::json::object();
        if (step.contains("params") && step["params"].is_object()) {
          params = step["params"];
        }

        nlohmann::json step_result = {
            {"index", static_cast<int>(i + 1)},
            {"action", action},
            {"submitted", false},
            {"success", false},
        };

        bool params_ok = true;
        std::string param_error;
        std::string validate_err = validate_media_params_cp(action, params);
        if (!validate_err.empty()) {
          params_ok = false;
          param_error = validate_err;
        }

        if (!params_ok) {
          failed = true;
          failed_at = static_cast<int>(i + 1);
          pipeline_error_class = "param";
          step_result["error"] = param_error;
          step_result["error_class"] = "param";
          pipeline_steps.push_back(step_result);
          break;
        }

        const std::string idem = "cloud-pipeline-" + ctx.session_id + "-" + std::to_string(ctx.snapshot.size()) + "-" +
                                 std::to_string(i + 1) + "-" + action + "-" + ctx.idem_token;
        const std::string task_id = task_engine_->submit_task(action, params, idem);
        const auto task = task_engine_->get_task(task_id);
        const std::string state = task.value("state", "unknown");

        cloud_tool_calls.push_back({{"tool", "task_submit"}, {"action", action}, {"idempotency_key", idem}});
        cloud_tool_calls.push_back({{"tool", "task_get"}, {"task_id", task_id}});

        const bool ok = (state == "success");
        step_result["submitted"] = true;
        step_result["success"] = ok;
        step_result["task_id"] = task_id;
        step_result["state"] = state;
        step_result["code"] = task.value("code", 0);
        step_result["message"] = task.value("message", "");

        if (!ok) {
          failed = true;
          failed_at = static_cast<int>(i + 1);
          pipeline_error_class = "task";
          step_result["error_class"] = "task";
          pipeline_steps.push_back(step_result);
          break;
        }
        pipeline_steps.push_back(step_result);
        ++completed;

        if (action == "start_recording" && params.contains("duration_sec") &&
            params["duration_sec"].is_number_integer()) {
          const char* fast = std::getenv("THIN_AGENT_FAST_MEDIA");
          const bool skip_wait = (fast && *fast && std::string(fast) != "0");
          const int dur = params["duration_sec"].get<int>();
          if (!skip_wait && dur > 0) {
            pipeline_steps.back()["duration_wait_sec"] = dur;
            std::this_thread::sleep_for(std::chrono::seconds(dur));
          } else if (skip_wait) {
            pipeline_steps.back()["duration_wait_skipped"] = true;
          }
        }
      }

      const bool rollback_triggered = cfg_.pipeline_enable_rollback_hook && failed && completed > 0;
      std::vector<std::string> rollback_actions;
      if (rollback_triggered) {
        for (size_t r = pipeline_steps.size(); r > 0; --r) {
          const auto& done_step = pipeline_steps[r - 1];
          if (!done_step.value("success", false)) {
            continue;
          }
          const std::string done_action = done_step.value("action", "");
          if (done_action == "start_recording") {
            rollback_actions.push_back("stop_recording");
          } else if (done_action == "stop_recording") {
            rollback_actions.push_back("start_recording");
          }
        }
      }
      nlohmann::json rollback_steps = nlohmann::json::array();
      size_t rollback_executed = 0;
      bool rollback_failed = false;
      for (const auto& rb_action : rollback_actions) {
        const std::string rb_idem = "cloud-pipeline-rollback-" + ctx.session_id + "-" +
                                    std::to_string(ctx.snapshot.size()) + "-" +
                                    std::to_string(rollback_executed + 1) + "-" + rb_action + "-" +
                                    ctx.idem_token;
        const std::string rb_task_id = task_engine_->submit_task(rb_action, nlohmann::json::object(), rb_idem);
        const auto rb_task = task_engine_->get_task(rb_task_id);
        const std::string rb_state = rb_task.value("state", "unknown");
        const bool rb_ok = (rb_state == "success");

        cloud_tool_calls.push_back({{"tool", "task_submit"}, {"action", rb_action}, {"idempotency_key", rb_idem}, {"phase", "rollback"}});
        cloud_tool_calls.push_back({{"tool", "task_get"}, {"task_id", rb_task_id}, {"phase", "rollback"}});

        rollback_steps.push_back({
            {"action", rb_action},
            {"task_id", rb_task_id},
            {"state", rb_state},
            {"success", rb_ok},
            {"code", rb_task.value("code", 0)},
            {"message", rb_task.value("message", "")},
        });
        if (!rb_ok) {
          rollback_failed = true;
          break;
        }
        ++rollback_executed;
      }
      const std::string rollback_status =
          !rollback_triggered ? "skipped" : (rollback_failed ? "failed" : "done");
      const std::string pipeline_error_code = pipeline_error_code_from_class(pipeline_error_class);
      const auto pipeline_outcome = pipeline_outcome_object(failed, pipeline_error_class, failed_at);

      nlohmann::json pipeline_execution = {
          {"step_count", task_pipeline.size()},
          {"completed", completed},
          {"failed", failed},
          {"failed_at", failed ? failed_at : -1},
          {"steps", pipeline_steps},
          {"rollback", {{"enabled", cfg_.pipeline_enable_rollback_hook},
                        {"triggered", rollback_triggered},
                        {"attempted_steps", rollback_triggered ? completed : 0},
                        {"executed_steps", rollback_executed},
                        {"status", rollback_status},
                        {"steps", rollback_steps}}},
      };
      nlohmann::json pipeline_error = pipeline_error_object(
          pipeline_error_class,
          pipeline_error_code,
          failed ? failed_at : -1);

      return {
          {"type", "chat_result"},
          {"mode_used", "local-agent"},
          {"intent_backend", "cloud-strategy"},
          {"provider", provider},
          {"model", cfg_.model_name},
          {"api_base", cfg_.api_base},
          {"api_key_env", cfg_.api_key_env},
          {"api_key_state", key_state},
          {"cloud_http_status", cloud.http_status},
          {"memory_size", ctx.snapshot.size()},
          {"decision", {{"route", "local_task_pipeline"}, {"reason", pipeline_outcome["reason"]}, {"policy", "execute"}, {"intent", cloud_intent}, {"confidence", cloud_confidence}}},
          {"tool_calls", cloud_tool_calls},
          {"observation", {{"cloud_policy", cloud_policy}, {"pipeline_execution", pipeline_execution}, {"pipeline_error", pipeline_error}, {"pipeline_outcome", pipeline_outcome}, {"pipeline_error_class", pipeline_error_class}, {"pipeline_error_code", pipeline_error_code}, {"cloud_task_pipeline_contract", pipeline_contract_obs}, {"cloud_complex_intent_gate", {{"enabled", cfg_.complex_intent_force_cloud}, {"detected", ctx.complex_intent_detected}, {"forced", ctx.force_cloud_for_complex}}}}},
          {"decision_trace", nlohmann::json::array({
              {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", ctx.intent_info.value("intent", "unknown")}, {"confidence", ctx.intent_info.value("confidence", 0.0)}}}},
              {{"layer", "llm"}, {"input", "cloud_strategy"}, {"output", cloud_policy}},
              {{"layer", "policy"}, {"input", pipeline_policy_input("execute", failed, static_cast<int>(completed), pipeline_error_code)}, {"output", nlohmann::json{{"route", "local_task_pipeline"}, {"policy", "execute"}}}}
          })},
          {"text", failed
                       ? render_template(policy_text("cloud.task_pipeline_failed", ""), {{"failed_at", std::to_string(failed_at)}})
                       : render_template(policy_text("cloud.task_pipeline_submitted", ""),
                                         {{"step_count", std::to_string(task_pipeline.size())}, {"completed", std::to_string(completed)}})},
      };
    }

    if (is_cloud_meta_question(ctx.text)) {
      std::string reply = resolve_cloud_reply_text(cloud_policy, cloud.text);
      if (reply.empty() || reply.size() > 400) {
        reply = render_template(
            policy_text("cloud.meta_answer", ""),
            {{"mode", cfg_.mode},
             {"model", cfg_.model_name},
             {"provider", normalize_provider(cfg_.provider)}});
      }
      const std::string style_name = policy_style_default("normal");
      const std::string answer_prefix = policy_style_text(
          style_name, "cloud.answer_prefix", policy_text("cloud.answer_prefix", ""));
      return {
          {"type", "chat_result"},
          {"mode_used", "cloud"},
          {"intent_backend", "cloud-strategy"},
          {"provider", provider},
          {"model", cfg_.model_name},
          {"api_base", cfg_.api_base},
          {"api_key_env", cfg_.api_key_env},
          {"api_key_state", key_state},
          {"cloud_http_status", cloud.http_status},
          {"memory_size", ctx.snapshot.size()},
          {"decision", {{"route", "cloud_llm"}, {"reason", "cloud_meta_question_answer"}, {"policy", "fallback_cloud"}, {"intent", "general_query"}, {"confidence", cloud_confidence}}},
          {"tool_calls", cloud_tool_calls},
          {"observation", {{"cloud_policy", cloud_policy}, {"cloud_task_pipeline_contract", pipeline_contract_obs}}},
          {"decision_trace", nlohmann::json::array({
              {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", ctx.intent_info.value("intent", "unknown")}, {"confidence", ctx.intent_info.value("confidence", 0.0)}}}},
              {{"layer", "llm"}, {"input", "cloud_strategy"}, {"output", cloud_policy}},
              {{"layer", "policy"}, {"input", nlohmann::json{{"reason", "cloud_meta_question_answer"}, {"route", "cloud_llm"}, {"policy", "fallback_cloud"}}}, {"output", nlohmann::json{{"route", "cloud_llm"}, {"policy", "fallback_cloud"}}}}
          })},
          {"text", answer_prefix + reply},
      };
    }

    if (local_route_hint == "local_external_weather" || cloud_intent == "weather") {
      const std::string norm = normalize_text_for_intent(ctx.text);
      std::string city = detect_city_slot(norm);
      if (city.empty() && ctx.dialog_ctx.last_slots.is_object()) {
        city = ctx.dialog_ctx.last_slots.value("city", "");
      }
      if (!city.empty()) {
        return handle_chat(ctx.session_id, city + weather_query_suffix(), "", ctx.on_chunk);
      }
    }

    if ((local_route_hint == "local_profile" || is_profile_like_text(ctx.text) || is_skills_only_query(ctx.text)) &&
        !is_cloud_meta_question(ctx.text) && !is_supported_languages_query(ctx.text)) {
      const std::string query_lang_fb = detect_query_language(ctx.text);
      const bool skill_query_fb = is_skills_only_query(ctx.text);
      const std::string profile_body = build_profile_body(query_lang_fb, role_mgr_.get(), skill_registry_, false, skill_query_fb);
      const std::string profile_text = apply_style_prefix("profile", "concise_prefix", profile_body, "normal");
      return {
          {"type", "chat_result"},
          {"mode_used", "local-agent"},
          {"intent_backend", "cloud-strategy"},
          {"provider", provider},
          {"model", cfg_.model_name},
          {"api_base", cfg_.api_base},
          {"api_key_env", cfg_.api_key_env},
          {"api_key_state", key_state},
          {"cloud_http_status", cloud.http_status},
          {"memory_size", ctx.snapshot.size()},
          {"decision", {{"route", "local_profile"}, {"reason", "cloud_strategy_match_local_profile"}, {"policy", "execute"}, {"intent", "profile"}, {"confidence", cloud_confidence}}},
          {"tool_calls", cloud_tool_calls},
          {"observation", {{"capabilities", nlohmann::json::array({"status", "event_recent", "metrics", "memory_recent", "memory_history", "action", "task_engine", "external_query"})}, {"profile_mode", "concise"}, {"cloud_policy", cloud_policy}, {"cloud_task_pipeline_contract", pipeline_contract_obs}, {"cloud_complex_intent_gate", {{"enabled", cfg_.complex_intent_force_cloud}, {"detected", ctx.complex_intent_detected}, {"forced", ctx.force_cloud_for_complex}}}}},
          {"decision_trace", nlohmann::json::array({
              {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", ctx.intent_info.value("intent", "unknown")}, {"confidence", ctx.intent_info.value("confidence", 0.0)}}}},
              {{"layer", "llm"}, {"input", "cloud_strategy"}, {"output", cloud_policy}},
              {{"layer", "policy"}, {"input", nlohmann::json{{"route_hint", local_route_hint}, {"risk", cloud_risk}, {"route", "local_profile"}, {"policy", "execute"}}}, {"output", nlohmann::json{{"route", "local_profile"}, {"policy", "execute"}}}}
          })},
          {"text", profile_text},
      };
    }

    if (local_route_hint == "local_status") {
      auto st = status_payload();
      return {
          {"type", "chat_result"},
          {"mode_used", "local-agent"},
          {"intent_backend", "cloud-strategy"},
          {"provider", provider},
          {"model", cfg_.model_name},
          {"api_base", cfg_.api_base},
          {"api_key_env", cfg_.api_key_env},
          {"api_key_state", key_state},
          {"cloud_http_status", cloud.http_status},
          {"memory_size", ctx.snapshot.size()},
          {"decision", {{"route", "local_status"}, {"reason", "cloud_strategy_match_local_status"}, {"policy", "execute"}, {"intent", "status"}, {"confidence", cloud_confidence}}},
          {"tool_calls", nlohmann::json::array({{{"tool", "cloud_llm"}, {"provider", provider}, {"model", cfg_.model_name}}, {{"tool", "status"}}})},
          {"observation", {{"status", st}, {"cloud_policy", cloud_policy}, {"cloud_task_pipeline_contract", pipeline_contract_obs}, {"cloud_complex_intent_gate", {{"enabled", cfg_.complex_intent_force_cloud}, {"detected", ctx.complex_intent_detected}, {"forced", ctx.force_cloud_for_complex}}}}},
          {"decision_trace", nlohmann::json::array({
              {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", ctx.intent_info.value("intent", "unknown")}, {"confidence", ctx.intent_info.value("confidence", 0.0)}}}},
              {{"layer", "llm"}, {"input", "cloud_strategy"}, {"output", cloud_policy}},
              {{"layer", "policy"}, {"input", nlohmann::json{{"route_hint", local_route_hint}, {"risk", cloud_risk}, {"route", "local_status"}, {"policy", "execute"}}}, {"output", nlohmann::json{{"route", "local_status"}, {"policy", "execute"}}}}
          })},
          {"text", render_template(
                       policy_text("cloud.status_local", ""),
                       {{"mode", cfg_.mode},
                        {"model", cfg_.model_name},
                        {"provider", normalize_provider(cfg_.provider)}})},
      };
    }

    if (local_route_hint == "local_task_inline_capture_photo" || local_route_hint == "local_task_inline_start_recording") {
      const std::string action = (local_route_hint == "local_task_inline_capture_photo") ? "capture_photo" : "start_recording";
      if (!task_engine_) {
        return {
            {"type", "chat_result"},
            {"mode_used", "local-agent"},
            {"intent_backend", "cloud-strategy"},
            {"provider", provider},
            {"model", cfg_.model_name},
            {"api_base", cfg_.api_base},
            {"api_key_env", cfg_.api_key_env},
            {"api_key_state", key_state},
            {"cloud_http_status", cloud.http_status},
            {"memory_size", ctx.snapshot.size()},
            {"decision", {{"route", "local_task_inline"}, {"reason", "cloud_strategy_task_engine_unavailable"}, {"policy", "reject"}, {"intent", cloud_intent}, {"confidence", cloud_confidence}}},
            {"tool_calls", cloud_tool_calls},
            {"observation", {{"cloud_policy", cloud_policy}, {"task_engine_ready", false}, {"pipeline_error_class", "engine"}, {"cloud_task_pipeline_contract", pipeline_contract_obs}, {"cloud_complex_intent_gate", {{"enabled", cfg_.complex_intent_force_cloud}, {"detected", ctx.complex_intent_detected}, {"forced", ctx.force_cloud_for_complex}}}}},
            {"decision_trace", nlohmann::json::array({
                {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", ctx.intent_info.value("intent", "unknown")}, {"confidence", ctx.intent_info.value("confidence", 0.0)}}}},
                {{"layer", "llm"}, {"input", "cloud_strategy"}, {"output", cloud_policy}},
                {{"layer", "policy"}, {"input", nlohmann::json{{"route_hint", local_route_hint}, {"risk", cloud_risk}, {"route", "local_task_inline"}, {"policy", "reject"}}}, {"output", nlohmann::json{{"route", "local_task_inline"}, {"policy", "reject"}}}}
            })},
            {"text", policy_text("cloud.task_inline_reject", "")},
        };
      }

      const std::string idem =
          "cloud-inline-" + ctx.session_id + "-" + std::to_string(ctx.snapshot.size()) + "-" + action + "-" +
          ctx.idem_token;
      const std::string task_id = task_engine_->submit_task(action, nlohmann::json::object(), idem);
      const auto task = task_engine_->get_task(task_id);
      const std::string state = task.value("state", "unknown");
      auto task_tool_calls = cloud_tool_calls;
      task_tool_calls.push_back({{"tool", "task_submit"}, {"action", action}, {"idempotency_key", idem}});
      task_tool_calls.push_back({{"tool", "task_get"}, {"task_id", task_id}});

      return {
          {"type", "chat_result"},
          {"mode_used", "local-agent"},
          {"intent_backend", "cloud-strategy"},
          {"provider", provider},
          {"model", cfg_.model_name},
          {"api_base", cfg_.api_base},
          {"api_key_env", cfg_.api_key_env},
          {"api_key_state", key_state},
          {"cloud_http_status", cloud.http_status},
          {"memory_size", ctx.snapshot.size()},
          {"decision", {{"route", "local_task_inline"}, {"reason", "cloud_strategy_match_local_task_inline"}, {"policy", "execute"}, {"intent", cloud_intent}, {"confidence", cloud_confidence}, {"slots", {{"action", action}, {"task_id", task_id}}}}},
          {"tool_calls", task_tool_calls},
          {"observation", {{"cloud_policy", cloud_policy}, {"task", task}, {"inline_task", {{"action", action}, {"task_id", task_id}, {"state", state}}}, {"cloud_task_pipeline_contract", pipeline_contract_obs}, {"cloud_complex_intent_gate", {{"enabled", cfg_.complex_intent_force_cloud}, {"detected", ctx.complex_intent_detected}, {"forced", ctx.force_cloud_for_complex}}}}},
          {"decision_trace", nlohmann::json::array({
              {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", ctx.intent_info.value("intent", "unknown")}, {"confidence", ctx.intent_info.value("confidence", 0.0)}}}},
              {{"layer", "llm"}, {"input", "cloud_strategy"}, {"output", cloud_policy}},
              {{"layer", "policy"}, {"input", nlohmann::json{{"route_hint", local_route_hint}, {"risk", cloud_risk}, {"action", action}, {"route", "local_task_inline"}, {"policy", "execute"}}}, {"output", nlohmann::json{{"route", "local_task_inline"}, {"policy", "execute"}}}}
          })},
          {"text", render_template(policy_text("cloud.task_inline_submitted", ""),
                                   {{"action", action}, {"task_id", task_id}, {"state", state}})},
      };
    }

    if ((is_profile_like_text(ctx.text) || is_skills_only_query(ctx.text)) && !is_cloud_meta_question(ctx.text) && !is_supported_languages_query(ctx.text)) {
      const bool skill_query = is_skills_only_query(ctx.text);
      const std::string profile_body = build_profile_body(ctx.query_lang, role_mgr_.get(), skill_registry_, false, skill_query);
      const std::string profile_text = apply_style_prefix("profile", "concise_prefix", profile_body, "normal");
      note_dialog_state(ctx.session_id, "profile", "local_profile", {{"profile_mode", "concise"}});
      return {
          {"type", "chat_result"},
          {"mode_used", "local-agent"},
          {"intent_backend", "cloud-strategy"},
          {"provider", provider},
          {"model", cfg_.model_name},
          {"api_base", cfg_.api_base},
          {"api_key_env", cfg_.api_key_env},
          {"api_key_state", key_state},
          {"cloud_http_status", cloud.http_status},
          {"memory_size", ctx.snapshot.size()},
          {"decision", {{"route", "local_profile"}, {"reason", "identity_hard_gate_local_profile"}, {"policy", "execute"}, {"intent", "profile"}, {"confidence", 0.99}}},
          {"tool_calls", cloud_tool_calls},
          {"observation", {{"capabilities", nlohmann::json::array({"status", "event_recent", "metrics", "memory_recent", "memory_history", "action", "task_engine", "external_query"})}, {"profile_mode", "concise"}, {"cloud_policy", cloud_policy}, {"cloud_task_pipeline_contract", pipeline_contract_obs}, {"cloud_complex_intent_gate", {{"enabled", cfg_.complex_intent_force_cloud}, {"detected", ctx.complex_intent_detected}, {"forced", ctx.force_cloud_for_complex}}}}},
          {"decision_trace", nlohmann::json::array({
              {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", ctx.intent_info.value("intent", "unknown")}, {"confidence", ctx.intent_info.value("confidence", 0.0)}}}},
              {{"layer", "llm"}, {"input", "cloud_strategy"}, {"output", cloud_policy}},
              {{"layer", "policy"}, {"input", nlohmann::json{{"reason", "identity_hard_gate"}, {"route", "local_profile"}, {"policy", "execute"}}}, {"output", nlohmann::json{{"route", "local_profile"}, {"policy", "execute"}}}}
          })},
          {"text", profile_text},
      };
    }

    const std::string style_name = policy_style_default("normal");
    const std::string answer_prefix = policy_style_text(
        style_name,
        "cloud.answer_prefix",
        policy_text("cloud.answer_prefix", ""));
    return {
        {"type", "chat_result"},
        {"mode_used", "cloud"},
        {"intent_backend", "cloud-strategy"},
        {"provider", provider},
        {"model", cfg_.model_name},
        {"api_base", cfg_.api_base},
        {"api_key_env", cfg_.api_key_env},
        {"api_key_state", key_state},
        {"cloud_http_status", cloud.http_status},
        {"memory_size", ctx.snapshot.size()},
        {"decision", {{"route", "cloud_llm"}, {"reason", "cloud_strategy_answer_direct"}, {"policy", "fallback_cloud"}, {"intent", cloud_intent}, {"confidence", cloud_confidence}}},
        {"tool_calls", cloud_tool_calls},
        {"observation", {{"cloud_policy", cloud_policy}, {"style", style_name}, {"cloud_task_pipeline_contract", pipeline_contract_obs}, {"cloud_complex_intent_gate", {{"enabled", cfg_.complex_intent_force_cloud}, {"detected", ctx.complex_intent_detected}, {"forced", ctx.force_cloud_for_complex}}}}},
        {"decision_trace", nlohmann::json::array({
            {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", ctx.intent_info.value("intent", "unknown")}, {"confidence", ctx.intent_info.value("confidence", 0.0)}}}},
            {{"layer", "llm"}, {"input", "cloud_strategy"}, {"output", cloud_policy}},
            {{"layer", "policy"}, {"input", nlohmann::json{{"route_hint", local_route_hint}, {"risk", cloud_risk}, {"route", "cloud_llm"}, {"policy", "fallback_cloud"}}}, {"output", nlohmann::json{{"route", "cloud_llm"}, {"policy", "fallback_cloud"}}}}
        })},
        {"text", answer_prefix + resolve_cloud_reply_text(cloud_policy, cloud.text)},
    };
  }
  return nullptr;
}

/// v0.50.0: 云前置域（原 handle_chat 段 G1）。
/// budget 预算检查（gate 意图循环）+ key 解析 + offline 模式 /
/// missing-key / auto 模式本地级联三路提前终结。返回 null 表示继续云端。
/// 出参 provider/key_state/api_key 供后续云端域使用。
nlohmann::json AgentService::chat_cloud_precheck(ChatContext& ctx,
                                                 std::string& provider,
                                                 std::string& key_state,
                                                 std::string& api_key) {
  // v0.42.3: cron 任务 provider 覆盖（通过 cfg_ 临时覆盖实现，见 cron callback）
  provider = normalize_provider(cfg_.provider);
  key_state = "not_required";

  const int input_chars = static_cast<int>(ctx.text.size());
  const int latency_budget_ms =
      cfg_.budget_max_latency_ms > 0 ? cfg_.budget_max_latency_ms : cfg_.request_timeout_ms;
  const double estimated_cost_cents = static_cast<double>(input_chars) * 0.002;
  const bool budget_input_exceeded =
      (cfg_.budget_max_input_chars > 0 && input_chars > cfg_.budget_max_input_chars);
  const bool budget_latency_exceeded =
      (cfg_.budget_max_latency_ms > 0 && latency_budget_ms < cfg_.request_timeout_ms);
  const bool budget_cost_exceeded =
      (cfg_.budget_max_cost_cents > 0.0 && estimated_cost_cents > cfg_.budget_max_cost_cents);

  // 配置驱动的 gate 意图（budget 超预算澄清等）
  for (const auto& gate_name : intent_names_by_kind("gate")) {
    IntentSpec gate_spec;
    if (!load_intent_spec(gate_name, &gate_spec) || gate_spec.kind != "gate") continue;
    if (!gate_spec.gate_mode.empty() && gate_spec.gate_mode != cfg_.mode) continue;
    if (gate_spec.predicate == "budget_exceeded") {
      if (!budget_input_exceeded && !budget_latency_exceeded && !budget_cost_exceeded) continue;
      std::string reason = gate_spec.reason_budget_input;
      if (budget_latency_exceeded) reason = gate_spec.reason_budget_latency;
      if (budget_cost_exceeded) reason = gate_spec.reason_budget_cost;
      const std::string label =
          gate_spec.intent_label.empty() ? gate_spec.name : gate_spec.intent_label;
      return make_local_reply(ctx, 
          gate_spec.route_execute.empty() ? "local_clarify" : gate_spec.route_execute,
          reason,
          policy_text(gate_spec.clarify_template_key, ""),
          {{"budget_gate",
            {{"input_chars", input_chars},
             {"max_input_chars", cfg_.budget_max_input_chars},
             {"request_timeout_ms", cfg_.request_timeout_ms},
             {"max_latency_ms", cfg_.budget_max_latency_ms},
             {"estimated_cost_cents", estimated_cost_cents},
             {"max_cost_cents", cfg_.budget_max_cost_cents},
             {"input_exceeded", budget_input_exceeded},
             {"latency_exceeded", budget_latency_exceeded},
             {"cost_exceeded", budget_cost_exceeded}}}},
          nlohmann::json::array(),
          label,
          gate_spec.default_confidence,
          nlohmann::json::object(),
          "clarify");
    }
  }

  api_key = resolve_api_key(credential_pool_.get(), cfg_.api_key_env, cfg_.provider);
  // v0.49.2: 测试 mock 注入存在时无需真实 key（CloudLlmClient 走 mock，
  // 不发网络请求）。否则单测里 mock 永远不可达——key 检查提前 return。
  const char* test_mock_env = std::getenv("THIN_AGENT_TEST_CLOUD_RESPONSE");
  const bool test_mock_active = (test_mock_env && *test_mock_env);
  // v0.53.34: lmstudio 本地端点无需 API key——不因 key 缺失降级 offline
  const bool provider_keyless =
      normalize_provider(cfg_.provider) == "lmstudio";
  key_state = (api_key.empty() && !test_mock_active && !provider_keyless)
                  ? "missing" : "present";

  if (cfg_.mode == "offline") {
    // 离线模式：走 HybridRouter (TemplateModel → GgufModel → fallback)
    thin_agent::local::HybridRouter local_router;
    thin_agent::local::CascadeResult local_result;
    if (ctx.on_chunk) {
      local_result = local_router.route_stream("chat", ctx.text, ctx.on_chunk, 256);
    } else {
      local_result = local_router.route("chat", ctx.text, 256);
    }
    std::string offline_text;
    if (local_result.ok && !local_result.output.empty()) {
      offline_text = local_result.output;
    } else {
      offline_text = render_template(
          policy_text("cloud.offline_local_fallback",
                      policy_text("cloud.local_offline_fallback",
                                  "[offline] 当前离线模式。可执行本地动作。输入回显：{ctx.text}")),
          {{"text", ctx.text}});
    }
    return {
        {"type", "chat_result"},
        {"mode_used", "offline"},
        {"intent_backend", "rules"},
        {"memory_size", ctx.snapshot.size()},
        {"decision", {{"route", local_result.ok ? local_result.model_used : "offline_fallback"},
                      {"reason", "offline_mode_no_cloud"},
                      {"policy", "execute"}}},
        {"tool_calls", nlohmann::json::array()},
        {"decision_trace", nlohmann::json::array({{{"layer", "rules"}, {"input", ctx.text},
                                                    {"output", local_result.ok ? local_result.model_used : "offline_fallback"}}})},
        {"text", offline_text},
    };
  }

  if (key_state == "missing") {
    thin_agent::local::HybridRouter local_router;
    thin_agent::local::CascadeResult local_result;
    if (ctx.on_chunk) {
      local_result = local_router.route_stream("chat", ctx.text, ctx.on_chunk, 256);
    } else {
      local_result = local_router.route("chat", ctx.text, 256);
    }
    std::string fallback_text;
    if (local_result.ok && !local_result.output.empty()) {
      fallback_text = local_result.output;
    } else {
      fallback_text = render_template(
          policy_text("cloud.fallback_missing_key",
                      policy_text("cloud.missing_key_fallback",
                                  "[offline] 未检测到云模型密钥。输入回显：{ctx.text}")),
          {{"text", ctx.text}});
    }
    return {
        {"type", "chat_result"},
        {"mode_used", "offline-fallback"},
        {"intent_backend", "cloud"},
        {"provider", provider},
        {"model", cfg_.model_name},
        {"api_base", cfg_.api_base},
        {"api_key_env", cfg_.api_key_env},
        {"api_key_state", key_state},
        {"fallback_reason", "missing_api_key"},
        {"memory_size", ctx.snapshot.size()},
        {"decision", {{"route", local_result.ok ? local_result.model_used : "cloud_fallback"},
                      {"reason", "missing_api_key"},
                      {"policy", "fallback_cloud"}}},
        {"tool_calls", nlohmann::json::array()},
        {"decision_trace", nlohmann::json::array({
            {{"layer", "intent"}, {"input", ctx.text}, {"output", nlohmann::json{{"intent", "general_query"}, {"confidence", 0.40}}}},
            {{"layer", "policy"}, {"input", nlohmann::json{{"reason", "missing_api_key"}, {"risk", "none"}, {"confidence", 0.0}, {"route", local_result.ok ? local_result.model_used : "cloud_fallback"}, {"policy", "fallback_cloud"}}}, {"output", nlohmann::json{{"route", local_result.ok ? local_result.model_used : "cloud_fallback"}, {"policy", "fallback_cloud"}}}}
        })},
        {"text", fallback_text},
    };
  }

  // ── 本地优先：auto 模式（混合）或无云 key 时，完整 cascade 后再上云 ──
  //  与 offline 走相同级联（Template → Qwen懒加载 → Gemma懒加载），
  //  区别：级联失败不终止，继续走云端
  if ((cfg_.mode == "auto" || key_state != "present") &&
      !ctx.force_cloud_for_complex && cfg_.fallback != "none") {
    thin_agent::local::HybridRouter local_router;
    thin_agent::local::CascadeResult local_result;
    if (ctx.on_chunk) {
      local_result = local_router.route_stream("chat", ctx.text, ctx.on_chunk, 256);
    } else {
      local_result = local_router.route("chat", ctx.text, 256);
    }
    if (local_result.ok && !local_result.output.empty()) {
      std::string reply_text = local_result.output;
      nlohmann::json observation = nlohmann::json::object();

      ctx.query_lang = detect_query_language(ctx.text);
      const std::string reply_lang = detect_query_language(reply_text);
      observation["query_lang"] = ctx.query_lang;
      observation["template_lang"] = reply_lang;

      const std::string translated_reply =
          apply_reply_translation_if_needed(cfg_, reply_text, ctx.query_lang, reply_lang, ctx.text);
      if (translated_reply != reply_text) {
        reply_text = translated_reply;
        observation["reply_translation_applied"] = true;
        observation["translation_applied"] = true;
      }

      return {
          {"type", "chat_result"},
          {"mode_used", "local-agent"},
          {"intent_backend", "rules"},
          {"provider", provider},
          {"model", local_result.model_used},
          {"memory_size", ctx.snapshot.size()},
          {"decision", {{"route", local_result.model_used},
                        {"reason", "local_cascade_match"},
                        {"policy", "local_first"}}},
          {"tool_calls", nlohmann::json::array()},
          {"decision_trace", nlohmann::json::array({
              {{"layer", "local_cascade"}, {"input", ctx.text},
               {"output", local_result.model_used},
               {"tried", local_result.tried}}
          })},
          {"text", reply_text},
          {"observation", observation},
      };
    }
    // cascade 未命中 → 继续走云端
  }

  return nullptr;
}

}  // namespace thin_agent
