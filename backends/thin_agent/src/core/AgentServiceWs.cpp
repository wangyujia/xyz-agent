#include "thin_agent/core/FanoutLimits.h"  // v0.54.1: 扇出上限
#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/HookSystem.h"  // v0.52.29
#include "thin_agent/core/slash_commands.h"  // v0.53.21
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
#include <mutex>
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
#include "thin_agent/llm/LlmCircuitBreaker.h"
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

nlohmann::json AgentService::handle_hook_register(const nlohmann::json& req) {
  using HE = thin_agent::HookEvent;
  const std::string event = req.value("event", "");
  HE ev;
  if (!parse_hook_event(event, &ev))
    return {{"type", "error"}, {"message", "未知事件: " + event +
          "（合法值: session_start/end, fc_start/end, tool_pre/post/error, message_in）"}};
  thin_agent::HookRegistration reg;
  reg.event = ev;
  // 双源：shell_cmd（字符串）或 callback="cpp"（进程内回调注册须代码层，
  // WS 通道仅支持 shell——回调注册走 C++ API）
  const std::string shell = req.value("shell_cmd", "");
  const std::string cb = req.value("callback", "");
  if (shell.empty() && cb != "cpp")
    return {{"type", "error"},
            {"message", "须提供 shell_cmd（或 callback=cpp+代码层注册）"}};
  if (!shell.empty()) {
    reg.shell_cmd = shell;
    reg.shell_timeout_ms = req.value("timeout_ms", 5000);
  } else {
    return {{"type", "error"},
            {"message", "WS 通道暂只支持 shell 钩子；C++ 回调请走代码层 API"}};
  }
  if (req.contains("tool_filter") && req["tool_filter"].is_array())
    for (const auto& t : req["tool_filter"])
      if (t.is_string()) reg.tool_filter.push_back(t.get<std::string>());
  const std::string id = thin_agent::HookSystem::instance().register_hook(
      std::move(reg));
  log_event("hook", LogLevel::Info, "registered",
            {{"id", id}, {"event", event}, {"shell", !shell.empty()}});
  return {{"type", "hook_registered"}, {"hook_id", id}, {"event", event}};
}

nlohmann::json AgentService::handle_hook_unregister(const nlohmann::json& req) {
  const std::string id = req.value("hook_id", "");
  if (id.empty()) return {{"type", "error"}, {"message", "缺 hook_id"}};
  const bool removed = thin_agent::HookSystem::instance().unregister_hook(id);
  if (removed) log_event("hook", LogLevel::Info, "unregistered", {{"id", id}});
  return {{"type", "hook_unregistered"}, {"hook_id", id}, {"removed", removed}};
}

nlohmann::json AgentService::handle_hook_list() {
  auto hooks = thin_agent::HookSystem::instance().enumerate();
  return {{"type", "hook_list"}, {"count", hooks.size()}, {"hooks", hooks}};
}

/// 对话主处理流程，见 AgentService.h。
nlohmann::json AgentService::handle_chat(const std::string& session_id,
                                         const std::string& text,
                                         const std::string& cmd_id,
                                         StreamCallback on_chunk,
                                         EventCallback on_event,
                                         const std::string& image_base64) {
  // v0.50.0: 请求级上下文——handle_chat 逐步拆分为私有域方法，各域通过
  // ChatContext& 共享状态（字段语义与拆分前同名局部变量一一对应）。
  ChatContext ctx;
  ctx.session_id = session_id;
  ctx.text = text;
  ctx.cmd_id = cmd_id;
  ctx.on_chunk = on_chunk;
  ctx.on_event = on_event;
  ctx.image_base64 = image_base64;
  // 幂等键后缀：优先用客户端 cmd_id，避免同 session/同 memory 深度误复用旧任务
  ctx.idem_token = !cmd_id.empty()
                       ? cmd_id
                       : ("t" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                                    std::chrono::steady_clock::now().time_since_epoch())
                                                    .count()));

  // v0.29.0: drain pending webhooks — inject as context prefix
  ctx.effective_text = text;
  {
    std::lock_guard<std::mutex> lk(mu_);
    // v0.51.3: 惰性清扫过期孤儿审批（TTL 5min）——原设计只在批准时检查
    // TTL，用户弃之不理则 pending 永久累积（实测验证矩阵触发过孤儿场景）。
    // 挂在 chat 入口：任何新请求进来时顺带清理，零新线程零定时器。
    if (!fc_pending_approvals_.empty()) {
      const auto now = std::chrono::steady_clock::now();
      for (auto it = fc_pending_approvals_.begin();
           it != fc_pending_approvals_.end();) {
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.created_at).count();
        if (age > PendingApproval::kTimeoutSec) {
          log_event("hitl", LogLevel::Info, "orphan approval expired, purged",
                    {{"session", it->first}, {"tool", it->second.tool_name},
                     {"age_sec", age}});
          // v0.53.81: 孤儿审批=run 的第三出口——一并收敛终态（否则
          // fc_runs 永久 running，重启后被改判 interrupted 触发幽灵续跑）
          finalize_fc_run_locked(it->first, "expired");
          it = fc_pending_approvals_.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (!pending_webhooks_.empty()) {
      std::ostringstream wh_prefix;
      wh_prefix << "[系统通知] 在您响应用户消息期间，收到了以下外部事件：\n";
      for (const auto& wh : pending_webhooks_) {
        wh_prefix << "- [" << wh.source << "] " << wh.event << ": " << wh.payload << "\n";
      }
      wh_prefix << "请根据这些事件判断是否需要调整您的响应。\n---\n用户消息：";
      ctx.effective_text = wh_prefix.str() + text;
      pending_webhooks_.clear();
    }
  }

  // v0.52.29: message_in 钩子——用户消息入站（改写前原文）
  HookSystem::instance().dispatch(
      HookEvent::MessageIn,
      {{"session", session_id}, {"text", text}, {"effective_len",
                                                 ctx.effective_text.size()}});

  // v0.52.26: 断点续跑——检测 resume 意图（"继续/恢复"+存在 interrupted run）
  // 时把 journal 快照注入会话记忆前缀，FC 循环从快照续跑而非从零重来。
  // 未命中 resume 语义则零开销直通。持久化审批的恢复推送也在此处
  // （重启后内存 pending 为空，journal 里有则重建）。
  if (fc_run_store_) {
    FcRun rr = fc_run_store_->load_resumable(session_id);
    const bool wants_resume =
        ctx.effective_text.find("继续") != std::string::npos ||
        ctx.effective_text.find("恢复") != std::string::npos;
    if (!rr.run_id.empty()) {
      if (wants_resume) {
        // 重建：journal 快照 + 用户新指令
        std::vector<ChatMessage> jmsgs;
        if (rr.messages.is_array()) {
          for (const auto& jm : rr.messages) {
            ChatMessage m;
            m.role = jm.value("role", "");
            m.content = jm.value("content", "");
            m.tool_call_id = jm.value("tool_call_id", "");
            m.name = jm.value("name", "");
            if (jm.contains("tool_calls")) m.tool_calls = jm["tool_calls"];
            jmsgs.push_back(std::move(m));
          }
        }
        std::lock_guard<std::mutex> lk(mu_);
        auto& mem = session_chat_memory_[session_id];
        // 快照覆盖现有记忆（interrupted run 的记录比当前残缺记忆更完整），
        // 再补用户原始诉求 + 本次续跑指令
        mem = jmsgs;
        if (!rr.user_text.empty())
          mem.insert(mem.begin(), ChatMessage{"user", rr.user_text});
        mem.push_back(ChatMessage{"user", ctx.effective_text +
            "（系统注：上次任务因服务重启中断，以上是中断前的完整进度，"
            "请基于进度继续，不要重做已完成步骤）"});
        log_event("fc-journal", LogLevel::Info, "resume run",
                  {{"run", rr.run_id}, {"snapshot_msgs", jmsgs.size()},
                   {"iter_used", rr.iter}});
      } else {
        // 有可恢复 run 但用户没说继续——轻提示（不劫持路由）
        ctx.effective_text += "\n[系统注：检测到上次中断的任务可恢复，"
                              "回复\"继续\"可断点续跑]";
      }
    }
    // 重启后持久化审批恢复（内存 pending 为空时才重建，避免重复）
    {
      std::lock_guard<std::mutex> lk(mu_);
      bool has_pending = fc_pending_approvals_.count(session_id) > 0;
      if (!has_pending) {
        FcPendingApproval jpa = fc_run_store_->take_approval(session_id);
        if (!jpa.run_id.empty()) {
          PendingApproval npa;
          npa.tool_name = jpa.tool_name;
          npa.tool_args = jpa.tool_args;
          npa.fc_iterations_used = jpa.fc_iterations_used;
          npa.created_at = std::chrono::steady_clock::now();
          npa.user_text = jpa.user_text;
          fc_pending_approvals_[session_id] = std::move(npa);
          log_event("fc-journal", LogLevel::Info,
                    "restored persisted approval", {{"tool", jpa.tool_name}});
        }
      }
    }
    // journal 审批过期清理（与内存版同 TTL）
    fc_run_store_->purge_expired_approvals(PendingApproval::kTimeoutSec);
  }

  {
    std::lock_guard<std::mutex> lk(mu_);
    auto& mem = session_chat_memory_[session_id];
    mem.push_back(ChatMessage{"user", ctx.effective_text});
    persist_session_msg(session_id, mem.back());  // v0.53.28 持久化
    // Token 预算裁剪（替代旧 memory_window_ 固定条数）
    trim_memory_by_token_budget(mem,
        static_cast<size_t>(ctx_cfg_.history_budget_tokens),
        ctx_cfg_.full_context_turns,
        static_cast<size_t>(ctx_cfg_.truncated_max_chars));
    ctx.snapshot = mem;
    // v0.53.77: 已在上方 L252 mu_ 锁内——裸调用即安全(此前误补锁
    /// 造成同线程二次 lock=自死锁,lifecycle 挂死实测定位)
    record_event("chat:" + session_id);
  }

  // Helper: 将 snapshot (ChatMessage) 转为 JSON 数组（向后兼容）
  auto snapshot_json = [&]() {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : ctx.snapshot) arr.push_back(t.content);
    return arr;
  };

  // 摘要：跟踪对话消息，超阈值时自动压缩
  if (summarizer_) {
    summarizer_->add_message("user", text);
    if (summarizer_->needs_summarize()) {
      std::string summary = summarizer_->summarize();
      if (!summary.empty()) {
        log_event("summarizer", LogLevel::Info, "auto-summarized",
                  {{"msgs_kept", summarizer_->message_count()},
                   {"summary_chars", summary.size()}});
      }
    }
  }

  append_long_term_memory(session_id, text);

  ctx.dialog_ctx = snapshot_dialog_context(session_id);
  tick_dialog_context(session_id);

  ctx.text_lower = to_lower_copy(text);
  ctx.query_lang = detect_query_language(text);
  ctx.tpl_lang = pick_template_lang(ctx.query_lang, text);
  ctx.use_zh_progress = is_simplified_chinese(text);  // 进度/动作消息语言选择

  // 立刻推送第一条进度消息，让用户感知到系统正在处理
  chat_push_thinking(ctx, 1, "local_analysis");

  // ── 域 A: 输入翻译（非简体中文/非英文 → 翻译成英文，用于关键词匹配和快速路由）──
  chat_translate_input(ctx);

  // ── 域 B: 意图分类三级级联 + 槽位续接（命中续接则直接返回）──
  if (nlohmann::json slot_or_null = chat_classify_intent(ctx); !slot_or_null.is_null()) {
    return slot_or_null;
  }

  const double kExecuteThreshold = intent_execute_threshold();
  const double kClarifyThreshold = intent_clarify_threshold();


  // v0.50.0: 媒体关键词确定性路由保留位（原域 B 内局部变量，媒体域消费）
  const bool media_keyword_reserved =
      contains_any_policy(ctx.text_lower, "task_capture_inline") ||
      contains_any_policy(ctx.text_lower, "action_capture") ||
      contains_any_policy(ctx.text_lower, "action_start_recording") ||
      contains_any_policy(ctx.text_lower, "task_start_recording_inline");
  const bool reserved_for_deterministic_route = media_keyword_reserved;

  ctx.model_status_query = is_model_runtime_query(ctx.text_lower);
  ctx.classified_intent = ctx.intent_info.value("intent", "");  // mutable for Tier 2 re-classify

  // 配置驱动的 clarify 意图（short_ack / messaging_capability 等）
  if (!ctx.force_cloud_for_complex) {
    for (const auto& clarify_name : intent_names_by_kind("clarify")) {
      IntentSpec clarify_spec;
      if (!load_intent_spec(clarify_name, &clarify_spec) || clarify_spec.kind != "clarify") continue;
      if (!intent_triggered(clarify_spec, ctx.classified_intent, text, ctx.text_lower)) continue;
      const std::string label =
          clarify_spec.intent_label.empty() ? clarify_spec.name : clarify_spec.intent_label;
      return make_local_reply(ctx, 
          clarify_spec.route_execute.empty() ? "local_clarify" : clarify_spec.route_execute,
          clarify_spec.reason_execute.empty() ? clarify_spec.name + "_clarify" : clarify_spec.reason_execute,
          policy_text(clarify_spec.clarify_template_key, ""),
          nlohmann::json::object(),
          nlohmann::json::array(),
          label,
          clarify_spec.default_confidence,
          nlohmann::json::object(),
          "clarify");
    }
  }

  // ── 域 C: 本地意图路由（profile/status/event/memory_*/特殊 predicate）──
  if (nlohmann::json local_hit = chat_route_local_intents(ctx); !local_hit.is_null()) {
    return local_hit;
  }

  // v0.50.0: status 建议句式（外部 clarify 域消费；域 C 内有独立计算）
  const bool status_advice_like = contains_any_policy(ctx.text_lower, "status_advice_suggest") &&
                                  contains_any_policy(ctx.text_lower, "status_advice_status") &&
                                  contains_any_policy(ctx.text_lower, "status_advice_task_exec");

  // v0.50.0: 泛信息查询句式（云端策略段消费；域 D 内有独立计算）
  const bool general_info_query_like =
      contains_any_policy(ctx.text_lower, "general_info_query_include") &&
      !contains_any_policy(ctx.text_lower, "general_info_query_exclude");

  // ── 域 D: 外部意图路由（weather/news/search + external_clarify）──
  if (nlohmann::json ext_hit = chat_route_external_intents(ctx); !ext_hit.is_null()) {
    return ext_hit;
  }

  // ── 域 E: 媒体计划路由（问令门控 + pipeline 执行）──
  if (nlohmann::json media_hit = chat_route_media_plan(ctx); !media_hit.is_null()) {
    return media_hit;
  }

  // ── 域 F: 任务/动作/gate 意图路由 ──
  if (nlohmann::json tag_hit = chat_route_task_action_gate(ctx); !tag_hit.is_null()) {
    return tag_hit;
  }

  // ── 域 G1: 云前置（budget gate + key 解析 + offline/missing/auto 级联）──
  std::string provider;
  std::string key_state;
  std::string api_key;
  if (nlohmann::json pre_hit = chat_cloud_precheck(ctx, provider, key_state, api_key);
      !pre_hit.is_null()) {
    return pre_hit;
  }

  // ── 域 G2: 云端执行（tools schema + fast FC + 主 FC + 后处理）──
  AgentService::ChatCloudResult cloud_out = chat_run_cloud(ctx, api_key, key_state, provider);
  if (!cloud_out.final.is_null()) return cloud_out.final;
  if (cloud_out.need_fallback) {
    // ── 域 I: 云失败离线回退（原 cloud_fallback 标签段）──
    return chat_offline_fallback(ctx, cloud_out.cloud, key_state, provider);
  }
  const CloudChatResult cloud = cloud_out.cloud;

  // ── 域 H: 云策略 pipeline 路由（strategy JSON 处理 + 落地）──
  if (nlohmann::json strategy_hit = chat_route_cloud_strategy(ctx, cloud, key_state, provider);
      !strategy_hit.is_null()) {
    return strategy_hit;
  }

  // v0.53.13: 兜底 return——此前所有路由域未中时控制流走至函数尾（UB，
  // Release -Wreturn-type 实证）。返回显式 null 帧（调用侧 finish 自行包装）。
  return nlohmann::json::object();
}

