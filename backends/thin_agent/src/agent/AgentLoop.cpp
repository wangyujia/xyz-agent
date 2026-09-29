#include "thin_agent/agent/AgentLoop.h"
#include "thin_agent/core/HookSystem.h"

#include <algorithm>
#include <regex>
#include <sstream>

#include "thin_agent/agent/AgentTracer.h"
#include "thin_agent/agent/EmbeddingProvider.h"
#include "thin_agent/agent/ErrorDetector.h"
#include "thin_agent/agent/MemoryManager.h"
#include "thin_agent/agent/SkillManager.h"      // v0.39.1: skill 注入
#include "thin_agent/core/SecurityRedactor.h"
#include "thin_agent/core/PathValidator.h"   // v0.49.0: 统一风险分级
#include "thin_agent/core/Utf8Util.h"
#include "thin_agent/llm/CloudLlmClient.h"
#include "thin_agent/llm/CurlHttpClient.h"
#include "thin_agent/local/HybridRouter.h"
#include "thin_agent/llm/DemoConfigCompat.h"

namespace thin_agent {
namespace agent {

namespace {

std::string extract_json_block(const std::string& raw) {
  std::regex cb(R"(```json\s*\n?([\s\S]*?)\n?```)");
  std::smatch m;
  if (std::regex_search(raw, m, cb)) return m[1].str();
  std::regex cbg(R"(```\s*\n?([\s\S]*?)\n?```)");
  if (std::regex_search(raw, m, cbg)) return m[1].str();
  auto start = raw.find('{');
  if (start == std::string::npos) return raw;
  int depth = 0;
  for (size_t i = start; i < raw.size(); ++i) {
    if (raw[i] == '{') ++depth;
    else if (raw[i] == '}') { --depth; if (depth == 0) return raw.substr(start, i-start+1); }
  }
  return raw.substr(start);
}

std::string repair_json(const std::string& s) {
  std::string r;
  bool in_str = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char ch = s[i];
    if (ch == '"' && (i == 0 || s[i-1] != '\\')) in_str = !in_str;
    if (!in_str && ch == ',' && i+1 < s.size()) {
      size_t j = i+1;
      while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
      if (j < s.size() && (s[j] == '}' || s[j] == ']')) continue;
    }
    r.push_back(ch);
  }
  return r;
}

int estimate_token_count(const std::string& text) {
  // 粗略估算：英文 ~4 chars/token，中文 ~1.5 chars/token
  int en = 0, zh = 0;
  for (unsigned char c : text) {
    if (c < 128) ++en; else ++zh;
  }
  return en / 4 + zh * 2 / 3;
}

}  // namespace

AgentLoop::AgentLoop(const DemoConfigCompat& cfg, const std::string& api_key, ToolRegistry& registry)
    : cfg_(cfg), api_key_(api_key), registry_(registry) {}

AgentLoop::AgentLoop(const DemoConfigCompat& cfg, const std::string& api_key,
                     ToolRegistry& registry, AgentLoopConfig loop_cfg)
    : cfg_(cfg), api_key_(api_key), registry_(registry), loop_cfg_(std::move(loop_cfg)) {}

// ── 工具白名单过滤 ───────────────────────────────────────────

bool AgentLoop::is_tool_allowed(const std::string& name) const {
  if (loop_cfg_.allowed_tools.empty()) return true;  // 空 = 全部可用
  return loop_cfg_.allowed_tools.count(name) > 0;
}

nlohmann::json AgentLoop::filter_tools_json(const nlohmann::json& all_tools) const {
  if (loop_cfg_.allowed_tools.empty() || !all_tools.is_array()) return all_tools;
  nlohmann::json filtered = nlohmann::json::array();
  for (const auto& t : all_tools) {
    std::string fn = t.value("function", nlohmann::json::object()).value("name", "");
    if (is_tool_allowed(fn)) filtered.push_back(t);
  }
  return filtered;
}

// v0.53.5: 子代理工具钩子闸——主链 execute_one_tool 自 v0.52.29 过 tool_pre，
// 子代理链（AgentLoop→registry_.call）此前绕过：deny 钩子对子代理失效。
// 语义与主链一致：deny→工具不执行，理由作为 error 喂回子代理对话。
ToolCallResult AgentLoop::hook_gate(const std::string& name,
                                    const nlohmann::json& args) const {
  ToolCallResult denied;
  denied.tool_name = name;
  nlohmann::json hp;
  hp["tool"] = name;
  hp["args"] = args;
  auto verdict = HookSystem::instance().dispatch(HookEvent::ToolPre, hp);
  if (verdict.deny) {
    denied.ok = false;
    denied.error = "错误: 该操作被钩子拒绝 (tool_pre deny)。原因: " +
                   (verdict.reason.empty() ? "unspecified" : verdict.reason);
  }
  return denied;  // ok 默认路径由调用侧以 error 空判过闸
}

std::string AgentLoop::filter_tools_prompt(const std::string& raw_prompt) const {
  if (loop_cfg_.allowed_tools.empty()) return raw_prompt;  // 全部可用，透传
  // 简单策略：如果有限制，重新构建精简 prompt
  std::string filtered;
  for (const auto& t : registry_.all_tools()) {
    if (!is_tool_allowed(t.name)) continue;
    filtered += "- " + t.name + ": " + t.description + "\n";
  }
  return filtered;
}

// ── LLM 调用 ──────────────────────────────────────────────────

nlohmann::json AgentLoop::call_llm_native(const std::vector<ChatMessage>& messages,
                                           const nlohmann::json& tools_json) {
  auto span_id = AgentTracer::instance().start_span("llm_call");

  CloudChatResult result;

  if (loop_cfg_.stream_output) {
    CurlHttpClient http(loop_cfg_.request_timeout_ms);  // v0.52.3: 超时对齐配置（曾 15s 默认→GLM 慢时 decompose 卡 3min+）
    result = CloudLlmClient::chat_completion_with_tools(
        http, cfg_, api_key_, messages, tools_json, true, nullptr);
  } else {
    CurlHttpClient http(loop_cfg_.request_timeout_ms);  // v0.52.3: 同上
    result = CloudLlmClient::chat_completion_with_tools(
        http, cfg_, api_key_, messages, tools_json);
  }

  nlohmann::json out;
  out["ok"] = result.ok;
  out["text"] = result.text;
  out["http_status"] = result.http_status;
  out["error"] = result.error;
  // v0.53.30: 删 out["stream_chunks"]——零消费端死代码（子代理回复在 JSON
  // 里 text+chunks 双份膨胀，spawn 结果帧体积翻倍无收益）

  // 估算 token
  int in_tok = 0;
  for (const auto& m : messages) in_tok += estimate_token_count(m.content);
  int out_tok = estimate_token_count(result.text);
  AgentTracer::instance().record_llm(span_id, in_tok, out_tok, cfg_.model_name);
  AgentTracer::instance().end_span(span_id, result.ok ? "ok" : "error");

  return out;
}

nlohmann::json AgentLoop::call_llm_text(const std::vector<ChatMessage>& messages) {
  auto span_id = AgentTracer::instance().start_span("llm_call");
  CurlHttpClient http(loop_cfg_.request_timeout_ms);  // v0.52.3: 超时对齐配置
  auto result = CloudLlmClient::chat_completion(http, cfg_, api_key_, messages);
  nlohmann::json out;
  out["ok"] = result.ok;
  out["text"] = result.text;
  out["http_status"] = result.http_status;
  out["error"] = result.error;

  int in_tok = 0;
  for (const auto& m : messages) in_tok += estimate_token_count(m.content);
  int out_tok = estimate_token_count(result.text);
  AgentTracer::instance().record_llm(span_id, in_tok, out_tok, cfg_.model_name);
  AgentTracer::instance().end_span(span_id, result.ok ? "ok" : "error");

  return out;
}

// ── JSON 解析 ─────────────────────────────────────────────────

nlohmann::json AgentLoop::parse_llm_output(const std::string& raw_output) {
  std::string js = extract_json_block(raw_output);
  nlohmann::json parsed;
  try { parsed = nlohmann::json::parse(js); }
  catch (...) {
    std::string repaired = repair_json(js);
    try { parsed = nlohmann::json::parse(repaired); }
    catch (const std::exception& e) {
      return {{"parse_error", std::string("JSON parse: ") + e.what()}};
    }
  }

  if (parsed.contains("final_answer") && parsed["final_answer"].is_string())
    return {{"final_answer", parsed["final_answer"].get<std::string>()}, {"tool_calls", nlohmann::json::array()}};

  if (parsed.contains("tool_calls") && parsed["tool_calls"].is_array()) {
    nlohmann::json out;
    out["tool_calls"] = nlohmann::json::array();
    for (const auto& tc : parsed["tool_calls"]) {
      if (!tc.is_object() || !tc.contains("name")) continue;
      nlohmann::json c;
      c["name"] = tc["name"].get<std::string>();
      c["params"] = tc.contains("params") && tc["params"].is_object() ? tc["params"] : nlohmann::json::object();
      out["tool_calls"].push_back(c);
    }
    if (parsed.contains("final_answer") && parsed["final_answer"].is_string())
      out["final_answer"] = parsed["final_answer"].get<std::string>();
    return out;
  }

  if (parsed.is_string()) return {{"final_answer", parsed.get<std::string>()}, {"tool_calls", nlohmann::json::array()}};
  return {{"final_answer", raw_output}, {"tool_calls", nlohmann::json::array()}};
}

ChatMessage AgentLoop::build_tool_observation(const std::vector<ToolCallResult>& results) {
  std::ostringstream os;
  os << "工具调用结果：\n\n";
  for (size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    os << "[" << (i+1) << "] " << r.tool_name << ": " << (r.ok ? "成功" : "失败") << "\n";
    // v0.33.0: 红标敏感信息后再送入 LLM 上下文
    auto redacted = SecurityRedactor::redact_tool_result(r.ok ? r.result : nlohmann::json(r.error));
    if (r.ok) {
      os << redacted.dump();
    } else {
      os << redacted.get<std::string>();
    }
    os << "\n\n";
  }
  return ChatMessage{"user", os.str()};
}

int AgentLoop::estimate_chars(const std::vector<ChatMessage>& messages) {
  int t = 0; for (const auto& m : messages) t += m.role.size() + m.content.size(); return t;
}

// ── 上下文压缩 ────────────────────────────────────────────────

std::vector<ChatMessage> AgentLoop::compress_messages(
    const std::vector<ChatMessage>& messages) {
  if (!loop_cfg_.enable_compression || messages.size() < 5) return messages;

  // 保留 system + 最后 2 轮
  std::vector<ChatMessage> out;
  out.push_back(messages[0]);

  // 对中间轮次生成摘要
  std::ostringstream summary;
  summary << "以下是之前对话的摘要：\n";
  int chars = 0;
  for (size_t i = 1; i + 4 < messages.size(); ++i) {
    int cost = static_cast<int>(messages[i].content.size());
    if (chars + cost > 2000) break;
    std::string role_label = messages[i].role == "assistant" ? "助手" :
                             messages[i].role == "user" ? "用户" : messages[i].role;
    summary << "[" << role_label << "] " << thin_agent::utf8_truncate(messages[i].content, 200) << "\n";
    chars += cost;
  }
  out.push_back(ChatMessage{"system", summary.str()});

  // 保留最后 4 条
  for (size_t i = messages.size() > 4 ? messages.size() - 4 : 1; i < messages.size(); ++i)
    out.push_back(messages[i]);

  return out;
}

// ── 人工确认恢复 ──────────────────────────────────────────────

AgentLoopResult AgentLoop::continue_after_approval(bool approved) {
  AgentLoopResult result;
  if (paused_tool_name_.empty()) {
    result.ok = true;
    result.final_answer = "无待确认操作。";
    return result;
  }

  if (!approved) {
    result.ok = true;
    result.final_answer = "操作已取消。";
    paused_tool_name_.clear();
    return result;
  }

  if (!is_tool_allowed(paused_tool_name_)) {
    result.ok = false;
    result.error = "tool not allowed in role: " + paused_tool_name_;
    return result;
  }

  auto span_id = AgentTracer::instance().start_span("tool_call");
  ToolCallResult call_result = [&] {
    ToolCallResult g = hook_gate(paused_tool_name_, paused_tool_params_);
    if (!g.error.empty()) return g;
    return registry_.call(paused_tool_name_, paused_tool_params_,
                          loop_cfg_.request_timeout_ms);
  }();
  AgentTracer::instance().record_tool(span_id, paused_tool_name_,
                                       paused_tool_params_,
                                       call_result.ok ? call_result.result : nlohmann::json(call_result.error));
  AgentTracer::instance().end_span(span_id, call_result.ok ? "ok" : "error");

  result.tool_calls.push_back(call_result);
  paused_messages_.push_back(ChatMessage{"assistant", "tool: " + paused_tool_name_});
  paused_messages_.push_back(build_tool_observation({call_result}));

  // 继续循环
  paused_tool_name_.clear();
  // v0.53.48: 传暂停现场续跑(此前 run("") 重开空对话=批准前工具链全丢)
  auto resumed = std::move(paused_messages_);
  paused_messages_.clear();
  return run("", "", nlohmann::json::object(), std::move(resumed));
}

// ── 主循环 ────────────────────────────────────────────────────

AgentLoopResult AgentLoop::run(const std::string& user_query,
                                const std::string& system_prompt,
                                const nlohmann::json& memory_context,
                                std::vector<ChatMessage> resume_messages) {
  AgentLoopResult result;
  AgentTracer::instance().begin_session("agent_loop");
  int retries_remaining = loop_cfg_.max_retries;

  std::string full_prompt = system_prompt;

  // 记忆上下文
  std::string memory_text;
  if (memory_context.is_object() && !memory_context.empty()) {
    if (memory_context.contains("relevant_memories") &&
        memory_context["relevant_memories"].is_string()) {
      memory_text = memory_context["relevant_memories"].get<std::string>();
    }
  }
  // v0.38.0: 如果调用方未传 memory_context 但设置了 memory_mgr_，
  // 自动用 user_query 召回语义记忆（子 Agent 无需改调用签名）
  if (memory_text.empty() && memory_mgr_ && !user_query.empty()) {
    memory_text = memory_mgr_->build_context(user_query, 3);
  }
  if (!memory_text.empty()) {
    full_prompt += "\n\n## 相关历史记忆\n" + memory_text;
  }

  // v0.39.1: 技能自进化注入（子 Agent 路径，对齐主 FC 路径 v0.38.1）
  if (skill_mgr_ && !user_query.empty()) {
    auto matched = skill_mgr_->match_skills(user_query, 3, 0.3f);
    if (!matched.empty()) {
      std::string skill_inj = SkillManager::to_prompt_injection(matched);
      if (!skill_inj.empty()) {
        full_prompt += "\n\n" + skill_inj;
      }
    }
  }

  // 工具描述（非 native fc 时使用）
  std::string tools_text_prompt;
  nlohmann::json tools_openai = nlohmann::json::array();

  if (registry_.size() > 0) {
    tools_openai = filter_tools_json(registry_.to_openai_tools());
    if (!loop_cfg_.use_native_fc) {
      full_prompt += "\n\n" + filter_tools_prompt(registry_.build_tools_prompt());
    }
  }

  std::vector<ChatMessage> messages;
  if (!resume_messages.empty()) {
    // v0.53.48: 续跑——沿用暂停现场(system 已含 prompt,工具链不断档)
    messages = std::move(resume_messages);
  } else {
    messages.push_back(ChatMessage{"system", full_prompt});
    if (!user_query.empty()) messages.push_back(ChatMessage{"user", user_query});
  }

  auto turn_span = AgentTracer::instance().start_span("agent_turn");

  for (int turn = 0; turn < loop_cfg_.max_turns; ++turn) {
    // 上下文压缩
    if (loop_cfg_.enable_compression && turn > 0 &&
        turn > loop_cfg_.compress_after_turns &&
        estimate_chars(messages) > loop_cfg_.max_context_chars) {
      messages = compress_messages(messages);
    }

    // LLM 调用
    nlohmann::json llm_resp;
    if (loop_cfg_.use_native_fc && tools_openai.is_array() && !tools_openai.empty()) {
      llm_resp = call_llm_native(messages, tools_openai);
    } else {
      llm_resp = call_llm_text(messages);
    }

    if (!llm_resp.value("ok", false)) {
      thin_agent::local::HybridRouter local_router;
      auto local_result = local_router.route("chat", user_query, 256);
      if (local_result.ok && !local_result.output.empty()) {
        // v0.52.12: 假绿终结——本地兜底成功时 ok 翻 true 没问题（对
        // 最终用户聊天场景合理），但 spawn 子代理场景的调用方
        // （波次调度）只看 ok 就标 done——兜底文案"我当前处于离线
        // 模式"被当成交付结果落库。改为：兜底生效时 ok 保持 true
        // 但附 degraded 标记，由调用方决定语义；spawn 路径见
        // AgentService 的 degraded 判定。
        result.ok = true;
        result.degraded = true;  // v0.52.12: 降级标记（非原始 LLM 输出）
        result.final_answer = local_result.output;
        result.turns_used = turn + 1;
        // v0.52.8: 上游 LLM 真实错误透传（本地兜底掩盖归因——诊断
        // "为何离线"靠此字段；不影响 ok 语义）
        result.error = "llm_failed(local_fallback): " +
                       llm_resp.value("error", "unknown");
        AgentTracer::instance().end_span(turn_span, "ok");
        return result;
      }
      result.error = "LLM failed at turn " + std::to_string(turn) + ": " +
                     llm_resp.value("error", "unknown");
      AgentTracer::instance().end_span(turn_span, "error");
      return result;
    }

    std::string raw_text = llm_resp.value("text", "");

    // 尝试 native tool_calls 解析
    auto native_tcs = CloudLlmClient::parse_tool_calls(raw_text);
    if (!native_tcs.empty()) {
      // 有原生 tool_calls → 直接执行
      std::vector<ToolCallResult> turn_results;
      for (const auto& ntc : native_tcs) {
        // 人工确认检查（v0.49.0: 统一风险分级框架）
        const auto* schema = registry_.find(ntc.name);
        bool need_hitl = loop_cfg_.human_in_the_loop &&
            (tool_risk_of(ntc.name, ntc.arguments) == ToolRisk::Dangerous ||
             (schema && schema->dangerous));
        if (need_hitl) {
          result.needs_approval = true;
          result.approval_prompt = "将要执行危险操作: " + ntc.name + "\n参数: " + ntc.arguments.dump() + "\n是否继续？";
          result.pending_tool_name = ntc.name;
          result.pending_tool_params = ntc.arguments;
          paused_messages_ = messages;
          paused_tool_name_ = ntc.name;
          paused_tool_params_ = ntc.arguments;
          AgentTracer::instance().end_span(turn_span, "paused");
          return result;
        }

        auto span_id = AgentTracer::instance().start_span("tool_call");
        ToolCallResult cr;
        if (!is_tool_allowed(ntc.name)) {
          cr.tool_name = ntc.name;
          cr.ok = false;
          cr.error = "tool not allowed in current role";
        } else {
          ToolCallResult g = hook_gate(ntc.name, ntc.arguments);
          cr = !g.error.empty() ? g
                               : registry_.call(ntc.name, ntc.arguments,
                                                loop_cfg_.request_timeout_ms);
        }
        AgentTracer::instance().record_tool(span_id, ntc.name, ntc.arguments,
                                             cr.ok ? cr.result : nlohmann::json(cr.error));
        AgentTracer::instance().end_span(span_id, cr.ok ? "ok" : "error");
        result.tool_calls.push_back(cr);
        turn_results.push_back(cr);

        // v0.9.3: 自我纠错 — 检测工具调用异常并重试
        if (ErrorDetector::is_tool_error(cr) && !cr.policy_terminal() &&
            retries_remaining > 0) {  // v0.52.21: 策略终态不重试
          --retries_remaining;
          std::string hint = ErrorDetector::correction_hint(ntc.name, cr);
          messages.push_back(ChatMessage{"user", "[SYSTEM] " + hint});
          continue;  // back to LLM with correction hint
        }
      }

      messages.push_back(ChatMessage{"assistant", raw_text});
      messages.push_back(build_tool_observation(turn_results));
      continue;
    }

    // 文本解析
    nlohmann::json parsed = parse_llm_output(raw_text);
    if (parsed.contains("parse_error")) {
      // v0.53.19: 残缺 JSON 响应体防御（出口②）——parse_error 的常见成因
      // 就是截断的 {"choices":...（GLM 断流），此前直接把 raw_text 当答案
      // 返回 ok=true（h1d 实测：修复①的闸在 L547 只盖了另一出口，本出口
      // 仍然漏）。同样拒绝当答案，视为坏轮重试。
      if (raw_text.size() > 10 && raw_text.compare(0, 10, "{\"choices\":") == 0) {
        AgentTracer::instance().end_span(turn_span, "malformed_json_resp");
        continue;
      }
      result.final_answer = raw_text;
      result.ok = true;
      result.turns_used = turn + 1;
      AgentTracer::instance().end_span(turn_span, "ok");
      return result;
    }

    const auto& tc_json = parsed["tool_calls"];
    bool has_final = parsed.contains("final_answer") &&
                     parsed["final_answer"].is_string() &&
                     !parsed["final_answer"].get<std::string>().empty();

    if (tc_json.is_array() && !tc_json.empty()) {
      std::vector<ToolCallResult> turn_results;
      for (const auto& tc : tc_json) {
        std::string tn = tc.value("name", "");
        nlohmann::json tp = tc.value("params", nlohmann::json::object());

        const auto* schema = registry_.find(tn);
        bool need_hitl = loop_cfg_.human_in_the_loop &&
            (tool_risk_of(tn, tp) == ToolRisk::Dangerous ||
             (schema && schema->dangerous));
        if (need_hitl) {
          result.needs_approval = true;
          result.approval_prompt = "将要执行危险操作: " + tn;
          result.pending_tool_name = tn;
          result.pending_tool_params = tp;
          paused_messages_ = messages;
          paused_tool_name_ = tn;
          paused_tool_params_ = tp;
          AgentTracer::instance().end_span(turn_span, "paused");
          return result;
        }

        auto span_id = AgentTracer::instance().start_span("tool_call");
        ToolCallResult cr;
        if (!is_tool_allowed(tn)) {
          cr.tool_name = tn;
          cr.ok = false;
          cr.error = "tool not allowed in current role";
        } else {
          ToolCallResult g = hook_gate(tn, tp);
          cr = !g.error.empty() ? g
                               : registry_.call(tn, tp,
                                                loop_cfg_.request_timeout_ms);
        }
        AgentTracer::instance().record_tool(span_id, tn, tp,
                                             cr.ok ? cr.result : nlohmann::json(cr.error));
        AgentTracer::instance().end_span(span_id, cr.ok ? "ok" : "error");
        result.tool_calls.push_back(cr);
        turn_results.push_back(cr);

        // v0.9.3: 自我纠错 — 检测工具调用异常并重试
        if (ErrorDetector::is_tool_error(cr) && !cr.policy_terminal() &&
            retries_remaining > 0) {  // v0.52.21: 策略终态不重试
          --retries_remaining;
          std::string hint = ErrorDetector::correction_hint(tn, cr);
          messages.push_back(ChatMessage{"user", "[SYSTEM] " + hint});
          continue;  // back to LLM with correction hint
        }
      }

      messages.push_back(ChatMessage{"assistant", raw_text});
      messages.push_back(build_tool_observation(turn_results));
      continue;
    }

    // v0.53.18: 残缺 JSON 响应体防御——GLM 超时/断流后 raw_text 可能是
    // 截断的 {"choices":...}（parse_tool_calls 解析失败返回空，走"纯文本"
    // 分支被当 final_answer——真任务实测 97s 任务返回原始 JSON 碎片给用户，
    // 且此时 ok=true 构成假绿：任务实际未完成）。识别特征并拒绝当答案。
    if (!has_final && raw_text.size() > 8 &&
        raw_text.compare(0, 10, "{\"choices\":") == 0) {
      AgentTracer::instance().end_span(turn_span, "malformed_json_resp");
      continue;  // 视为坏轮，重试下一轮（耗尽走 turns_exhausted 真实上报）
    }
    result.final_answer = has_final ? parsed["final_answer"].get<std::string>() : raw_text;
    result.ok = true;
    result.turns_used = turn + 1;
    AgentTracer::instance().end_span(turn_span, "ok");
    return result;
  }

  // v0.53.17: turns 耗尽=任务未完成——此前 ok=true+固定"简化问题"文案构成
  // 假绿（真任务实测：写码+编译任务 turns 顶格后，文件已写但未编译，
  // 调用方拿到 ok=True 却只有降级文案，不知实际进度）。修：
  // ①ok=false+degraded（上层调度可见失败）
  // ②文案带实际进度（工具调用数），用户可判断加大 max_turns 重试
  result.final_answer =
      "任务未在 " + std::to_string(loop_cfg_.max_turns) +
      " 轮内完成（已执行 " + std::to_string(result.tool_calls.size()) +
      " 次工具调用）。可用更大的 max_turns 重试，或拆分任务。";
  result.ok = false;
  result.degraded = true;
  result.error = "turns_exhausted";
  result.turns_used = loop_cfg_.max_turns;
  AgentTracer::instance().end_span(turn_span, "turns_exhausted");
  return result;
}

}  // namespace agent
}  // namespace thin_agent