nlohmann::json AgentService::handle_action(const std::string& action, const nlohmann::json& args) const {
  if (!executor_) {
    return {{"type", "error"}, {"message", "executor not ready"}};
  }

  Kv kv;
  if (args.is_object()) {
    for (auto it = args.begin(); it != args.end(); ++it) {
      if (it.value().is_string()) kv[it.key()] = it.value().get<std::string>();
      else kv[it.key()] = it.value().dump();
    }
  }

  Result r = executor_->execute(action, kv, 1000);
  nlohmann::json data = nlohmann::json::object();
  for (const auto& [k, v] : r.data) data[k] = v;

  return {
      {"type", "action_result"},
      {"action", action},
      {"result", {{"code", r.code}, {"message", r.message}, {"data", data}}},
  };
}

nlohmann::json AgentService::handle_task_submit(const nlohmann::json& req) {
  if (!task_engine_) {
    return {{"type", "error"}, {"message", "task_engine not ready"}};
  }

  const std::string action = req.value("action", "");
  const nlohmann::json args = req.contains("args") ? req["args"] : nlohmann::json::object();
  const std::string idem = req.value("idempotency_key", "");

  std::string task_id = task_engine_->submit_task(action, args, idem);
  auto task = task_engine_->get_task(task_id);

  return {
      {"type", "task_submit_result"},
      {"task", task},
  };
}

nlohmann::json AgentService::handle_task_cancel(const nlohmann::json& req) {
  if (!task_engine_) return {{"type", "error"}, {"message", "task_engine not ready"}};
  const std::string task_id = req.value("task_id", "");
  const std::string reason = req.value("reason", "cancelled by user");
  return {{"type", "task_cancel_result"}, {"task", task_engine_->cancel_task(task_id, reason)}};
}

nlohmann::json AgentService::handle_task_replay(const nlohmann::json& req) {
  if (!task_engine_) return {{"type", "error"}, {"message", "task_engine not ready"}};
  const std::string task_id = req.value("task_id", "");
  const std::string idem = req.value("idempotency_key", "");
  return {{"type", "task_replay_result"}, {"data", task_engine_->replay_task(task_id, idem)}};
}

nlohmann::json AgentService::handle_task_audit(const nlohmann::json& req) {
  if (!task_engine_) return {{"type", "error"}, {"message", "task_engine not ready"}};
  const std::string task_id = req.value("task_id", "");
  const int limit = json_coerce_int(req, "limit", 50);
  return {{"type", "task_audit_result"}, {"data", task_engine_->list_task_audits(task_id, limit)}};
}

nlohmann::json AgentService::handle_chat_approve(const std::string& session_id,
                                           const nlohmann::json& req) {
  const bool approved = req.value("approved", false);
  nlohmann::json result;

  // ── v0.49.0: FC 循环审批路径 ─────────────────────────
  {
    std::unique_lock<std::mutex> lk(mu_);
    auto fc_it = fc_pending_approvals_.find(session_id);
    if (fc_it != fc_pending_approvals_.end()) {
      auto pa = std::move(fc_it->second);
      fc_pending_approvals_.erase(fc_it);

      // P1-1: TTL 过期检查（5 分钟）
      auto age = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - pa.created_at).count();
      if (age > PendingApproval::kTimeoutSec) {
        lk.unlock();
        log_event("hitl", LogLevel::Warn, "FC approval expired",
                  {{"age_sec", age}, {"tool", pa.tool_name}});
        return {
            {"type", "chat_result"},
            {"text", "审批已超时（" + std::to_string(age) + " 秒），操作未执行。请重新发起指令。"},
            {"decision", {{"route", "fc_loop"}, {"reason", "hitl_expired"}}},
        };
      }
      lk.unlock();

      log_event("hitl", LogLevel::Info, "FC approval",
                {{"result", approved ? "granted" : "denied"},
                 {"tool", pa.tool_name}, {"session", session_id}});

      if (!approved) {
        // v0.52.26: 审批已处理（拒绝）——清 journal 审批残留（TTL 0=全清）
        if (fc_run_store_) fc_run_store_->purge_expired_approvals(0);
        // v0.53.81: 拒绝=run 的第二出口——收敛终态（锁在此处已释放）
        {
          std::lock_guard<std::mutex> flk(mu_);
          finalize_fc_run_locked(session_id, "denied");
        }
        return {
            {"type", "chat_result"},
                {"text", "已取消危险操作: " + pa.tool_name + "。可以继续其他指令。"},
            {"decision", {{"route", "fc_loop"}, {"reason", "hitl_denied"}}},
        };
      }

      // 批准 → 单独执行该工具，然后带工具结果续跑 FC 循环（v0.52.2 方案 C：
      // 原语义"只执行不续跑"导致多步任务每步都要用户再发"继续"驱动，
      // 且 continue 话术不当时模型反问/重试，实测一个任务 3-5 次干预）。
      nlohmann::json args = nlohmann::json::parse(pa.tool_args, nullptr, false);
      CloudLlmClient::ParsedToolCall tc;
      tc.name = pa.tool_name;
      tc.arguments = args.is_object() ? args : nlohmann::json::object();

      // 执行（此线程 TLS 上下文已由 handle_request 注入；
      // 锁已在上方 TTL 检查后释放——v0.50.2 语义，工具执行+续跑全程锁外）
      ToolExecResult tr;
      std::string exec_err;
      try {
        tr = execute_one_tool(tc, skill_registry_, nullptr, true);
      } catch (const std::exception& e) {
        exec_err = e.what();
        tr.success = false;
        tr.output = "工具执行异常: " + exec_err;
      }

      std::ostringstream oss;
      oss << "✅ 已执行: " << pa.tool_name << "\n\n结果:\n"
          << utf8_truncate(tr.output, 2000);
      if (!tr.success) oss << "\n\n(工具执行失败)";

      // ── v0.52.2: 续跑 FC 循环（快照 + 工具结果）────────────
      if (!pa.fc_messages.empty()) {
        log_event("hitl", LogLevel::Info, "FC resume after approval",
                  {{"tool", pa.tool_name}, {"msgs", pa.fc_messages.size()}});

        std::vector<ChatMessage> resume_msgs = pa.fc_messages;
        // 工具结果作为 tool 消息 append（协议：与 assistant tool_calls 配对）
        ChatMessage tool_msg;
        tool_msg.role = "tool";
        tool_msg.tool_call_id = tc.id.empty() ? "call_resume_0" : tc.id;
        tool_msg.name = tc.name;
        tool_msg.content = tr.output.empty() ? "(无输出)" : tr.output;
        resume_msgs.push_back(std::move(tool_msg));

        // 重建续跑上下文（与主路径同源：tools schema / key / 审批回调）
        const nlohmann::json tools_schema = skill_registry_.build_tools_schema();
        const std::string api_key = resolve_api_key(
            credential_pool_.get(), cfg_.api_key_env, cfg_.provider);
        auto resume_approval_cb = [this, session_id](
            const std::string& tool_name, const nlohmann::json& tool_args) -> bool {
          // 同款审批回调（含 v0.52.1 cron 无人值守拒绝）
          if (!g_cron_filter.is_null()) {
            log_event("hitl", LogLevel::Warn,
                      "cron dangerous tool auto-denied (unattended)",
                      {{"tool", tool_name}, {"session", session_id}});
            g_fc_dangerous_denied = true;
            return false;
          }
          PendingApproval npa;
          npa.tool_name = tool_name;
          npa.tool_args = tool_args.dump();
          npa.created_at = std::chrono::steady_clock::now();
          npa.user_text = "（续跑中触发）" + tool_name;
          {
            std::lock_guard<std::mutex> lk2(mu_);
            fc_pending_approvals_[session_id] = std::move(npa);
          }
          log_event("hitl", LogLevel::Warn, "FC paused, dangerous tool",
                    {{"tool", tool_name}, {"session", session_id}});
          return false;
        };

        const int remaining_iters = std::max(
            1, 30 - pa.fc_iterations_used);  // 迭代预算=原上限-已用
        auto resumed = run_function_calling_loop(
            cfg_, api_key, resume_msgs, tools_schema,
            session_id, "", nullptr, skill_registry_,
            1, remaining_iters, true, nullptr, nullptr, resume_approval_cb);

        // v0.52.2: 续跑中二次暂停 — 快照回填 pending（与主路径同款），
        // 下一次 approve 仍可续跑（多级危险任务不降级为单工具语义）
        if (resumed.needs_approval && !resumed.fc_messages_snapshot.empty()) {
          std::lock_guard<std::mutex> lk2(mu_);
          auto pit2 = fc_pending_approvals_.find(session_id);
          if (pit2 != fc_pending_approvals_.end()) {
            pit2->second.fc_messages = resumed.fc_messages_snapshot;
            pit2->second.fc_iterations_used =
                pa.fc_iterations_used + resumed.fc_iterations_used;
          }
          return {
              {"type", "chat_result"},
              {"text", oss.str() + "\n\n⏸️ 续跑中触发新的危险操作，等待确认。"},
              {"decision", {{"route", "fc_loop"}, {"reason", "hitl_approved_resumed_pending"}}},
          };
        }
        // v0.53.81: 续跑收敛——未再触发审批即本次 run 终止（第一出口；
        // 此前无人 finish_run，fc_runs 滞留 running）
        if (!resumed.needs_approval) {
          std::lock_guard<std::mutex> flk(mu_);
          finalize_fc_run_locked(session_id, resumed.ok ? "done" : "failed");
        }
        if (resumed.ok && !resumed.text.empty()) {
          return {
              {"type", "chat_result"},
              {"text", oss.str() + "\n\n── 续跑结果 ──\n" + resumed.text},
              {"decision", {{"route", "fc_loop"}, {"reason", "hitl_approved_resumed"}}},
              {"tool_calls", nlohmann::json::array({{{"tool", pa.tool_name}}})},
          };
        }
        // 续跑失败（网络/二次暂停无快照等）→ 回退原语义（只报工具结果）
        return {
            {"type", "chat_result"},
            {"text", oss.str() + "\n\n（续跑未完成：" +
                 (resumed.error.empty() ? "无最终文本" : resumed.error) + "，可发\"继续\"驱动）"},
            {"decision", {{"route", "fc_loop"}, {"reason", "hitl_approved_resume_failed"}}},
            {"tool_calls", nlohmann::json::array({{{"tool", pa.tool_name}}})},
        };
      }

      // ── 无快照（旧暂停记录/其他来源）：维持原单工具语义 ──
      return {
          {"type", "chat_result"},
          {"text", oss.str()},
          {"decision", {{"route", "fc_loop"}, {"reason", "hitl_approved"}}},
          {"tool_calls", nlohmann::json::array({{{"tool", pa.tool_name}}})},
      };
    }
  }

  // ── 既有 AgentLoop 审批路径（子 Agent HITL）────────────
  // v0.50.2: 锁内只做查找+出队，continue_after_approval（含工具执行与
  // AgentLoop 续跑，秒级以上）移到锁外——此前全程持全局 mu_，审批一次
  // 阻塞所有 session 的 chat/memory 操作
  {
    std::shared_ptr<agent::AgentLoop> loop_ptr;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = paused_approvals_.find(session_id);
      if (it == paused_approvals_.end()) {
        result = {
            {"type", "chat_result"},
            {"mode_used", "agent-loop"},
            {"intent_backend", "cloud"},
            {"text", "没有待审批的操作，可能已超时。请重新发送指令。"},
            {"decision", {
                {"route", "agent_loop"},
                {"reason", "no_pending_approval"},
                {"policy", "clarify"}
            }}
        };
        return result;
      }

      // v0.53.48 标注:本恢复路径当前【不可达】——spawn 路径硬编码
      /// human_in_the_loop=false(AgentServiceTools L812),子代理从不
      /// 暂停,paused_approvals_ 无写入方。保留:未来启用子代理 HITL
      /// 时必须同时补 spawn handler 的 needs_approval→emplace 存链,
      /// 否则暂停即死路(检视 R37 定性:断链休眠非活 bug)
      loop_ptr = std::move(it->second);
      paused_approvals_.erase(it);
    }

    auto loop_result = loop_ptr->continue_after_approval(approved);

    {
      std::lock_guard<std::mutex> lk(mu_);
      result = {
        {"type", "chat_result"},
        {"mode_used", "agent-loop"},
        {"intent_backend", "cloud"},
        {"memory_size", static_cast<int>(session_chat_memory_[session_id].size())},
        {"decision", {
            {"route", "agent_loop"},
            {"reason", approved ? "hitl_approved" : "hitl_cancelled"},
            {"policy", "execute"}
        }},
        {"tool_calls", nlohmann::json::array({{{"tool", "cloud_llm"}}})},
        {"agent_loop_turns", loop_result.turns_used}
    };

    if (loop_result.ok) {
      result["text"] = loop_result.final_answer;
      // v0.53.41: ingest 上收到 worker 尾部异步(ws_agent_main 统一路径,
      /// 此处同步 ingest 的云端 embedding 延迟直接加在回复时延上)
    } else {
      result["text"] = loop_result.error.empty()
          ? "Agent 处理失败，请重试。"
          : loop_result.error;
    }
    }
  }

  return result;
}

/// v0.52.20: spawn 嵌套深度（全局 thread_local——三条 spawn 链共用：
/// orch_spawn/ToolRegistry 版/SkillRegistry 版。波次每任务独立线程=
/// 顶层从 0 计）。
// v0.53.1: 嵌套深度改全局原子——thread_local 在跨线程嵌套链（子代理
// loop 在 async 线程跑）下每层归零，护栏失效（e2e 实证：mock 嵌套链
// 打满 16 active 上限而非 depth 3 拦截）。全局计数对真实威胁（无界
// 递归）语义不变：深度仍按"进行中的嵌套链层数"计。

/// v0.52.21: 进程级活跃子代理总数上限——深度护栏挡不住【广度】爆炸
///（病态 LLM 恒返 spawn 调用时：3 层合法深度内 6×8×8 乘积=数百并发
/// 子代理，ASAN 实测 T1801 线程+OOM。原子计数进/出，超限快速拒绝。

/// v0.49.1: agent 编排族 WS API（从 handle_request 拆出，内容零变化）
/// 覆盖 spawn_agent/agent_message/agent_inbox*/agent_broadcast/bb_*/kanban_*/
/// agent_debate/role_*/orchestrate/orch_status/agent_dag/agent_decompose*/agent_synthesize
/// @return 匹配并处理的响应；无匹配分支时返回空 json（调用方继续尝试其他 type）
/// v0.50.4: 子 Agent 生成域（spawn_agent）。
nlohmann::json AgentService::slash_command(const std::string& session_id,
                                          const std::string& text) {
  // v0.53.21: 解析与词表见 slash_commands.h（默认表内置，chat_policy
  // slash_commands.aliases 覆盖；enabled=false 时清表=功能关闭）
  static std::map<std::string, std::string> cfg_alias;
  static bool cfg_loaded = false;
  if (!cfg_loaded) {
    cfg_alias = slash_alias_defaults();
    try {
      const auto& pol = chat_policy();
      if (pol.contains("slash_commands")) {
        if (!pol["slash_commands"].value("enabled", true)) {
          cfg_alias.clear();
        } else if (pol["slash_commands"].contains("aliases")) {
          for (auto it2 = pol["slash_commands"]["aliases"].begin();
               it2 != pol["slash_commands"]["aliases"].end(); ++it2) {
            cfg_alias[it2.key()] = it2.key();
            for (const auto& a : it2.value())
              if (a.is_string()) cfg_alias[a.get<std::string>()] = it2.key();
          }
        }
      }
    } catch (...) { /* 配置异常用默认 */ }
    cfg_loaded = true;
  }
  const std::string canon = slash_parse(text, cfg_alias);
  if (canon.empty()) return nullptr;

  if (canon == "/new" || canon == "/reset") {
    size_t cleared = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto mit = session_chat_memory_.find(session_id);
      if (mit != session_chat_memory_.end()) {
        cleared = mit->second.size();
        mit->second.clear();  // 清对话记忆（session 保留——连接不断）
      }
      slot_states_.erase(session_id);
      dialog_states_.erase(session_id);
      session_projects_.erase(session_id);
      fc_pending_approvals_.erase(session_id);
    }
    drop_session_file(session_id);  // v0.53.28: 持久化文件同步清理（新起点）
    if (canon == "/reset") {
      // /reset 额外复位运行态：熔断器+限流（排障场景——GLM 容量窗口
      // 触发熔断后秒拒，reset 立即恢复可用）
      LlmCircuitBreaker::instance().reset();
    }
    return {
        {"type", "chat_result"}, {"cmd_acked", canon}, {"ok", true},
        {"cleared_messages", cleared},
        {"text", canon == "/reset"
             ? "已重置会话并复位运行态（熔断/限流）。上下文已清空。"
             : "已开启新会话。上下文已清空。"}};
  }
  if (canon == "/clear") {
    size_t cleared = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto mit = session_chat_memory_.find(session_id);
      if (mit != session_chat_memory_.end()) {
        cleared = mit->second.size();
        mit->second.clear();
      }
    }
    drop_session_file(session_id);  // v0.53.28: 持久化文件同步清理
    return {{"type", "chat_result"}, {"cmd_acked", "/clear"}, {"ok", true},
            {"cleared_messages", cleared},
            {"text", "已清空对话历史（项目上下文与槽位保留）。"}};
  }
  if (canon == "/stop") {
    // v0.53.22: 中断跑中的 chat/spawn——abort_chat 置位 aborted_sessions_，
    // FC 循环每轮检查点退出（v0.47.3 基建）；队列中未起跑的任务自然作废
    abort_chat(session_id);
    return {{"type", "chat_result"}, {"cmd_acked", "/stop"}, {"ok", true},
            {"text", "已请求中断当前会话的进行中任务（协作式——当前轮完成后停止）。"}};
  }
  if (canon == "/model") {
    // 只读快照：当前主链+fast 通道+冗余池（切换属运行时热改配置，C 档/专项）
    nlohmann::json pool = nlohmann::json::array();
    for (const auto& p : cfg_.cloud_providers)
      pool.push_back({{"provider", p.provider}, {"model", p.model_name}});
    const auto& pol = chat_policy();
    return {
        {"type", "chat_result"}, {"cmd_acked", "/model"}, {"ok", true},
        {"current", {{"provider", cfg_.provider}, {"model", cfg_.model_name}}},
        {"fast_path",
         {{"model", pol.value("fast_path", nlohmann::json::object())
                        .value("model", "")},
          {"timeout_ms", pol.value("fast_path", nlohmann::json::object())
                              .value("timeout_ms", 0)}}},
        {"pool", pool},
        {"text", "模型快照（切换功能规划中）"}};
  }
  if (canon == "/tools") {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : agent::ToolRegistry::instance().all_tools())
      arr.push_back({{"name", t.name}, {"desc", t.description}});
    return {{"type", "chat_result"}, {"cmd_acked", "/tools"}, {"ok", true},
            {"count", arr.size()}, {"tools", arr},
            {"text", "工具清单 " + std::to_string(arr.size()) + " 项"}};
  }
  if (canon == "/sessions") {
    // C 档：活跃会话列表（核心侧视角——记忆条数+是否有项目上下文）。
    // 注：会话身份由 chat_id:thread_id 派生（宿主层），切换=客户端换
    // chat_id 发消息即可，无需 /switch 命令。
    nlohmann::json arr = nlohmann::json::array();
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (const auto& [sid, mem] : session_chat_memory_) {
        arr.push_back({{"session", sid},
                       {"messages", mem.size()},
                       {"project_ctx", session_projects_.count(sid) > 0}});
      }
    }
    return {{"type", "chat_result"}, {"cmd_acked", "/sessions"}, {"ok", true},
            {"count", arr.size()}, {"sessions", arr},
            {"text", "活跃会话 " + std::to_string(arr.size()) +
                         " 个（切换：用对应 chat_id 发消息）"}};
  }
  if (canon == "/retry") {
    // C 档：弹出最后一条 user 消息交还重放。slash_command 无重发通道——
    // 返回 __retry_text__ 标记，拦截层把 chat text 替换后继续走正常流程。
    std::string last_user;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto mit = session_chat_memory_.find(session_id);
      if (mit != session_chat_memory_.end()) {
        auto& mem = mit->second;
        for (auto it2 = mem.rbegin(); it2 != mem.rend(); ++it2) {
          if (it2->role == "user") {
            last_user = it2->content;
            mem.erase(std::next(it2).base());
            break;
          }
        }
      }
    }
    if (last_user.empty()) {
      return {{"type", "chat_result"}, {"cmd_acked", "/retry"}, {"ok", false},
              {"text", "没有可重试的用户消息。"}};
    }
    // 标记重放（拦截层识别后改写 text 走 chat）
    return {{"type", "chat_result"}, {"cmd_acked", "/retry"}, {"ok", true},
            {"__retry_text__", last_user},
            {"text", "重试上一条消息（" + std::to_string(last_user.size()) +
                         " 字符）"}};
  }
  if (canon == "/compact") {
    // C 档：把当前会话记忆压成 LLM 摘要（保留近 4 条原文+其余摘要）。
    // 唯一碰 LLM 的斜杠命令——耗时=一次 chat_completion（非 101ms 级）。
    std::vector<ChatMessage> mem;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto mit = session_chat_memory_.find(session_id);
      if (mit != session_chat_memory_.end()) mem = mit->second;
    }
    if (mem.size() <= 4) {
      return {{"type", "chat_result"}, {"cmd_acked", "/compact"}, {"ok", true},
              {"text", "消息数 " + std::to_string(mem.size()) +
                           " 条（≤4），无需压缩。"}};
    }
    // 摘要请求：前段压缩（保留尾部 4 条）
    std::string transcript;
    for (size_t i = 0; i + 4 < mem.size(); ++i)
      transcript += mem[i].role + ": " + mem[i].content + "\n";
    const size_t kMaxTranscript = 12000;
    if (transcript.size() > kMaxTranscript)
      transcript = transcript.substr(transcript.size() - kMaxTranscript);
    std::vector<ChatMessage> msgs = {
        {"user", "把以下对话历史压缩成简洁摘要（保留任务上下文、关键决定、"
                 "文件路径与代码要点，300 字内）：\n" + transcript}};
    const std::string api_key_compact =
        resolve_api_key(credential_pool_.get(), cfg_.api_key_env, cfg_.provider);
    CurlHttpClient http(cfg_.request_timeout_ms);
    auto r = CloudLlmClient::chat_completion(http, cfg_, api_key_compact, msgs);
    if (!r.ok || r.text.empty()) {
      return {{"type", "chat_result"}, {"cmd_acked", "/compact"}, {"ok", false},
              {"error", r.error},
              {"text", "压缩失败（LLM 不可用）：" + r.error}};
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto& mem2 = session_chat_memory_[session_id];
      std::vector<ChatMessage> keep(mem2.end() - 4, mem2.end());
      mem2.clear();
      mem2.push_back(ChatMessage{
          "assistant", "[上下文摘要] " + r.text});
      mem2.insert(mem2.end(), keep.begin(), keep.end());
    }
    return {{"type", "chat_result"}, {"cmd_acked", "/compact"}, {"ok", true},
            {"compressed_from", mem.size()},
            {"text", "已压缩 " + std::to_string(mem.size() - 4) +
                         " 条历史为摘要（保留最近 4 条原文）。"}};
  }
  if (canon == "/help") {
    // v0.53.24: 文案全走 chat_policy descriptions（铁律：用户可见文案
    // 外置；内置仅兜底）——命令集增减只改配置
    static const std::string kFallback =
        "斜杠命令：/new 新会话 | /reset 重置+复位熔断 | /clear 清历史 | "
        "/stop 中断任务 | /model 模型快照 | /tools 工具清单 | /sessions "
        "会话列表 | /compact 压缩上下文 | /retry 重试上条 | /status 状态";
    std::string help = kFallback;
    try {
      const auto& pol = chat_policy();
      if (pol.contains("slash_commands") &&
          pol["slash_commands"].contains("descriptions") &&
          pol["slash_commands"]["descriptions"].is_object() &&
          !pol["slash_commands"]["descriptions"].empty()) {
        std::string joined;
        for (auto it2 = pol["slash_commands"]["descriptions"].begin();
             it2 != pol["slash_commands"]["descriptions"].end(); ++it2) {
          if (!joined.empty()) joined += " | ";
          joined += it2.key() + " " + it2.value().get<std::string>();
        }
        help = "斜杠命令：" + joined;
      }
    } catch (...) { /* 配置异常用兜底 */ }
    return {{"type", "chat_result"}, {"cmd_acked", "/help"}, {"ok", true},
            {"text", help}};
  }
  // /status
  size_t mem_size = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto mit = session_chat_memory_.find(session_id);
    if (mit != session_chat_memory_.end()) mem_size = mit->second.size();
  }
  nlohmann::json st = {{"type", "chat_result"}, {"cmd_acked", "/status"}, {"ok", true},
                       {"session_id", session_id},
                       {"memory_messages", mem_size},
                       {"has_project_ctx", session_projects_.count(session_id) > 0}};
  return st;
}

/// v0.49.1: agent 编排族 WS API（从 handle_request 拆出，内容零变化）
nlohmann::json AgentService::orch_spawn(const std::string& session_id,
                                        const std::string& type,
                                        const nlohmann::json& req) {
  if (type != "spawn_agent") return nullptr;
  if (type == "spawn_agent") {
    // v0.52.20: 嵌套深度护栏——子代理可再 spawn 子代理（无界递归
    // 风险：线程/token 双爆）。thread_local 计数：波次每个任务在独立
    // async 线程起跑=天然从 0 计；同线程内嵌套逐层 +1，超 3 层拒绝。
    if (g_spawn_nest_depth >= 3) {
      // v0.54.11 (R93): 同 AgentServiceTools——护栏拒绝补日志（原先完全不可见）
      std::cerr << "[WARN] spawn nesting limit (3) reached — rejected"
                   " (depth=" << g_spawn_nest_depth
                << ", active=" << g_active_sub_agents.load() << ")\n";
      return {{"type", "spawn_result"}, {"ok", false},
              {"error", "spawn nesting limit (3) reached — decompose the task instead"}};
    }
    if (g_active_sub_agents.load() >= kMaxActiveSubAgents) {
      std::cerr << "[WARN] policy: too many active sub-agents (16) — rejected"
                   " (active=" << g_active_sub_agents.load() << ")\n";
      return {{"type", "spawn_result"}, {"ok", false},
              {"error", "policy: too many active sub-agents (16) — wait or decompose"}};
    }
    ++g_active_sub_agents;
    ++g_spawn_nest_depth;
    struct DepthGuard {
      ~DepthGuard() {
        --g_spawn_nest_depth;
        --g_active_sub_agents;
      }
    } depth_guard;

    std::string sub_goal = req.value("goal", "");
    if (sub_goal.empty()) return {{"type", "spawn_result"}, {"ok", false}, {"error", "missing goal"}};

    std::string role_name = req.value("role", "researcher");
    const agent::AgentRole* role = role_mgr_ ? role_mgr_->find(role_name) : nullptr;
    if (!role) {
      return {{"type", "spawn_result"}, {"ok", false},
                     {"error", "unknown role: " + role_name},
                     {"available_roles", [&]() {
                        nlohmann::json arr = nlohmann::json::array();
                        if (role_mgr_) for (auto& r : role_mgr_->list()) arr.push_back(r.name);
                        return arr;
                      }()}};
    }

    // 设置角色工具白名单
    agent::AgentLoopConfig sub_cfg;
    sub_cfg.max_turns = req.value("max_turns", 8);
    sub_cfg.request_timeout_ms = req.value("timeout_ms", 60000);
    sub_cfg.human_in_the_loop = false;  // 子 Agent 不中断
    if (!role->tools.empty()) {
      for (const auto& t : role->tools) sub_cfg.allowed_tools.insert(t);
    }

    // 角色化 system prompt
    std::string role_prompt = role->system_prompt;
    if (role_prompt.empty()) role_prompt = "You are a " + role->name + " specialist.";

    // 创建子 AgentLoop
    std::string api_key;
    api_key = resolve_api_key(credential_pool_.get(), cfg_.api_key_env, cfg_.provider);

    auto sub_loop = std::make_unique<agent::AgentLoop>(cfg_, api_key,
                                                       agent::ToolRegistry::instance(), sub_cfg);
    if (memory_manager_) sub_loop->set_memory_manager(memory_manager_.get());
    if (skill_manager_) sub_loop->set_skill_manager(skill_manager_.get());  // v0.39.1
    // 构建角色化任务提示
    std::string task_prompt = role_prompt + "\n\n## Task\n" + sub_goal +
                              "\n\nRespond with your findings directly. Do not ask follow-up questions.";

    // 注入 GoalManager / CorrectionStore 上下文
    std::string goal_ctx, corr_ctx;
    if (goal_mgr_) goal_ctx = goal_mgr_->to_prompt_injection();
    if (correction_store_) {
      std::vector<std::string> known_tools;
      for (const auto& t : agent::ToolRegistry::instance().all_tools())
        known_tools.push_back(t.name);
      corr_ctx = correction_store_->to_prompt_injection(known_tools);
    }
    std::string full_sys = goal_ctx + corr_ctx + task_prompt;

    // v0.11.4: 注入待收消息
    std::string agent_id = req.value("agent_id", role_name + "_" + std::to_string(++sub_agent_counter_));
    if (agent_bus_) {
      auto msgs = agent_bus_->drain(agent_id);
      if (!msgs.empty()) {
        full_sys += "\n\n## Messages from other agents\n";
        for (const auto& m : msgs) full_sys += "- " + m + "\n";
      }
    }

    // v0.11.5: 注入共享黑板
    if (blackboard_ && blackboard_->size() > 0) {
      full_sys += blackboard_->to_prompt_injection();
    }

    auto result = sub_loop->run(sub_goal, full_sys);

    // v0.11.4: 子 Agent 完成后，提取可转发给其他 Agent 的信息
    // 如果输出中提到其他角色名，转发
    if (agent_bus_ && result.ok && !result.final_answer.empty()) {
      std::string output = result.final_answer;
      // 简单启发式：如果输出包含 "→ agent_name" 或 "@agent_name" 格式，转发
      if (role_mgr_) {
        for (auto& r : role_mgr_->list()) {
          std::string marker = "@" + r.name;
          if (output.find(marker) != std::string::npos) {
            // 提取 @agent_name 之后直到下一段的内容作为消息
            auto pos = output.find(marker);
            auto end = output.find('\n', pos + marker.size());
            if (end == std::string::npos) end = output.size();
            std::string msg = output.substr(pos + marker.size(), end - pos - marker.size());
            // trim
            while (!msg.empty() && (msg.front() == ' ' || msg.front() == ':')) msg.erase(0, 1);
            if (!msg.empty()) agent_bus_->send(agent_id, r.name, msg);
          }
        }
      }
    }

    // v0.53.12: spawn 链独立落库 trace——此前仅 chat 链尾落库（L2001），
    // spawn-only 会话的 spans 留内存（下次 chat 被顺带写入或进程退出丢失），
    // 子代理工具调用（如 web_search）无可审计痕迹（真 e2e 实测 3 轮 0 落库）。
    agent::AgentTracer::instance().save_to_db(default_traces_db_path());
    agent::AgentTracer::instance().clear();

    return {{"type", "spawn_result"},
                   // v0.52.12: 假绿终结——degraded（本地兜底产物）时
                   // ok 强制 false：波次调度只看 ok 标 done，离线兜底
                   // 文案不得当业务交付（真 e2e 实测 4 任务假 done）。
                   {"ok", result.ok && !result.degraded},
                   {"agent_id", agent_id},
                   {"role", role_name},
                   {"output", result.final_answer},
                   {"turns", result.turns_used},
                   {"tool_calls_count", result.tool_calls.size()},
                   {"error", result.degraded && result.error.empty()
                                 ? std::string("degraded(local_fallback)")
                                 : result.error}};
  }

  // v0.53.13: 兜底 return——role 解析异常路径走到函数尾（UB，Release
  // -Wreturn-type 实证）。与 role 缺失分支同语义。
  return {{"type", "spawn_result"}, {"ok", false},
          {"error", "role_resolution_failed"}};

  // v0.11.4: Agent 间消息
}

/// v0.50.4: 消息/黑板域（agent_message/inbox*/broadcast + bb_*）。
nlohmann::json AgentService::orch_agent_mail(const std::string& session_id,
                                             const std::string& type,
                                             const nlohmann::json& req) {
  if (type == "agent_message") {
    // v0.53.1: 转发 collab_msg 插件（单一事实源）
    auto r = skill_registry_.dispatch_cpp("agent_message", req);
    if (r.contains("success")) { r["type"] = "agent_message_result"; return r; }
    return {{"type", "error"}, {"message", r.value("error", "agent_message unavailable")}};
  }
  if (type == "agent_inbox") {
    // v0.53.1: 转发 collab_msg 插件（单一事实源）
    auto r = skill_registry_.dispatch_cpp("agent_inbox", req);
    if (r.contains("success")) { r["type"] = "agent_inbox_result"; return r; }
    return {{"type", "error"}, {"message", r.value("error", "agent_inbox unavailable")}};
  }
  if (type == "agent_inbox_drain") {
    if (!agent_bus_) return {{"type", "error"}, {"message", "bus not available"}};
    std::string agent_id = req.value("agent_id", session_id);
    auto msgs = agent_bus_->drain(agent_id);
    nlohmann::json arr = nlohmann::json::array();
    for (auto& m : msgs) arr.push_back(m);
    return {{"type", "inbox_drained"}, {"agent_id", agent_id}, {"messages", arr}, {"count", msgs.size()}};
  }
  if (type == "agent_broadcast") {
    if (!agent_bus_) return {{"type", "error"}, {"message", "bus not available"}};
    std::string from = req.value("from", session_id);
    std::string msg = req.value("message", "");
    if (msg.empty()) return {{"type", "error"}, {"message", "message required"}};
    agent_bus_->broadcast(from, msg);
    return {{"type", "broadcast_sent"}, {"ok", true}, {"from", from}};
  }

  // v0.11.5: 共享黑板
  if (type == "bb_write") {
    auto r = skill_registry_.dispatch_cpp("bb_write", req);
    if (r.contains("success")) { r["type"] = "bb_written"; return r; }
    return {{"type", "error"}, {"message", "bb_write unavailable"}};
  }
  if (type == "bb_read") {
    // v0.53.0: 转发 collab 插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("bb_read", req);
    if (r.contains("success")) { r["type"] = "bb_value"; return r; }
    return {{"type", "error"}, {"message", "bb_read unavailable"}};
  }
  if (type == "bb_keys") {
    if (!blackboard_) return {{"type", "error"}, {"message", "blackboard not available"}};
    auto k = blackboard_->keys();
    return {{"type", "bb_keys"}, {"keys", k}, {"count", k.size()}};
  }
  if (type == "bb_clear") {
    if (!blackboard_) return {{"type", "error"}, {"message", "blackboard not available"}};
    blackboard_->clear();
    return {{"type", "bb_cleared"}, {"ok", true}};
  }

  // v0.11.6: Kanban 看板
  if (type == "kanban_push") {
    // v0.53.0: 转发 kanban 插件（单一事实源；WS 语义=LLM 工具语义）
    auto r = skill_registry_.dispatch_cpp("kanban_push", req);
    if (r.contains("success")) { r["type"] = "kanban_pushed"; return r; }
    return {{"type", "error"}, {"message", r.value("error", "kanban unavailable")}};
  }
  if (type == "kanban_push_batch") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("kanban_push_batch", req);
    if (r.contains("success")) { r["type"] = "kanban_batch_pushed"; return r; }
    return {{"type", "error"}, {"message", "kanban_push_batch unavailable"}};
  }
  if (type == "kanban_status") {
    auto r = skill_registry_.dispatch_cpp("kanban_status", req);
    if (r.contains("board")) { r["type"] = "kanban_status"; return r; }
    return {{"type", "error"}, {"message", "kanban unavailable"}};
  }
  if (type == "kanban_run") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("kanban_run", req);
    if (r.contains("success")) { r["type"] = "kanban_run_result"; return r; }
    return {{"type", "error"}, {"message", "kanban_run unavailable"}};
  }
  if (type == "kanban_clear") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("kanban_clear", req);
    if (r.contains("success")) { r["type"] = "kanban_cleared"; return r; }
    return {{"type", "error"}, {"message", "kanban_clear unavailable"}};
  }

  // v0.11.7: Agent 协商辩论

  return nullptr;
}

/// v0.50.4: Kanban 看板域（kanban_push/push_batch/status/run/clear）。
nlohmann::json AgentService::orch_kanban(const std::string& session_id,
                                         const std::string& type,
                                         const nlohmann::json& req) {
  if (type == "agent_debate") {
    std::string goal = req.value("goal", "");
    auto roles_json = req.value("roles", nlohmann::json::array());
    int rounds = req.value("rounds", 2);

    if (goal.empty()) return {{"type", "debate_result"}, {"ok", false}, {"error", "missing goal"}};
    if (!roles_json.is_array() || roles_json.empty())
      roles_json = {"viewer", "tester", "debugger"};  // 默认三角辩论

    // v0.54.1 (R88): **扇出上限**。此前 roles/rounds 完全由请求体决定且无上限 ——
    // roles×rounds 个 std::async 各跑一遍完整 spawn_agent 管线（max_turns=6），
    // 客户端用 roles=[...1000] × rounds=1000 即可驱动百万级任务打垮服务（线程/内存/LLM 配额），
    // 且请求要等全部轮次跑完才返回。与 R86 的 goal_auto_reason（串行无上限）同族。
    // 现在：默认最多 8 角色 × 5 轮（env 可调），并把截断**如实回报**给调用方。
    static const size_t kMaxDebateRoles = [] {
      const char* e = getenv("THIN_AGENT_DEBATE_MAX_ROLES");
      const long v = e ? atol(e) : 0;
      return v > 0 ? static_cast<size_t>(v) : static_cast<size_t>(8);
    }();
    static const int kMaxDebateRounds = [] {
      const char* e = getenv("THIN_AGENT_DEBATE_MAX_ROUNDS");
      const long v = e ? atol(e) : 0;
      return v > 0 ? static_cast<int>(v) : 5;
    }();
    const auto fanout = clamp_debate_fanout(roles_json.size(), rounds,
                                            kMaxDebateRoles, kMaxDebateRounds);
    if (fanout.truncated) {
      std::cerr << "[WARN] agent_debate fan-out clamped: roles " << fanout.roles_requested
                << "→" << fanout.roles << ", rounds " << fanout.rounds_requested << "→"
                << fanout.rounds << " (max " << kMaxDebateRoles << "×" << kMaxDebateRounds
                << "; raise THIN_AGENT_DEBATE_MAX_ROLES/ROUNDS to allow more)\n";
      if (fanout.roles < roles_json.size()) {
        nlohmann::json kept = nlohmann::json::array();
        for (size_t i = 0; i < fanout.roles; ++i) kept.push_back(roles_json[i]);
        roles_json = std::move(kept);
      }
      rounds = fanout.rounds;
    }

    // 清空黑板和消息总线，为新辩论准备
    if (blackboard_) blackboard_->clear();
    if (agent_bus_) agent_bus_->clear_all();

    nlohmann::json all_round_results = nlohmann::json::array();

    for (int round = 0; round < rounds; ++round) {
      nlohmann::json round_results = nlohmann::json::array();
      std::vector<std::future<nlohmann::json>> futures;

      for (auto& rj : roles_json) {
        std::string role_name = rj.is_string() ? rj.get<std::string>() : rj.value("role", "researcher");
        std::string agent_id = role_name + "_r" + std::to_string(round);

        futures.push_back(std::async(std::launch::async, [this, session_id, goal, role_name, agent_id, round, rounds]() {
          nlohmann::json spawn_req;
          spawn_req["type"] = "spawn_agent";
          spawn_req["agent_id"] = agent_id;

          // 每轮的 prompt 不同
          std::string round_goal;
          if (round == 0) {
            round_goal = goal + "\n\nProvide your independent analysis. Be thorough and cite evidence.\n"
                         "After your analysis, note any questions for other roles using @role_name format.";
          } else {
            round_goal = goal + "\n\n## Round " + std::to_string(round + 1) + "\n"
                         "Read the shared blackboard — it contains all agents' analyses from previous rounds.\n"
                         "Refine your position. If you disagree with another agent's finding, explain why.\n"
                         "Use @role_name to directly address specific agents. Your goal is to converge toward consensus.\n"
                         "At the end, state your final position clearly.";
          }
          spawn_req["goal"] = round_goal;
          spawn_req["role"] = role_name;
          spawn_req["max_turns"] = 6;

          auto result = handle_request(session_id, spawn_req);

          // 写入黑板共享
          // (通过 bb_write 间接 — 这里直接写)
          if (this->blackboard_) {
            this->blackboard_->write("round_" + std::to_string(round) + "_" + role_name,
                                     result.value("output", ""));
          }

          return result;
        }));
      }

      // 等待本轮所有 Agent 完成
      for (auto& f : futures) {
        auto result = f.get();
        round_results.push_back({
          {"round", round + 1},
          {"role", result.value("role", "unknown")},
          {"ok", result.value("ok", false)},
          {"output", result.value("output", "")}
        });
      }

      all_round_results.push_back({
        {"round", round + 1},
        {"results", round_results}
      });
    }

    // 最终：moderator 综合所有轮次产出共识
    nlohmann::json synth_req;
    synth_req["type"] = "agent_synthesize";
    synth_req["goal"] = goal + "\n\nAfter " + std::to_string(rounds) + " rounds of debate between multiple agents.\n"
                        "Identify:\n1. Points of AGREEMENT (all agents concur)\n"
                        "2. Points of DISAGREEMENT (which agent holds which view, with their reasoning)\n"
                        "3. Your WEIGHTED RECOMMENDATION (lean on the strongest evidence)";

    // 收集所有轮次结果作为合成输入
    nlohmann::json synth_results = nlohmann::json::array();
    if (blackboard_) {
      for (auto& k : blackboard_->keys()) {
        auto val = blackboard_->read(k);
        synth_results.push_back({
          {"role", k},
          {"output", val.is_string() ? val.get<std::string>() : val.dump()},
          {"ok", true}
        });
      }
    }
    synth_req["results"] = synth_results;
    auto synth_resp = handle_request(session_id, synth_req);

    return {{"type", "debate_result"},
                   {"ok", synth_resp.value("ok", false)},
                   {"goal", goal},
                   {"rounds", rounds},
                   {"participants", roles_json},
                   {"round_results", all_round_results},
                   {"consensus", synth_resp.value("conclusion", "")},
                   {"error", synth_resp.value("error", "")},
                   // v0.54.1 (R88): 截断必须**可见**（如实回报，不静默丢）
                   {"fanout_truncated", fanout.truncated},
                   {"roles_requested", fanout.roles_requested},
                   {"rounds_requested", fanout.rounds_requested}};
  }

  // v0.11.0: 角色管理

  return nullptr;
}

/// v0.50.4: 辩论/角色域（agent_debate + role_*/skill_remove/summarize）。
nlohmann::json AgentService::orch_debate_roles(const std::string& session_id,
                                               const std::string& type,
                                               const nlohmann::json& req) {
  if (type == "role_list") {
    if (!role_mgr_) return {{"type", "error"}, {"message", "role manager not available"}};
    auto roles = role_mgr_->list();
    nlohmann::json arr = nlohmann::json::array();
    for (auto& r : roles) arr.push_back(r.to_json());
    return {{"type", "role_list_result"}, {"roles", arr}};
  }
  if (type == "role_register") {
    if (!role_mgr_) return {{"type", "error"}, {"message", "role manager not available"}};
    try {
      auto role = agent::AgentRole::from_json(req);
      if (role.name.empty()) return {{"type", "error"}, {"message", "role name required"}};
      role_mgr_->register_role(role);
      return {{"type", "role_registered"}, {"ok", true}, {"name", role.name}};
    } catch (const std::exception& e) {
      return {{"type", "error"}, {"message", std::string("invalid role json: ") + e.what()}};
    }
  }
  if (type == "role_remove") {
    if (!role_mgr_) return {{"type", "error"}, {"message", "role manager not available"}};
    std::string name = req.value("name", "");
    if (name.empty()) return {{"type", "error"}, {"message", "role name required"}};
    return {{"type", "role_removed"}, {"ok", role_mgr_->remove(name)}, {"name", name}};
  }

  if (type == "skill_remove") {
    if (!skill_manager_) return {{"type", "error"}, {"message", "not available"}};
    return {{"type", "skill_removed"}, {"ok", skill_manager_->remove_skill(req.value("skill_id", ""))}};
  }

  if (type == "summarize") {
    // v0.53.1: 转发 meta 插件（单一事实源）
    auto r = skill_registry_.dispatch_cpp("summarize", req);
    if (r.contains("success")) { r["type"] = "summarize_result"; return r; }
    return {{"type", "error"}, {"message", r.value("error", "summarize unavailable")}};
  }

  return nullptr;
}

/// v0.50.4: 编排执行域（orchestrate/orch_status/agent_dag）。
nlohmann::json AgentService::orch_orchestrate_dag(const std::string& session_id,
                                                  const std::string& type,
                                                  const nlohmann::json& req) {
  if (type == "orchestrate") {
    if (!orchestrator_) return {{"type", "error"}, {"message", "orchestrator not available"}};
    auto tasks_json = req.value("tasks", nlohmann::json::array());
    if (!tasks_json.is_array() || tasks_json.empty())
      return {{"type", "error"}, {"message", "tasks required"}};

    AgentOrchestrator::OrchestrationRequest oreq;
    oreq.request_id = req.value("request_id", session_id + "_orch");
    oreq.session_id = session_id;
    for (const auto& t : tasks_json) {
      AgentOrchestrator::OrchestrationTask task;
      task.task_id = t.value("task_id", "task_" + std::to_string(oreq.tasks.size()));
      task.name = t.value("name", task.task_id);
      task.prompt = t.value("prompt", "");
      task.context = t.value("context", nlohmann::json::object());
      oreq.tasks.push_back(std::move(task));
    }

    // v0.25.1: 真实 AgentLoop executor（替代 stub）
    orchestrator_->set_executor([this](const AgentOrchestrator::OrchestrationTask& t) {
      AgentOrchestrator::TaskResult r;
      r.task_id = t.task_id;

      // 确定角色
      std::string role_name = t.role.empty() ? "researcher" : t.role;
      const agent::AgentRole* role = role_mgr_ ? role_mgr_->find(role_name) : nullptr;

      agent::AgentLoopConfig sub_cfg;
      sub_cfg.max_turns = t.context.value("max_turns", 8);
      sub_cfg.request_timeout_ms = t.context.value("timeout_ms", 60000);
      sub_cfg.human_in_the_loop = false;

      std::string system_prompt;
      if (role) {
        system_prompt = role->system_prompt;
        if (!role->tools.empty()) {
          for (const auto& tool : role->tools) sub_cfg.allowed_tools.insert(tool);
        }
      }
      if (system_prompt.empty()) system_prompt = "You are a " + role_name + " specialist.";

      std::string full_prompt = system_prompt + "\n\n## Task\n" + t.prompt +
          "\n\nRespond with your findings directly. Do not ask follow-up questions.";

      std::string api_key;
      api_key = resolve_api_key(credential_pool_.get(), cfg_.api_key_env, cfg_.provider);

      auto sub_loop = std::make_unique<agent::AgentLoop>(
          cfg_, api_key, agent::ToolRegistry::instance(), sub_cfg);
      if (memory_manager_) sub_loop->set_memory_manager(memory_manager_.get());
    if (skill_manager_) sub_loop->set_skill_manager(skill_manager_.get());  // v0.39.1
      auto result = sub_loop->run(t.prompt, full_prompt);

      r.ok = result.ok;
      r.result = {
        {"output", result.final_answer},
        {"turns", result.turns_used},
        {"tool_calls", result.tool_calls.size()},
        {"error", result.error}
      };
      return r;
    });

    auto result = orchestrator_->execute(oreq, req.value("timeout_ms", 30000));
    nlohmann::json rj;
    rj["type"] = "orchestrated";
    rj["request_id"] = result.request_id;
    rj["all_ok"] = result.all_ok;
    rj["total_latency_ms"] = result.total_latency_ms;
    rj["results"] = nlohmann::json::array();
    for (const auto& r : result.results) {
      rj["results"].push_back({{"task_id", r.task_id}, {"ok", r.ok}, {"result", r.result}, {"error", r.error}, {"latency_ms", r.latency_ms}});
    }
    return std::move(rj);
  }

  if (type == "orch_status") {
    if (!orchestrator_) return {{"type", "error"}, {"message", "orchestrator not available"}};
    auto s = orchestrator_->status();
    return {{"type", "orch_status"}, {"num_workers", s.num_workers}, {"active_tasks", s.active_tasks}, {"queued_tasks", s.queued_tasks}, {"total_completed", s.total_completed}};
  }

  // v0.11.1: DAG 编排
  if (type == "agent_dag") {
    if (!orchestrator_) return {{"type", "error"}, {"message", "orchestrator not available"}};
    auto nodes_json = req.value("nodes", nlohmann::json::array());
    if (!nodes_json.is_array() || nodes_json.empty())
      return {{"type", "error"}, {"message", "nodes required"}};

    AgentOrchestrator::DAGRequest dag_req;
    dag_req.request_id = req.value("request_id", session_id + "_dag");
    dag_req.session_id = session_id;
    for (const auto& n : nodes_json) {
      AgentOrchestrator::DAGNode node;
      node.task_id = n.value("task_id", "task_" + std::to_string(dag_req.nodes.size()));
      node.name = n.value("name", node.task_id);
      node.prompt = n.value("prompt", "");
      node.role = n.value("role", "");
      if (n.contains("depends_on") && n["depends_on"].is_array()) {
        for (const auto& d : n["depends_on"]) node.depends_on.push_back(d.get<std::string>());
      }
      node.context = n.value("context", nlohmann::json::object());
      dag_req.nodes.push_back(std::move(node));
    }

    orchestrator_->set_executor([this](const AgentOrchestrator::OrchestrationTask& t) {
      AgentOrchestrator::TaskResult r;
      r.task_id = t.task_id;

      std::string role_name = t.role.empty() ? "researcher" : t.role;
      const agent::AgentRole* role = role_mgr_ ? role_mgr_->find(role_name) : nullptr;

      agent::AgentLoopConfig sub_cfg;
      sub_cfg.max_turns = t.context.value("max_turns", 8);
      sub_cfg.request_timeout_ms = t.context.value("timeout_ms", 60000);
      sub_cfg.human_in_the_loop = false;

      std::string system_prompt;
      if (role) {
        system_prompt = role->system_prompt;
        if (!role->tools.empty())
          for (const auto& tool : role->tools) sub_cfg.allowed_tools.insert(tool);
      }
      if (system_prompt.empty())
        system_prompt = "You are a " + role_name + " specialist.";

      std::string full_prompt = system_prompt + "\n\n## Task\n" + t.prompt +
          "\n\nRespond with your findings directly. Do not ask follow-up questions.";

      std::string api_key;
      api_key = resolve_api_key(credential_pool_.get(), cfg_.api_key_env, cfg_.provider);

      auto sub_loop = std::make_unique<agent::AgentLoop>(
          cfg_, api_key, agent::ToolRegistry::instance(), sub_cfg);
      if (memory_manager_) sub_loop->set_memory_manager(memory_manager_.get());
    if (skill_manager_) sub_loop->set_skill_manager(skill_manager_.get());  // v0.39.1
      auto result = sub_loop->run(t.prompt, full_prompt);

      r.ok = result.ok;
      r.result = {
        {"output", result.final_answer},
        {"turns", result.turns_used},
        {"tool_calls", result.tool_calls.size()},
        {"error", result.error}
      };
      return r;
    });

    auto result = orchestrator_->execute_dag(dag_req, req.value("timeout_ms", 30000));
    nlohmann::json rj;
    rj["type"] = "agent_dag_result";
    rj["request_id"] = result.request_id;
    rj["all_ok"] = result.all_ok;
    rj["total_latency_ms"] = result.total_latency_ms;
    rj["results"] = nlohmann::json::array();
    for (const auto& r : result.results) {
      rj["results"].push_back({{"task_id", r.task_id}, {"ok", r.ok}, {"result", r.result}, {"error", r.error}, {"latency_ms", r.latency_ms}});
    }
    return std::move(rj);
  }

  // v0.11.3: 动态任务分解

  return nullptr;
}

/// v0.52.4(L3): 跑测试 gate。绿=true；红/执行失败=false（out 带输出）。
/// 宿主侧 popen（测试须真实环境，不走沙箱）；超时硬杀。
bool AgentService::run_project_test_gate(const std::string& session_id,
                                         nlohmann::json& report) {
  report = nlohmann::json::object();
  report["commands"] = nlohmann::json::array();
  report["passed"] = true;
  auto cmds = collect_project_test_commands(session_id);
  if (cmds.empty()) {
    report["skipped"] = true;
    return true;  // 无已知测试系统——不阻塞（通用性：纯文本任务等）
  }
  report["skipped"] = false;
  for (auto& cmd : cmds) {
    nlohmann::json entry;
    entry["command"] = cmd;
    std::string output;
    bool ok = false;
    // popen + 超时：读线程+进程组杀（复用 patrol 模式）
    std::string full = "timeout 120 " + cmd + " 2>&1";
    FILE* pipe = ::popen(full.c_str(), "r");
    if (pipe) {
      char buf[4096];
      size_t total = 0;
      while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        if (total < 16384) output += buf;
        total += strlen(buf);
      }
      int rc = ::pclose(pipe);
      ok = (rc == 0);  // timeout(1) 124=超时也非零
      entry["exit_code"] = rc;
    } else {
      entry["error"] = "popen_failed";
    }
    entry["ok"] = ok;
    if (output.size() > 4096) output = output.substr(0, 2048) + "\n...[截断]...\n" + output.substr(output.size() - 2048);
    entry["output_tail"] = output;
    report["commands"].push_back(entry);
    if (!ok) {
      report["passed"] = false;
      report["failed_command"] = cmd;
      break;  // 首红即停（输出已够回炉诊断）
    }
  }
  return report["passed"].get<bool>();
}

nlohmann::json AgentService::run_decompose_waves(
    const std::string& session_id,
    const nlohmann::json& tasks,
    const nlohmann::json& completed_in,
    int parent_goal_id,
    const std::map<std::string, int>& taskid_to_goal,
    int max_concurrent) {
  nlohmann::json all_results = nlohmann::json::array();

  // v0.52.4: 断点续跑——已完成的子任务（completed_in 按 task_id 索引）
  // 直接入结果集不重跑；依赖判定以其 ok 状态为准。
  std::map<std::string, nlohmann::json> completed;
  for (auto it = completed_in.begin(); it != completed_in.end(); ++it) {
    completed[it.key()] = it.value();
    all_results.push_back(it.value());
  }

  std::vector<nlohmann::json> pending;
  for (auto& t : tasks) {
    std::string tid = t.value("task_id", "");
    // v0.52.4: 已终结（完成/失败）的子任务不重跑——从 pending 剔除
    auto cit = completed.find(tid);
    if (cit != completed.end()) continue;
    pending.push_back(t);
  }

  while (!pending.empty()) {
    std::vector<size_t> ready;
    for (size_t i = 0; i < pending.size(); ++i) {
      bool all_deps_ok = true;
      auto deps = pending[i].value("depends_on", nlohmann::json::array());
      for (const auto& d : deps) {
        std::string dep_id = d.get<std::string>();
        auto it = completed.find(dep_id);
        if (it == completed.end() || !it->second.value("ok", false)) {
          all_deps_ok = false;
          break;
        }
      }
      if (all_deps_ok) ready.push_back(i);
    }

    if (ready.empty()) {
      for (auto& p : pending) {
        nlohmann::json err;
        err["task_id"] = p.value("task_id", "unknown");
        err["ok"] = false;
        err["error"] = "blocked: unresolved dependencies";
        all_results.push_back(err);
      }
      break;
    }

    std::vector<std::future<nlohmann::json>> futures;
    for (size_t ri = 0; ri < ready.size() && (int)ri < max_concurrent; ++ri) {
      size_t idx = ready[ri];
      auto task = pending[idx];
      std::string sub_goal = task.value("prompt", "");
      std::string role_name = task.value("role", "researcher");
      int max_turns_val = task.value("max_turns", 8);
      int timeout_val = task.value("timeout_ms", 60000);

      futures.push_back(std::async(std::launch::async, [this, session_id, sub_goal, role_name, max_turns_val, timeout_val]() {
        nlohmann::json spawn_req;
        spawn_req["type"] = "spawn_agent";
        spawn_req["goal"] = sub_goal;
        spawn_req["role"] = role_name;
        spawn_req["max_turns"] = max_turns_val;
        spawn_req["timeout_ms"] = timeout_val;
        return handle_request(session_id, spawn_req);
      }));
    }

    for (size_t fi = 0; fi < futures.size(); ++fi) {
      auto result = futures[fi].get();
      auto& task = pending[ready[fi]];
      result["task_id"] = task.value("task_id", "task_" + std::to_string(fi));
      result["name"] = task.value("name", "");
      all_results.push_back(result);
      completed[task.value("task_id", "")] = result;
      if (parent_goal_id > 0 && goal_mgr_) {
        auto git = taskid_to_goal.find(task.value("task_id", ""));
        if (git != taskid_to_goal.end()) {
          goal_mgr_->update_status(git->second,
                                   result.value("ok", false) ? "done" : "failed");
          goal_mgr_->refresh_parent_progress(git->second);
        }
      }
      pending[ready[fi]] = nlohmann::json();
    }

    pending.erase(std::remove_if(pending.begin(), pending.end(),
                                 [](const nlohmann::json& j) { return j.is_null(); }),
                  pending.end());
  }
  return all_results;
}

nlohmann::json AgentService::orch_decompose(const std::string& session_id,
                                            const std::string& type,
                                            const nlohmann::json& req) {
  // v0.52.4: 断点扫描——active 父目标带分解 meta 即可恢复（启动报告用）
  if (type == "agent_decompose_scan") {
    nlohmann::json out;
    out["type"] = "decompose_scan_result";
    out["resumable"] = 0;
    out["goals"] = nlohmann::json::array();
    if (goal_mgr_) {
      for (auto& g : goal_mgr_->list("active")) {
        auto meta = goal_mgr_->get_meta(g.id);
        if (meta.is_null() || !meta.contains("tasks")) continue;
        int total = (int)meta.value("tasks", nlohmann::json::array()).size();
        int done = (int)meta.value("completed", nlohmann::json::object()).size();
        if (done < total) {
          out["resumable"] = out["resumable"].get<int>() + 1;
          out["goals"].push_back({{"goal_id", g.id},
                                  {"description", g.description},
                                  {"done", done}, {"total", total}});
        }
      }
    }
    return out;
  }
  // v0.52.4: 断点续跑——从 goal meta 恢复（tasks/映射/已完成结果），
  // 未终结子任务继续跑，全终结后补合成。服务重启/GIMP 网断等中断后
  // 用 agent_decompose_resume {goal_id} 续跑，不重跑已完成子任务。
  if (type == "agent_decompose_resume") {
    int goal_id = req.value("goal_id", 0);
    if (goal_id <= 0 || !goal_mgr_)
      return {{"type", "decompose_resume_result"}, {"ok", false},
              {"error", "missing goal_id"}};
    auto bp = goal_mgr_->get_meta(goal_id);
    if (bp.is_null() || !bp.contains("tasks") || !bp.contains("completed"))
      return {{"type", "decompose_resume_result"}, {"ok", false},
              {"error", "no breakpoint meta for goal " + std::to_string(goal_id)}};

    auto tasks = bp.value("tasks", nlohmann::json::array());
    auto completed = bp.value("completed", nlohmann::json::object());
    std::map<std::string, int> taskid_to_goal;
    auto t2g = bp.value("taskid_to_goal", nlohmann::json::object());
    for (auto it = t2g.begin(); it != t2g.end(); ++it)
      taskid_to_goal[it.key()] = it.value().get<int>();
    // v0.54.2 (R89): max_concurrent **客户端直接可控且无上限**（此前 `req.value(...,3)`）。
    // 传 1000 即 1000 条完整子代理管线并行。钳到 ≤8（env 可调）。
    int max_concurrent = req.value("max_concurrent", 3);
    {
      const char* e = getenv("THIN_AGENT_DECOMPOSE_MAX_CONCURRENT");
      const int cap = e ? std::max(1, atoi(e)) : 8;
      if (max_concurrent > cap) {
        std::cerr << "[WARN] decompose max_concurrent clamped: " << max_concurrent << "→" << cap
                  << " (raise THIN_AGENT_DECOMPOSE_MAX_CONCURRENT to allow more)\n";
        max_concurrent = cap;
      }
      if (max_concurrent < 1) max_concurrent = 1;
    }

    auto all_results = run_decompose_waves(
        session_id, tasks, completed, goal_id, taskid_to_goal, max_concurrent);

    // 更新断点（新一轮结果）
    bp["completed"] = nlohmann::json::object();
    for (auto& r : all_results)
      if (!r.value("task_id", "").empty())
        bp["completed"][r.value("task_id", "")] = r;
    goal_mgr_->set_meta(goal_id, bp);

    // 补合成（L3 gate 同款）
    nlohmann::json gate_report;
    bool gate_passed = run_project_test_gate(session_id, gate_report);
    if (!gate_passed) {
      log_event("orchestrate", LogLevel::Warn, "L3 gate: tests failed, resume synthesis blocked",
                {{"failed_command", gate_report.value("failed_command", "")}});
      goal_mgr_->update_status(goal_id, "failed");
      return {{"type", "decompose_resume_result"},
              {"ok", false},
              {"goal_id", goal_id},
              {"error", "project tests failed (L3 gate)"},
              {"gate", gate_report},
              {"sub_results", all_results}};
    }

    nlohmann::json synth_req;
    synth_req["type"] = "agent_synthesize";
    synth_req["goal"] = req.value("goal", goal_mgr_->get(goal_id).description);
    synth_req["results"] = all_results;
    auto synth_resp = handle_request(session_id, synth_req);

    // v0.52.16: 合成结论落库——树全完成后结论只在 WS 响应里，服务
    // 重启/断连即永久丢失（真 e2e 实测 conclusion 不在 goal meta）。
    // 写回 meta.synthesis，resume/审计可追溯。
    {
      auto m = goal_mgr_->get_meta(goal_id);
      if (!m.is_null()) {
        m["synthesis"] = synth_resp.value("conclusion", "");
        m["synthesis_at"] = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        goal_mgr_->set_meta(goal_id, m);
      }
    }

    return {{"type", "decompose_resume_result"},
            {"ok", synth_resp.value("ok", false)},
            {"goal_id", goal_id},
            {"sub_results", all_results},
            {"conclusion", synth_resp.value("conclusion", "")},
            {"error", synth_resp.value("error", "")}};
  }
  if (type == "agent_decompose") {
    std::string goal = req.value("goal", "");
    if (goal.empty()) return {{"type", "decompose_result"}, {"ok", false}, {"error", "missing goal"}};

    // 构建可用角色列表
    std::string roles_desc;
    if (role_mgr_) {
      for (auto& r : role_mgr_->list()) {
        roles_desc += "- " + r.name + ": " + r.description + "\n";
      }
    }
    if (roles_desc.empty()) roles_desc = "- researcher: general research and analysis\n- summarizer: synthesis\n";

    // 分解 prompt
    std::string decomp_prompt = R"(
You are a task decomposition specialist. Given a complex goal, break it down into sub-tasks
that can be executed by specialized agents. Each sub-task should be independent where possible,
and only depend on other sub-tasks when their output is needed.

## Available Roles
)" + roles_desc + R"(

## Output Format (JSON only, no markdown)
{
  "tasks": [
    {
      "task_id": "unique_id",
      "name": "short name",
      "prompt": "detailed instruction for this sub-task",
      "role": "one of the available roles",
      "depends_on": ["task_id_of_dependency"]  // empty array if no dependencies
    }
  ]
}

## Rules
1. Assign the MOST SPECIFIC role to each task. Do not use "researcher" for code review.
2. Only add depends_on when one task genuinely needs another's output.
3. Tasks with no dependencies should all be in the first wave (parallel).
4. Each prompt must be self-contained — the agent cannot ask follow-up questions.
5. Limit to 3-7 sub-tasks. Do not over-decompose.

## Goal
)" + goal;

    // 调用 LLM
    std::string api_key;
    api_key = resolve_api_key(credential_pool_.get(), cfg_.api_key_env, cfg_.provider);

    agent::AgentLoopConfig decomp_cfg;
    decomp_cfg.max_turns = 1;
    decomp_cfg.human_in_the_loop = false;
    // v0.52.8: 分解提示大+GLM coding 端点慢（直连实测 41s+），分解单次
    // LLM 超时 150s。两侧都要设：CloudLlmClient FC 路径按
    // cfg.request_timeout_ms 重建 CurlHttpClient（v0.51.5 动态超时
    // 探测），仅设 loop 侧会被 30s 覆盖（真 e2e 第七轮实测仍 30s 断）
    decomp_cfg.request_timeout_ms = 150000;
    auto decomp_cloud_cfg = cfg_;
    decomp_cloud_cfg.request_timeout_ms = 150000;
    // v0.52.8: 分解输出大（多任务 JSON），思考型模型 reasoning 与
    // content 共享 max_tokens 预算——2048 下 reasoning 易耗尽预算致
    // content 空（真 e2e 实测 empty_content_with_reasoning 连败）
    decomp_cloud_cfg.max_completion_tokens = 8192;

    auto decomp_loop = std::make_unique<agent::AgentLoop>(decomp_cloud_cfg, api_key,
                                                          agent::ToolRegistry::instance(), decomp_cfg);
    auto decomp_result = decomp_loop->run(goal, decomp_prompt);

    // v0.52.8 诊断：分解失败时记录 AgentLoop 层细节（本地兜底会掩盖
    // 真实 LLM 错误——raw 是 HybridRouter 离线文本时无从归因）
    if (decomp_result.final_answer.find("离线模式") != std::string::npos ||
        decomp_result.final_answer.find("offline") != std::string::npos) {
      log_event("orchestrate", LogLevel::Warn, "decompose hit local fallback (LLM failed upstream)",
                {{"error", decomp_result.error}, {"turns", decomp_result.turns_used}});
    }

    // 解析 JSON
    nlohmann::json tasks_json;
    if (decomp_result.ok && !decomp_result.final_answer.empty()) {
      try {
        // 提取 JSON block
        // v0.52.3: 增强——LLM 常包 markdown fence 或前后说明文本，
        // find('{')/rfind('}') 对"尾随 } 的说明文字"会截出非法片段
        // （e2e 实测 GLM 分解输出解析失败）。改为平衡括号扫描：
        // 从每个 '{' 起找配对 '}'，首个可完整解析且含 tasks 的段胜出。
        std::string raw = decomp_result.final_answer;
        // v0.52.3: 平衡括号扫描提取（JsonExtract.h，e2e 实测 LLM 输出
        // 包 markdown fence/尾随文本时旧 find/rfind 截出非法片段）
        tasks_json = extract_json_object_with_array(raw, "tasks");
      } catch (...) {
        tasks_json = nlohmann::json::object();
      }
    }

    if (!tasks_json.contains("tasks") || !tasks_json["tasks"].is_array()) {
      log_event("orchestrate", LogLevel::Warn,
                "decompose parse failed (raw head)",
                {{"len", decomp_result.final_answer.size()},
                 {"ok", decomp_result.ok},
                 {"head", decomp_result.final_answer.substr(0, 200)}});
      return {{"type", "decompose_result"}, {"ok", false},
                     {"error", "failed to parse decomposition"},
                     {"raw", decomp_result.final_answer}};
    }

    return {{"type", "decompose_result"},
                   {"ok", true},
                   {"goal", goal},
                   {"tasks", tasks_json["tasks"]},
                   {"task_count", tasks_json["tasks"].size()}};
  }

  // v0.11.3: 一键分解+执行+合成
  if (type == "agent_decompose_and_run") {
    // Step 1: 分解
    nlohmann::json decomp_req;
    decomp_req["type"] = "agent_decompose";
    decomp_req["goal"] = req.value("goal", "");
    auto decomp_resp = handle_request(session_id, decomp_req);
    // v0.52.16: 分解被拒降级重试——GLM 对部分复杂 goal 返回
    // "处理步骤较多，已超过系统限制"（真 e2e 两次实测），当解析
    // 失败直接回错，需人工改写 goal。语义识别该拒答后自动注入
    // 简化指令重试一次（"任务已简化，最多 3 个子任务"），过不了
    // 再如实报错。
    if (!decomp_resp.value("ok", false)) {
      const std::string raw_err = decomp_resp.value("error", "");
      const std::string raw_head = decomp_resp.value("raw", "").substr(0, 120);
      const bool rejected_by_llm =
          raw_head.find("超过系统限制") != std::string::npos ||
          raw_head.find("处理步骤较多") != std::string::npos ||
          raw_head.find("too many steps") != std::string::npos;
      (void)raw_err;
      if (rejected_by_llm) {
        log_event("orchestrate", LogLevel::Warn,
                  "decompose rejected by LLM, retrying simplified",
                  {{"head", raw_head}});
        nlohmann::json retry_req;
        retry_req["type"] = "agent_decompose";
        retry_req["goal"] = req.value("goal", "") +
            "\n\n（注意：上一版分解因步骤过多被拒。请输出不超过 3 个"
            "子任务的简化分解，每个子任务的 prompt 必须简短自足。）";
        decomp_resp = handle_request(session_id, retry_req);
      }
    }
    if (!decomp_resp.value("ok", false)) {
      return {{"type", "decompose_and_run_result"}, {"ok", false},
                     {"stage", "decompose"}, {"error", decomp_resp.value("error", "decomposition failed")}};
    }

    auto tasks = decomp_resp.value("tasks", nlohmann::json::array());

    // v0.52.3: 任务树落库（use_goal_tree 或显式 goal_tree=true）——
    // 父目标=原始 goal，子任务=分解项；完成后 refresh_parent_progress
    // 级联（跨会话可 goal_tree 查进度）。
    const bool use_goal_tree = req.value("use_goal_tree", false) ||
                               req.value("goal_tree", false);
    int parent_goal_id = 0;
    std::map<std::string, int> taskid_to_goal;
    if (use_goal_tree && goal_mgr_) {
      auto parent = goal_mgr_->create(req.value("goal", ""));
      parent_goal_id = parent.id;
      for (const auto& t : tasks) {
        auto sub = goal_mgr_->create_subtask(
            parent_goal_id, t.value("name", t.value("task_id", "")));
        taskid_to_goal[t.value("task_id", "")] = sub.id;
      }
      log_event("orchestrate", LogLevel::Info, "decomposition persisted to goal tree",
                {{"parent", parent_goal_id}, {"subtasks", taskid_to_goal.size()}});
    }

    // Step 2: 波浪执行（v0.52.4 抽公共函数 run_decompose_waves——
    // decompose_and_run 与 resume 共用；断点=meta.completed，按 task_id
    // 索引的已完成结果，续跑时直接入结果集不重跑）
    // v0.54.2 (R89): max_concurrent **客户端直接可控且无上限**（此前 `req.value(...,3)`）。
    // 传 1000 即 1000 条完整子代理管线并行。钳到 ≤8（env 可调）。
    int max_concurrent = req.value("max_concurrent", 3);
    {
      const char* e = getenv("THIN_AGENT_DECOMPOSE_MAX_CONCURRENT");
      const int cap = e ? std::max(1, atoi(e)) : 8;
      if (max_concurrent > cap) {
        std::cerr << "[WARN] decompose max_concurrent clamped: " << max_concurrent << "→" << cap
                  << " (raise THIN_AGENT_DECOMPOSE_MAX_CONCURRENT to allow more)\n";
        max_concurrent = cap;
      }
      if (max_concurrent < 1) max_concurrent = 1;
    }

    // v0.52.4: 分解 payload 落 meta（断点恢复的数据源）——含 tasks、
    // taskid→goal 映射、已完成结果（每波后增量更新）
    nlohmann::json breakpoint;
    breakpoint["tasks"] = tasks;
    breakpoint["taskid_to_goal"] = nlohmann::json::object();
    for (auto& [k, v] : taskid_to_goal) breakpoint["taskid_to_goal"][k] = v;
    breakpoint["completed"] = nlohmann::json::object();
    if (parent_goal_id > 0 && goal_mgr_)
      goal_mgr_->set_meta(parent_goal_id, breakpoint);

    auto all_results = run_decompose_waves(
        session_id, tasks, nlohmann::json::object(),
        parent_goal_id, taskid_to_goal, max_concurrent);

    // 完成后落终态（合成前的完整结果——崩溃后可 resume 补合成）
    breakpoint["completed"] = nlohmann::json::object();
    for (auto& r : all_results)
      if (!r.value("task_id", "").empty())
        breakpoint["completed"][r.value("task_id", "")] = r;
    if (parent_goal_id > 0 && goal_mgr_)
      goal_mgr_->set_meta(parent_goal_id, breakpoint);

    // Step 3: 合成（v0.52.4 L3：合成前测试 gate——项目测试红则拒绝
    // 合成、结果标 failed，测试输出随附供回炉诊断；resume 后重验）
    nlohmann::json gate_report;
    bool gate_passed = run_project_test_gate(session_id, gate_report);
    if (!gate_passed) {
      log_event("orchestrate", LogLevel::Warn, "L3 gate: tests failed, synthesis blocked",
                {{"failed_command", gate_report.value("failed_command", "")}});
      // 任务树父目标置 failed（断点保留——resume 回炉后重验）
      if (parent_goal_id > 0 && goal_mgr_)
        goal_mgr_->update_status(parent_goal_id, "failed");
      return {{"type", "decompose_and_run_result"},
              {"ok", false},
              {"goal", req.value("goal", "")},
              {"goal_tree_id", parent_goal_id},
              {"error", "project tests failed (L3 gate)"},
              {"gate", gate_report},
              {"sub_results", all_results}};
    }

    nlohmann::json synth_req;
    synth_req["type"] = "agent_synthesize";
    synth_req["goal"] = req.value("goal", "");
    synth_req["results"] = all_results;
    auto synth_resp = handle_request(session_id, synth_req);

    // v0.52.16: 合成结论落库（同 resume 路径，防断连/重启丢失）
    if (parent_goal_id > 0 && goal_mgr_) {
      auto m = goal_mgr_->get_meta(parent_goal_id);
      if (!m.is_null()) {
        m["synthesis"] = synth_resp.value("conclusion", "");
        m["synthesis_at"] = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        goal_mgr_->set_meta(parent_goal_id, m);
      }
    }

    return {{"type", "decompose_and_run_result"},
                   {"ok", synth_resp.value("ok", false)},
                   {"goal", req.value("goal", "")},
                   {"sub_results", all_results},
                   {"conclusion", synth_resp.value("conclusion", "")},
                   {"summary", synth_resp.value("conclusion",
                       "任务完成：" + req.value("goal", "").substr(0, 120))},
                   {"sub_agent_count", all_results.size()},
                   {"goal_tree_id", parent_goal_id},
                   {"error", synth_resp.value("error", "")}};
  }

  // v0.11.2: LLM 结果合成
  if (type == "agent_synthesize") {
    std::string goal = req.value("goal", "");
    auto sub_results = req.value("results", nlohmann::json::array());
    if (goal.empty()) return {{"type", "synthesize_result"}, {"ok", false}, {"error", "missing goal"}};
    if (!sub_results.is_array() || sub_results.empty())
      return {{"type", "synthesize_result"}, {"ok", false}, {"error", "no sub-agent results to synthesize"}};

    // 构建合成 prompt
    std::string synthesis_prompt =
        "You are a synthesis specialist. Given the outputs from multiple specialized agents "
        "working on the same goal, your job is to:\n"
        "- Cross-reference findings across agents\n"
        "- Resolve contradictions by weighting evidence\n"
        "- Produce a single actionable conclusion with supporting evidence\n\n"
        "Output format:\n"
        "[CONCLUSION] One-paragraph summary of the answer\n"
        "[EVIDENCE] agent_name: key finding\n"
        "[RECOMMENDATION] What action to take next\n\n"
        "## Goal\n" + goal + "\n\n## Sub-Agent Outputs\n";

    for (size_t i = 0; i < sub_results.size(); ++i) {
      const auto& sr = sub_results[i];
      std::string agent_name = sr.value("role", sr.value("task_id", "agent_" + std::to_string(i)));
      std::string output = sr.value("output", sr.value("result", nlohmann::json::object()).dump());
      std::string ok_str = sr.value("ok", true) ? "OK" : "FAILED";
      synthesis_prompt += "\n### Agent: " + agent_name + " [" + ok_str + "]\n" + output + "\n";
    }

    synthesis_prompt += "\n\nNow synthesize these into a single conclusion.";

    // 使用 summarizer 角色或默认 AgentLoop
    std::string api_key;
    api_key = resolve_api_key(credential_pool_.get(), cfg_.api_key_env, cfg_.provider);

    agent::AgentLoopConfig syn_cfg;
    syn_cfg.max_turns = 3;
    syn_cfg.human_in_the_loop = false;
    // v0.52.17: 合成超时/预算对齐——默认 request_timeout_ms 仅 60s，
    // 合成 prompt 携带全部子代理输出（大）+GLM 慢（真 e2e 3 并发实测：
    // 波次完成后合成排队超时，客户端 540s 收不到帧）。与分解同款
    // 双侧 150s；max_completion_tokens 8192（思考型模型 reasoning
    // 与 content 共享预算）。
    syn_cfg.request_timeout_ms = 150000;
    auto syn_cloud_cfg = cfg_;
    syn_cloud_cfg.request_timeout_ms = 150000;
    syn_cloud_cfg.max_completion_tokens = 8192;

    auto syn_loop = std::make_unique<agent::AgentLoop>(syn_cloud_cfg, api_key,
                                                       agent::ToolRegistry::instance(), syn_cfg);
    auto syn_result = syn_loop->run(goal, synthesis_prompt);

    return {{"type", "synthesize_result"},
                   {"ok", syn_result.ok},
                   {"conclusion", syn_result.final_answer},
                   {"turns", syn_result.turns_used},
                   {"sub_agent_count", sub_results.size()},
                   {"error", syn_result.error}};
  }
  return nlohmann::json();  // 无匹配分支

  return nullptr;
}

nlohmann::json AgentService::handle_agent_orchestration(
    const std::string& session_id, const std::string& type,
    const nlohmann::json& req,
    const std::function<nlohmann::json(nlohmann::json)>& finish) {
  // ── v0.50.4: 编排域拆分（六族，返回 payload 由 finish 包装 meta）──
  if (nlohmann::json hit = orch_spawn(session_id, type, req); !hit.is_null()) {
    return finish(std::move(hit));
  }
  if (nlohmann::json hit = orch_agent_mail(session_id, type, req); !hit.is_null()) {
    return finish(std::move(hit));
  }
  if (nlohmann::json hit = orch_kanban(session_id, type, req); !hit.is_null()) {
    return finish(std::move(hit));
  }
  if (nlohmann::json hit = orch_debate_roles(session_id, type, req); !hit.is_null()) {
    return finish(std::move(hit));
  }
  if (nlohmann::json hit = orch_orchestrate_dag(session_id, type, req); !hit.is_null()) {
    return finish(std::move(hit));
  }
  if (nlohmann::json hit = orch_decompose(session_id, type, req); !hit.is_null()) {
    return finish(std::move(hit));
  }
  return nlohmann::json();
}

nlohmann::json AgentService::handle_request(const std::string& session_id, const nlohmann::json& req,
                                            StreamCallback on_chunk, EventCallback on_event) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto meta = extract_meta(req);
  const std::string type = req.value("type", "");

  // v0.50.3: 入口统一注入会话级项目上下文到 thread_local（RAII）。
  // 此前仅 type=="chat" 分支注入且不清理——①chat_approve/spawn_agent/goal 等
  // 含工具执行的分支无 ro/rw 约束（审批恰是危险写操作）；②worker 线程复用
  // 时残留旧会话上下文泄漏到下一请求。所有分支统一生效，析构自动清理。
  std::unique_ptr<ProjectContextGuard> project_guard;
  {
    PathValidator::ProjectContext pc;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = session_projects_.find(session_id);
      if (it != session_projects_.end() && it->second.valid) pc = it->second;
    }
    if (pc.valid) {
      project_guard = std::make_unique<ProjectContextGuard>(pc);
    } else {
      project_guard = std::make_unique<ProjectContextGuard>(nullptr);
    }
  }

  auto finish = [&](nlohmann::json payload) {
    const auto t1 = std::chrono::steady_clock::now();
    const auto dt = std::chrono::duration<double, std::milli>(t1 - t0).count();
    {
      std::lock_guard<std::mutex> lk(mu_);
      ++total_requests_;
      ++type_counters_[type.empty() ? "<empty>" : type];
      last_latency_ms_ = dt;
      total_latency_ms_ += dt;
    }

    if (payload.contains("decision_trace") && payload["decision_trace"].is_array()) {
      const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      const auto ts = std::to_string(now_ms);
      for (auto& step : payload["decision_trace"]) {
        if (step.is_object() && (!step.contains("ts") || !step["ts"].is_string() || step["ts"].get<std::string>().empty())) {
          step["ts"] = ts;
        }
      }
    }

    return with_meta(std::move(payload), meta);
  };

  if (type == "ping") {
    {
      std::lock_guard<std::mutex> lk(mu_);
      record_event("ping:" + session_id);  // v0.53.82: 单锁——record_event 规约=调用方持 mu_
    }
    return finish({{"type", "pong"}});
  }

  if (type == "status") {
    return finish({{"type", "status"}, {"data", status_payload()}});
  }

  // v0.52.30: Hook System 用户侧暴露
  if (type == "hook_register") {
    return finish(handle_hook_register(req));
  }
  if (type == "hook_unregister") {
    return finish(handle_hook_unregister(req));
  }
  if (type == "hook_list") {
    return finish(handle_hook_list());
  }

  // v0.47.1: token 用量统计查询
  if (type == "usage_stats") {
    return finish({{"type", "usage_stats"}, {"data", usage_stats()}});
  }

  // v0.47.3: 流式中断 — 用户中途喊停
  if (type == "chat_abort") {
    abort_chat(session_id);
    return finish({{"type", "chat_aborted"}, {"session_id", session_id}});
  }

  // v0.47.5: 缓存统计
  if (type == "cache_stats") {
    return finish({{"type", "cache_stats"}, {"data", response_cache_->stats()}});
  }

  // v0.48.0: 设置/切换当前会话的项目目录 + 读写模式
  if (type == "set_project") {
    std::string project_path = req.value("path", "");
    std::string mode = req.value("mode", "ro");
    if (project_path.empty()) {
      return finish({{"type", "error"}, {"message", "path required"}});
    }
    // 规范化项目路径
    std::string normalized = PathValidator::normalize(project_path);
    std::error_code ec;
    if (!std::filesystem::is_directory(normalized, ec)) {
      return finish({{"type", "error"}, {"message", "not_a_directory: " + normalized}});
    }
    PathValidator::ProjectContext pctx;
    pctx.project_root = normalized;
    pctx.read_only = (mode != "rw");
    pctx.valid = true;
    {
      std::lock_guard<std::mutex> lk(mu_);
      session_projects_[session_id] = pctx;
    }
    return finish({{"type", "project_set"},
                   {"root", normalized},
                   {"mode", pctx.read_only ? "ro" : "rw"}});
  }

  // v0.48.0: 只切换读写模式（不换目录）
  if (type == "set_project_mode") {
    std::string mode = req.value("mode", "");
    if (mode.empty()) {
      return finish({{"type", "error"}, {"message", "mode required (ro/rw)"}});
    }
    // v0.53.84(P0): 锁必须**先出作用域**再 finish——finish 自身也要锁 mu_,
    // 持锁调用它=同线程二次 lock 自死锁(get_project/set_project_mode 实测冻结
    // 全服务;gdb:mu_ __owner==worker 自身)。故锁内只组装 payload。
    nlohmann::json out;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = session_projects_.find(session_id);
      if (it == session_projects_.end() || !it->second.valid) {
        out = {{"type", "error"}, {"message", "no_project_set: call set_project first"}};
      } else {
        it->second.read_only = (mode != "rw");
        out = {{"type", "project_mode_set"},
               {"mode", it->second.read_only ? "ro" : "rw"},
               {"root", it->second.project_root}};
      }
    }
    return finish(std::move(out));
  }

  // v0.48.0: 查询当前会话的项目上下文
  if (type == "get_project") {
    nlohmann::json out;  // v0.53.84(P0): 同上——锁内只读、锁外 finish
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = session_projects_.find(session_id);
      if (it == session_projects_.end() || !it->second.valid) {
        out = {{"type", "project_info"}, {"valid", false}};
      } else {
        out = {{"type", "project_info"},
               {"valid", true},
               {"root", it->second.project_root},
               {"mode", it->second.read_only ? "ro" : "rw"}};
      }
    }
    return finish(std::move(out));
  }

  if (type == "chat") {
    // v0.53.21: 斜杠命令拦截——/ 开头且命中词表 → 本地处理零 LLM。
    // 词表外 /xxx 原样走 chat（LLM 自行应对）。
    std::string text = req.value("text", "");  // v0.53.23: /retry 可改写
    if (!text.empty() && text[0] == '/') {
      auto sc = slash_command(session_id, text);
      if (!sc.is_null()) {
        // /retry：改写 text 重放上一条 user 消息（走正常 chat 流程）
        if (sc.contains("__retry_text__")) {
          text = sc["__retry_text__"].get<std::string>();
          log_event("intent", LogLevel::Info,
                    "slash: /retry replay, len=" + std::to_string(text.size()));
        } else {
          sc["cmd_id"] = meta.cmd_id;  // 客户端请求关联
          return sc;
        }
      }
    }
    const auto chat_started = std::chrono::steady_clock::now();

    // v0.50.3: TLS 注入已提升到 handle_request 入口（RAII，全分支生效）
    auto chat_result = handle_chat(session_id, text, meta.cmd_id,
                                   on_chunk, on_event, req.value("image", ""));
    log_event("timing", LogLevel::Debug, "handle_chat returned");
    append_decision_audit(session_id, text, chat_result, chat_started);
    log_event("timing", LogLevel::Debug, "audit done");

    // v0.14: 将助手回复写入对话短期记忆（Phase 1 上下文连贯性）
    std::string reply_text = chat_result.value("text", "");
    if (!reply_text.empty()) {
      std::lock_guard<std::mutex> lk(mu_);
      auto& mem = session_chat_memory_[session_id];
      mem.push_back(ChatMessage{"assistant", reply_text});
      persist_session_msg(session_id, mem.back());  // v0.53.28 持久化
      // Token 预算裁剪（替代旧 memory_window_ 固定条数）
      trim_memory_by_token_budget(mem,
          static_cast<size_t>(ctx_cfg_.history_budget_tokens),
          ctx_cfg_.full_context_turns,
          static_cast<size_t>(ctx_cfg_.truncated_max_chars));
    }

    // 持久化 trace（静默失败不影响主流程）
    log_event("timing", LogLevel::Debug, "memory done, about to save_to_db");
    agent::AgentTracer::instance().save_to_db(default_traces_db_path());
    log_event("timing", LogLevel::Debug, "save_to_db done");
    agent::AgentTracer::instance().clear();

    // v0.27.6: 持久化会话消息到 SessionStore
    if (session_store_) {
      session_store_->record_message(session_id, "user", req.value("text", ""));
      session_store_->record_message(session_id, "assistant", reply_text);
      // 首条消息设标题
      session_store_->touch_session(session_id, req.value("text", "").substr(0, 80));
    }
    // v0.29.0: 同步录制 assistant 回复到对话摘要器
    if (summarizer_ && !reply_text.empty()) {
      summarizer_->add_message("assistant", reply_text);
    }

    return finish(std::move(chat_result));
  }

  if (type == "trace_list") {
    int limit = json_coerce_int(req, "limit", 20);
    return finish({{"type", "trace_list"}, {"traces", agent::AgentTracer::list_recent(limit, default_traces_db_path())}});
  }

  if (type == "chat_approve") {
    return finish(handle_chat_approve(session_id, req));
  }

  if (type == "action") {
    const std::string action = req.value("action", "");
    {
      std::lock_guard<std::mutex> lk(mu_);
      record_event("action:" + action);  // v0.53.82: 单锁
    }
    const nlohmann::json args = req.contains("args") ? req["args"] : nlohmann::json::object();
    return finish(handle_action(action, args));
  }

  if (type == "task_submit") {
    {
      std::lock_guard<std::mutex> lk(mu_);
      record_event("task_submit:" + req.value("action", ""));  // v0.53.82: 单锁
    }
    return finish(handle_task_submit(req));
  }

  if (type == "task_get") {
    if (!task_engine_) return finish({{"type", "error"}, {"message", "task_engine not ready"}});
    const std::string task_id = req.value("task_id", "");
    return finish({{"type", "task_get_result"}, {"task", task_engine_->get_task(task_id)}});
  }

  if (type == "task_list") {
    if (!task_engine_) return finish({{"type", "error"}, {"message", "task_engine not ready"}});
    const int limit = json_coerce_int(req, "limit", 20);
    const std::string state = req.value("state", "");
    const std::string action = req.value("action", "");
    int limit_applied = limit;
    const auto tasks = task_engine_->list_tasks(limit, state, action, &limit_applied);
    return finish({{"type", "task_list_result"},
                   {"filters", {{"state", state}, {"action", action}}},
                   {"limit_applied", limit_applied},
                   {"returned_count", static_cast<int>(tasks.size())},
                   {"tasks", tasks}});
  }

  if (type == "task_cancel") {
    {
      std::lock_guard<std::mutex> lk(mu_);
      record_event("task_cancel:" + req.value("task_id", ""));  // v0.53.82: 单锁
    }
    return finish(handle_task_cancel(req));
  }

  if (type == "task_replay") {
    {
      std::lock_guard<std::mutex> lk(mu_);
      record_event("task_replay:" + req.value("task_id", ""));  // v0.53.82: 单锁
    }
    return finish(handle_task_replay(req));
  }

  if (type == "task_audit") {
    return finish(handle_task_audit(req));
  }

  if (type == "memory_recent") {
    return finish(memory_recent_payload(session_id));
  }

  if (type == "memory_history") {
    const int limit = json_coerce_int(req, "limit", 20);
    return finish(memory_history_payload(limit));
  }

  if (type == "memory_search") {
    const std::string query = req.value("query", std::string());
    const int limit = json_coerce_int(req, "limit", 20);
    return finish(memory_search_payload(query, limit));
  }

  if (type == "memory_summary") {
    const std::string query = req.value("query", std::string());
    const int limit = json_coerce_int(req, "limit", 20);
    return finish(memory_summary_payload(query, limit));
  }

  if (type == "event_recent") {
    return finish(event_recent_payload());
  }

  if (type == "metrics") {
    auto payload = metrics_payload();
    auto& data = payload["data"];
    if (!data.contains("by_type") || !data["by_type"].is_object()) {
      data["by_type"] = nlohmann::json::object();
    }
    auto& by_type = data["by_type"];
    int metrics_count = 0;
    if (by_type.contains("metrics") && by_type["metrics"].is_number_integer()) {
      metrics_count = by_type["metrics"].get<int>();
    }
    by_type["metrics"] = metrics_count + 1;

    if (data.contains("type_count") && data["type_count"].is_number_integer()) {
      if (metrics_count == 0) {
        data["type_count"] = data["type_count"].get<int>() + 1;
      }
    }

    if (data.contains("total_requests") && data["total_requests"].is_number_integer()) {
      data["total_requests"] = data["total_requests"].get<int>() + 1;
    }

    return finish(std::move(payload));
  }

  // --- checkpoint / cron / skills / summarize WS API ---

  if (type == "checkpoint_save") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("checkpoint_save", req);
    if (r.contains("success")) { r["type"] = "checkpoint_saved"; return finish(r); }
    return finish({{"type", "error"}, {"message", "checkpoint_save unavailable"}});
  }

  if (type == "checkpoint_rollback") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("checkpoint_rollback", req);
    if (r.contains("success")) { r["type"] = "checkpoint_rollback"; return finish(r); }
    return finish({{"type", "error"}, {"message", "checkpoint_rollback unavailable"}});
  }

  if (type == "checkpoint_list") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("checkpoint_list", req);
    if (r.contains("success")) { r["type"] = "checkpoint_list"; return finish(r); }
    return finish({{"type", "error"}, {"message", "checkpoint_list unavailable"}});
  }

  if (type == "cron_add") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("cron_add", req);
    if (r.contains("task")) { r["type"] = "cron_added"; return finish(r); }
    return finish({{"type", "error"}, {"message", "cron_add unavailable"}});
  }

  if (type == "cron_list") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("cron_list", req);
    if (r.contains("success")) { r["type"] = "cron_list"; return finish(r); }
    return finish({{"type", "error"}, {"message", "cron_list unavailable"}});
  }

  if (type == "cron_remove") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("cron_remove", req);
    if (r.contains("success")) { r["type"] = "cron_removed"; return finish(r); }
    return finish({{"type", "error"}, {"message", "cron_remove unavailable"}});
  }

  if (type == "cron_stats") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("cron_stats", req);
    if (r.contains("success")) { r["type"] = "cron_stats"; return finish(r); }
    return finish({{"type", "error"}, {"message", "cron_stats unavailable"}});
  }

  // v0.42.3: cron_reply — 用户回复 cron 任务
  if (type == "cron_reply") {
    if (!cron_scheduler_) return finish({{"type", "error"}, {"message", "not available"}});
    int task_id = req.value("task_id", 0);
    if (task_id <= 0) return finish({{"type", "error"}, {"message", "task_id required"}});
    auto it = cron_session_contexts_.find(task_id);
    if (it == cron_session_contexts_.end())
      return finish({{"type", "error"}, {"message", "task context not found"}});
    std::string user_text = req.value("text", "");
    if (user_text.empty()) return finish({{"type", "error"}, {"message", "text required"}});
    auto& ctx = it->second;
    std::string cron_prompt = ctx.value("prompt", "");
    std::string cron_result = ctx.value("result", "");
    std::string final_prompt = "[Cron task context]\nTask: " + ctx.value("task_name", "")
        + "\nPrompt: " + cron_prompt
        + "\nPrevious result: " + utf8_truncate(cron_result, 2000)
        + "\n\nUser reply: " + user_text;
    auto result = handle_chat("cron_reply_" + std::to_string(task_id), final_prompt);
    // v0.53.36: 用完即弃——cron reply session 不 close,session_chat_memory_
    // 随每次触发追加 assistant 回复无限缓涨(高频 cron 长跑数千条)
    {
      std::lock_guard<std::mutex> lk(mu_);
      session_chat_memory_.erase("cron_reply_" + std::to_string(task_id));
      slot_states_.erase("cron_reply_" + std::to_string(task_id));
      dialog_states_.erase("cron_reply_" + std::to_string(task_id));
    }
    return finish({{"type", "cron_replied"}, {"result", result}});
  }

  if (type == "skill_list") {
    if (!skill_manager_) return finish({{"type", "error"}, {"message", "not available"}});
    auto skills = skill_manager_->list_skills();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& sk : skills) {
      std::string state_str;
      switch (sk.state) {
        case agent::SkillState::active: state_str = "active"; break;
        case agent::SkillState::stale: state_str = "stale"; break;
        case agent::SkillState::archived: state_str = "archived"; break;
      }
      arr.push_back({{"id", sk.id}, {"name", sk.name}, {"use_count", sk.use_count},
                     {"state", state_str}, {"created_at", sk.created_at},
                     {"last_used_at", sk.last_used_at}});
    }
    return finish({{"type", "skill_list"}, {"skills", arr}});
  }

  if (type == "skill_stats") {
    if (!skill_manager_) return finish({{"type", "error"}, {"message", "not available"}});
    return finish({{"type", "skill_stats"}, {"stats", skill_manager_->stats()}});
  }

  if (type == "skill_maintain") {
    if (!skill_manager_) return finish({{"type", "error"}, {"message", "not available"}});
    int stale = req.value("stale_days", 30);
    int archive = req.value("archive_days", 60);
    return finish({{"type", "skill_maintained"}, {"result", skill_manager_->auto_maintain(stale, archive)}});
  }

  if (type == "correction_record") {
    // v0.53.1: 转发 meta 插件（单一事实源）
    auto r = skill_registry_.dispatch_cpp("correction_record", req);
    if (r.contains("success")) { r["type"] = "correction_record_result"; return finish(r); }
    return finish({{"type", "error"}, {"message", r.value("error", "correction_record unavailable")}});
  }

  if (type == "correction_stats") {
    if (!correction_store_) return finish({{"type", "error"}, {"message", "not available"}});
    return finish({{"type", "correction_stats"}, {"stats", correction_store_->stats()}});
  }

  // v0.10.4: ProactiveMonitor
  if (type == "monitor_watch_file") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("monitor_watch_file", req);
    if (r.contains("success")) { r["type"] = "monitor_watching"; return finish(r); }
    return finish({{"type", "error"}, {"message", "monitor_watch_file unavailable"}});
  }

  if (type == "monitor_start") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("monitor_start", req);
    if (r.contains("success")) { r["type"] = "monitor_started"; return finish(r); }
    return finish({{"type", "error"}, {"message", "monitor_start unavailable"}});
  }

  if (type == "monitor_stop") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("monitor_stop", req);
    if (r.contains("success")) { r["type"] = "monitor_stopped"; return finish(r); }
    return finish({{"type", "error"}, {"message", "monitor_stop unavailable"}});
  }

  if (type == "monitor_status") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("monitor_status", req);
    if (r.contains("status")) { r["type"] = "monitor_status"; return finish(r); }
    return finish({{"type", "error"}, {"message", "monitor_status unavailable"}});
  }

  // v0.11.0: 角色化子 Agent 生成
  // v0.49.1: agent 编排族拆分至 handle_agent_orchestration（约 790 行 → 6 行调用）
  {
    auto r = handle_agent_orchestration(session_id, type, req, finish);
    if (!r.is_null()) return r;
  }

  // v0.9.4: 长期目标管理
  if (type == "goal_add") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("goal_add", req);
    if (r.contains("success")) { r["type"] = "goal_add_result"; return finish(r); }
    return finish({{"type", "error"}, {"message", "goal_add unavailable"}});
  }
  if (type == "goal_list") {
    // v0.53.0: 转发插件 handler（单一事实源）。
    // v0.53.3: 无插件进程（单测）或插件实例未注入时回退核心直查——
    // 此前转发失败直接报错，单测环境（不加载 .so）下 goal_list 断裂
    auto r = skill_registry_.dispatch_cpp("goal_list", req);
    if (r.value("success", false)) {
      r["type"] = "goal_list_result";
      return finish(r);
    }
    if (!goal_mgr_)
      return finish({{"type", "goal_list_result"}, {"goals", nlohmann::json::array()}});
    std::string filter = req.value("status", "");
    auto goals = goal_mgr_->list(filter);
    nlohmann::json arr = nlohmann::json::array();
    for (auto& g : goals)
      arr.push_back({{"id", g.id}, {"description", g.description},
                     {"status", g.status}, {"progress_pct", g.progress_pct},
                     {"parent_id", g.parent_id}});
    return finish({{"type", "goal_list_result"}, {"goals", arr}});
  }
  if (type == "goal_update") {
    // v0.53.0: 转发插件 handler（单一事实源）
    auto r = skill_registry_.dispatch_cpp("goal_update", req);
    if (r.contains("success")) { r["type"] = "goal_update_result"; return finish(r); }
    return finish({{"type", "error"}, {"message", "goal_update unavailable"}});
  }
  if (type == "goal_delete") {
    int id = req.value("id", 0);
    if (!goal_mgr_ || id <= 0) return finish({{"type", "goal_delete_result"}, {"ok", false}});
    bool ok = goal_mgr_->remove(id);
    return finish({{"type", "goal_delete_result"}, {"ok", ok}});
  }

  // v0.10.3: 目标驱动主动推理
  if (type == "goal_auto_reason") {
    // v0.53.99: goal_auto_reason() 现返回对象（含 results/processed/total/truncated/
    // remaining）。这里**保持 results 仍是数组**（不改既有响应契约），并把截断信息平铺上来。
    {
      auto ar = goal_auto_reason();
      nlohmann::json resp;
      resp["type"] = "goal_auto_reason_result";
      resp["results"] = ar.contains("results") ? ar["results"] : nlohmann::json::array();
      resp["processed"] = ar.value("processed", 0);
      resp["total_goals"] = ar.value("total_goals", 0);
      resp["truncated"] = ar.value("truncated", false);
      resp["remaining"] = ar.value("remaining", 0);
      return finish(resp);
    }
  }

  if (type == "goal_proactive_enable") {
    if (!cron_scheduler_ || !goal_mgr_)
      return finish({{"type", "error"}, {"message", "not available"}});
    int interval = req.value("interval_minutes", 30);
    auto t = cron_scheduler_->add_task("goal_proactive_drive",
        "every " + std::to_string(interval) + "m",
        "[INTERNAL] goal_auto_reason trigger");
    return finish({{"type", "goal_proactive_enabled"}, {"task", t}});
  }

  // v0.9.2: 热切换本地模型
  if (type == "switch_model") {
    const std::string model_name = req.value("model", "");
    const std::string gguf_path = req.value("gguf_path", "");
    if (model_name.empty() || gguf_path.empty()) {
      return finish({{"type", "switch_model_result"}, {"ok", false}, {"error", "missing model or gguf_path"}});
    }
    bool ok = thin_agent::local::ModelPool::instance().reload(model_name, gguf_path);
    return finish({{"type", "switch_model_result"}, {"ok", ok}, {"model", model_name}, {"gguf_path", gguf_path}});
  }

  // ── v0.53.10: web_search 数据源运行时配置（读取/切换+落盘持久化）──
  // set 模式：web_search_config {set: {provider: "bing"|"duckduckgo"|"mock", ...}}
  //   合法 provider 词表校验后写 chat_policy.json 的 web_search 段并重置策略缓存
  //   （下一轮 fetch_search 即生效，无需重启）。
  // get 模式：web_search_config {} 返回当前配置段。
  if (type == "web_search_config") {
    const std::string path = chat_policy_path();
    nlohmann::json policy;
    {  // 读盘（不经缓存——避免缓存态与盘态漂移）
      std::ifstream f(path);
      if (!f.is_open())
        return finish({{"type", "web_search_config_result"}, {"ok", false},
                       {"error", "policy_file_unreadable"}, {"path", path}});
      try { f >> policy; } catch (...) {
        return finish({{"type", "web_search_config_result"}, {"ok", false},
                       {"error", "policy_file_invalid_json"}, {"path", path}});
      }
    }
    if (!req.contains("set") || !req["set"].is_object()) {
      // get：返回当前 web_search 段
      nlohmann::json cur = policy.contains("web_search") && policy["web_search"].is_object()
                               ? policy["web_search"] : nlohmann::json::object();
      return finish({{"type", "web_search_config_result"}, {"ok", true},
                     {"mode", "get"}, {"config", cur}, {"path", path}});
    }
    const auto& setv = req["set"];
    // provider 合法性：词表（与 ExternalInfoClient::fetch_search 支持集一致）
    static const std::vector<std::string> kProviders = {"bing", "duckduckgo", "mock", "zai"};
    if (setv.contains("provider")) {
      // v0.53.15: 类型防御——数字/数组等非 string 值会让 value<string> 抛
      // type_error.302（真审计实测 provider:123 无响应无错误帧，异常逃逸连接层）。
      std::string p;
      if (setv["provider"].is_string()) p = setv["provider"].get<std::string>();
      if (std::find(kProviders.begin(), kProviders.end(), p) == kProviders.end())
        return finish({{"type", "web_search_config_result"}, {"ok", false},
                       {"error", "invalid_provider"}, {"provider", p},
                       {"valid", kProviders}});
    }
    // 应用到 web_search 段（浅合并：只覆盖 set 里出现的键）
    nlohmann::json ws = policy.contains("web_search") && policy["web_search"].is_object()
                            ? policy["web_search"] : nlohmann::json::object();
    for (auto it = setv.begin(); it != setv.end(); ++it) ws[it.key()] = it.value();
    policy["web_search"] = ws;
    // 落盘（缩进 2 保持人可编辑）
    std::ofstream of(path);
    if (!of.is_open())
      return finish({{"type", "web_search_config_result"}, {"ok", false},
                     {"error", "policy_file_unwritable"}, {"path", path}});
    of << policy.dump(2) << "\n";
    chat_policy_reset();  // 下次 chat_policy() 重读盘→立即生效
    log_event("web-search", LogLevel::Info, "config updated",
              {{"path", path}, {"keys", setv.size()}});
    return finish({{"type", "web_search_config_result"}, {"ok", true}, {"mode", "set"},
                   {"applied", setv}, {"path", path}});
  }

  return finish({{"type", "error"}, {"message", "unknown type"}});
}

}  // namespace thin_agent
