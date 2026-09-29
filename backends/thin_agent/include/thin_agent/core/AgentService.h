#pragma once

#include <deque>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "thin_agent/core/CredentialPool.h"
#include "thin_agent/core/WebhookClient.h"
#include "thin_agent/core/ResponseCache.h"
#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/IntentScorer.h"
#include "thin_agent/llm/CloudLlmClient.h"   // ChatMessage
#include "thin_agent/core/TaskEngine.h"
#include "thin_agent/llm/DemoConfigCompat.h"
#include "thin_agent/agent/MemoryManager.h"
#include "thin_agent/agent/AgentLoop.h"
#include "thin_agent/agent/AgentRole.h"
#include "thin_agent/agent/ConversationSummarizer.h"
#include "thin_agent/agent/CheckpointManager.h"
#include "thin_agent/agent/FilesystemCheckpoint.h"
#include "thin_agent/agent/SkillManager.h"
#include "thin_agent/agent/GoalManager.h"
#include "thin_agent/agent/ErrorCorrectionStore.h"
#include "thin_agent/core/CronScheduler.h"
#include "thin_agent/core/AgentOrchestrator.h"
#include "thin_agent/core/ProactiveMonitor.h"
#include "thin_agent/core/SubAgentBus.h"
#include "thin_agent/core/Blackboard.h"
#include "thin_agent/core/KbSearcher.h"
#include "thin_agent/core/KanbanBoard.h"
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/core/FactStore.h"
#include "thin_agent/core/SessionStore.h"
#include "thin_agent/core/FcRunStore.h"
#include "thin_agent/plugin/PluginContext.h"  // v0.53.0
#include "thin_agent/core/HookSystem.h"  // v0.52.30
#include "thin_agent/core/PatrolProbe.h"
#include "thin_agent/core/PathValidator.h"
#include "thin_agent/llm/CurlHttpClient.h"
#include "thin_agent/RuntimePaths.h"

namespace thin_agent {

/// 流式输出回调：chunk 为增量文本片段，done=true 表示流结束。
using StreamCallback = std::function<void(const std::string& chunk, bool done)>;

/// 事件推送回调：用于 thinking / progress 等中间状态，前端可据此渲染多级进度。
/// event_type 例如 "thinking"，data 为 JSON payload（如 {"tier":1,"msg":"本地分析中..."}）。
using EventCallback = std::function<void(const std::string& event_type, const nlohmann::json& data)>;

/// Agent 核心服务：处理 WebSocket/HTTP 请求，编排本地规则、槽位澄清、外部查询与云端策略。
/// 对话文案与关键词匹配优先走 ChatPolicy 配置，C++ 仅保留路由与契约逻辑。
class AgentService {
 public:
  AgentService(DemoConfigCompat cfg,
               std::shared_ptr<ActionExecutor> executor,
               std::shared_ptr<TaskEngine> task_engine);
  ~AgentService();  // v0.53.4: 后台线程存活门闩落闸

  /// 会话建立时初始化该 session 的短期记忆容器。
  void on_session_open(const std::string& session_id);

  /// v0.53.28: 会话记忆持久化（正式客户端配套——服务重启聊天不丢）。
  /// 落盘 sessions/<sanitized_sid>.jsonl（追加写）；on_session_open 内存
  /// 空时回灌（cap 200 条）；reset/clear 删除文件。静默失败不影响主流程。
  void persist_session_msg(const std::string& session_id, const ChatMessage& msg);
  void restore_session_msgs(const std::string& session_id);
  void drop_session_file(const std::string& session_id);
  std::string session_file_path(const std::string& session_id) const;

  /// v0.53.21: 斜杠命令（/new /reset /clear /help /status）——本地处理，
  /// 零 LLM 调用。词表与中文文案外置 chat_policy（slash_commands 段）。
  /// @return 响应 json；未命中词表返回 null json（调用方继续正常 chat 流程）
  nlohmann::json slash_command(const std::string& session_id,
                               const std::string& text);

  /// 会话关闭时清理槽位与对话上下文状态。
  void on_session_close(const std::string& session_id);

  /// v0.53.0: 插件服务面（窄接口注入；broadcast 回调供插件广播）
  friend class AgentServicePluginContext;
  std::unique_ptr<PluginContext> plugin_ctx_;

  /// v0.52.30: Hook System 用户侧暴露 —— WS handler 三件套
  /// hook_register: {type, event, shell_cmd|callback_name, tool_filter?,
  ///                 timeout_ms?} → {hook_id}
  /// hook_unregister: {hook_id} → {removed}
  /// hook_list: {} → 已注册钩子清单（脱敏，不含回调体）
  nlohmann::json handle_hook_register(const nlohmann::json& req);
  nlohmann::json handle_hook_unregister(const nlohmann::json& req);
  nlohmann::json handle_hook_list();

  /// v0.25.9: 设置广播回调（用于 cron 通知等异步事件推送到所有 WS 客户端）
  void set_broadcast_callback(std::function<void(const nlohmann::json&)> cb) { broadcast_cb_ = std::move(cb); }

  /// 返回握手/能力声明 JSON。
  nlohmann::json hello(const std::string& session_id) const;

  /// ── Agent 工具访问接口（供 ToolSchema 回调使用）─────────────────
  /// 返回 status_payload（运行状态、模式、provider 等）。
  nlohmann::json agent_tool_status() const;
  /// 返回指定 session 的短期记忆。
  nlohmann::json agent_tool_memory_recent(const std::string& session_id) const;
  /// 搜索长期记忆（JSONL + task SQLite）。
  nlohmann::json agent_tool_memory_search(const std::string& query, int limit) const;
  /// 返回近期感知事件列表。
  nlohmann::json agent_tool_event_recent() const;

  /// ── 新组件访问接口 ─────────────────────
  agent::ConversationSummarizer* summarizer() { return summarizer_.get(); }
  agent::GoalManager* goal_mgr() { return goal_mgr_.get(); }
  agent::ErrorCorrectionStore* correction_store() { return correction_store_.get(); }

  /// v0.10.3: 目标驱动主动推理。
  /// 检测滞停目标，为每个生成推理提示并提交到 chat 管线。
  /// @return 每个目标的处理结果数组。
  nlohmann::json goal_auto_reason();
  agent::CheckpointManager* checkpoint_mgr() { return checkpoint_mgr_.get(); }
  agent::FilesystemCheckpoint* fs_checkpoint_mgr() { return fs_checkpoint_mgr_.get(); }
  agent::SkillManager* skill_manager() { return skill_manager_.get(); }
  agent::AgentRoleManager* role_manager() { return role_mgr_.get(); }
  SubAgentBus* agent_bus() { return agent_bus_.get(); }
  Blackboard* blackboard() { return blackboard_.get(); }
  KanbanBoard* kanban() { return kanban_.get(); }
  CronScheduler* cron_scheduler() { return cron_scheduler_.get(); }
  AgentOrchestrator* orchestrator() { return orchestrator_.get(); }
  KbSearcher* kb_searcher() { return kb_searcher_.get(); }
  FactStore* fact_store() { return fact_store_.get(); }
  SessionStore* session_store() { return session_store_.get(); }

  /// 统一请求入口：按 type 分发到 chat / action / task / 感知等处理器。
  /// @param on_chunk 流式输出回调（可选），逐 token 推送增量文本。
  /// @param on_event 事件推送回调（可选），用于 thinking / progress 中间状态。
  nlohmann::json handle_request(const std::string& session_id, const nlohmann::json& req,
                                StreamCallback on_chunk = nullptr,
                                EventCallback on_event = nullptr);

  /// v0.29.0: 非阻塞 webhook 入队 — 立即返回，不调用 LLM。
  /// Webhook 事件会在下次 handle_chat 时自动注入为上下文。
  void enqueue_webhook(const std::string& source,
                       const std::string& event,
                       const std::string& payload);

  /// v0.29.0: 返回 SkillRegistry 引用（供 MCP Server 使用）。
  SkillRegistry& skill_registry() { return skill_registry_; }

  /// v0.45.4: 判断归一化文本是否为文件操作类请求（路径 + 操作词）。
  /// 规则引擎用其排除 profile 误判（路径/内容可能含 thin_agent 等
  /// 关键词子串）。独立成静态方法便于单元测试。
  static bool is_file_op_request(const std::string& norm);

  /// v0.47.1: 查询累计 token 用量统计（线程安全）。
  /// @return {prompt_tokens, completion_tokens, total_tokens, api_calls}
  nlohmann::json usage_stats() const;

  /// v0.50.8: 运维汇总统计（/stats HTTP 口数据源：version/uptime/usage/cache/cron）
  nlohmann::json ops_stats() const;

  /// v0.53.35: Prometheus 文本格式导出（/metrics 端点数据源）。
  /// 指标前缀 thin_agent_——uptime/usage/cache/cron/ws 队列/会话/熔断。
  std::string metrics_prometheus() const;

  /// v0.47.3: 中断指定 session 的 chat 请求（用户中途喊停）。
  /// FC 循环每轮迭代开始检查，已执行的工具结果不丢失（返回部分结果）。
  void abort_chat(const std::string& session_id);

  /// v0.53.25: 宿主层 WS 队列统计注入（ws_agent_main 注册回调：队列深度/
  /// worker 数/累计丢弃——/stats 聚合用；核心不反向依赖宿主层）。
  void set_ws_stats_provider(std::function<nlohmann::json()> fn) {
    ws_stats_provider_ = std::move(fn);
  }

  /// v0.47.3: 检查并清除中断标志（FC 循环内部调用，private）
  bool check_and_clear_abort(const std::string& session_id);

 private:
  /// 请求元数据：用于 trace 与 cmd 关联。
  struct RequestMeta {
    std::string cmd_id;    ///< 命令 ID，未提供时自动生成
    std::string trace_id;  ///< 链路追踪 ID
  };

  /// v0.50.0: handle_chat 跨域共享的请求级上下文（handle_chat 拆分为
  /// 多个私有域方法后，各域通过引用读写同一份状态，替代原函数局部变量。
  /// 字段语义与拆分前的同名局部变量一一对应，求值时序由编排顺序保证）。
  struct ChatContext {
    // ── 输入与派生文本 ──
    std::string session_id;
    std::string text;            ///< 原始用户输入（翻译不改写，多语 LLM 使用）
    std::string effective_text;  ///< webhook 前缀注入后的文本（记忆快照用）
    std::string text_lower;      ///< 小写化（输入翻译后为英文），关键词匹配用
    std::string cmd_id;
    std::string idem_token;
    std::string image_base64;

    // ── 会话与语言 ──
    std::vector<ChatMessage> snapshot;  ///< 会话记忆快照（含本轮 user）
    DialogContext dialog_ctx;
    std::string query_lang;
    std::string tpl_lang;
    bool use_zh_progress = true;

    // ── 意图分类结果（rules→fuzzy→ONNX 级联后的终值，域方法可读）──
    nlohmann::json intent_info;      ///< 分类结果（含 intent/confidence/backend/slots）
    std::string classified_intent;   ///< intent_info["intent"]（供 triggered 判定）
    bool model_status_query = false; ///< is_model_runtime_query(text_lower)

    // ── 复杂意图探测 ──
    nlohmann::json probe_markers;     ///< 命中的复杂意图标记
    bool complex_intent_detected = false;
    bool force_cloud_for_complex = false;

    // ── 回调 ──
    StreamCallback on_chunk;
    EventCallback on_event;
  };

  /// v0.50.0: 统一本地意图回复组装（原 handle_chat 内 local_reply lambda）。
  nlohmann::json make_local_reply(ChatContext& ctx,
                                  const std::string& route,
                                  const std::string& reason,
                                  const std::string& reply,
                                  nlohmann::json observation,
                                  nlohmann::json tool_calls = nlohmann::json::array(),
                                  const std::string& intent = "",
                                  double confidence = 1.0,
                                  nlohmann::json slots = nlohmann::json::object(),
                                  const std::string& policy = "execute");

  /// v0.50.0: 输入翻译域（原 handle_chat 段 A）。
  void chat_translate_input(ChatContext& ctx);

  /// v0.50.0: 云前置域（原 handle_chat 段 G1）——budget gate + key 解析 +
  /// offline/missing-key/auto 级联提前终结。返回 null 表示继续云端；
  /// provider/key_state/api_key 为出参。
  nlohmann::json chat_cloud_precheck(ChatContext& ctx,
                                     std::string& provider,
                                     std::string& key_state,
                                     std::string& api_key);

  /// v0.50.0: 云策略 pipeline 路由域（原 handle_chat 段 H）——strategy JSON
  /// 处理 + task pipeline 执行 + 本地 route_hint 落地。命中返回最终回复，
  /// 未命中返回 null（调用方走通用回答兜底）。
  nlohmann::json chat_route_cloud_strategy(ChatContext& ctx,
                                           const CloudChatResult& cloud,
                                           const std::string& key_state,
                                           const std::string& provider);

  /// v0.50.0: 云失败离线回退域（原 handle_chat 段 I / cloud_fallback 标签）。
  nlohmann::json chat_offline_fallback(ChatContext& ctx,
                                       const CloudChatResult& cloud,
                                       const std::string& key_state,
                                       const std::string& provider);

  /// v0.50.0: 云端执行域（原 handle_chat 段 G2）的产出。
  /// final 非空=已得到终态回复；need_fallback=云调用失败需离线回退；
  /// continue_pipeline=成功但需传统 pipeline JSON 路径继续。
  struct ChatCloudResult {
    nlohmann::json final;          ///< 终态回复（非 null 时调用方直接返回）
    bool need_fallback = false;    ///< 云失败 → 调用方走 cloud_fallback
    bool continue_pipeline = false;///< 需 pipeline JSON 处理（cloud 携带结果）
    CloudChatResult cloud;         ///< 云调用原始结果
  };

  /// v0.50.0: 云端执行域（原 handle_chat 段 G2）。
  ChatCloudResult chat_run_cloud(ChatContext& ctx,
                                               const std::string& api_key,
                                               const std::string& key_state,
                                               const std::string& provider);

  /// v0.50.0: 任务/动作/gate 意图域（原 handle_chat 段 F）——task 内联提交 +
  /// action 直接执行 + gate 预算澄清。命中返回最终回复，未命中返回 null。
  nlohmann::json chat_route_task_action_gate(ChatContext& ctx);

  /// v0.50.0: 外部意图路由域（原 handle_chat 段 D）——配置驱动的外部意图
  /// 执行 + external_clarify。命中返回最终回复，未命中返回 null。
  nlohmann::json chat_route_external_intents(ChatContext& ctx);

  /// v0.50.0: 媒体计划域（原 handle_chat 段 E）——话轮门控 + pipeline 执行。
  /// 命中返回最终回复，未命中返回 null。
  nlohmann::json chat_route_media_plan(ChatContext& ctx);

  /// v0.50.0: 本地意图路由域（原 handle_chat 段 C）——配置驱动的本地意图
  /// 循环。命中返回最终回复，未命中返回 null。
  nlohmann::json chat_route_local_intents(ChatContext& ctx);

  /// v0.50.0: 意图分类域（原 handle_chat 段 B）——rules→fuzzy→ONNX 三级
  /// 级联 + 复杂意图探测 + 多轮槽位续接。槽位续接命中返回最终回复，
  /// 否则返回 null（调用方继续后续路由域）。
  nlohmann::json chat_classify_intent(ChatContext& ctx);

  /// 思考进度推送（原 push_thinking lambda）。
  void chat_push_thinking(const ChatContext& ctx, int tier, const std::string& key,
                          const std::vector<std::pair<std::string, std::string>>& params = {}) const;

  /// 多轮槽位澄清状态：用户补充 city/topic 等后可续接执行 pending intent。
  struct SlotState {
    std::string pending_intent;              ///< 待完成的意图，如 weather / news / weather_advice
    nlohmann::json filled_slots;             ///< 已填槽位，如 {"city": "上海"}
    std::vector<std::string> missing_slots;  ///< 仍缺失的槽位名列表
    nlohmann::json session_snapshot;         ///< 进入澄清时的会话快照
    int clarify_turns{0};                    ///< 已澄清轮数
    static constexpr int kMaxClarifyTurns = 3;  ///< 超过则放弃槽位状态
  };

  /// 返回 status 请求体：运行模式、provider、允许动作列表与 sense 摘要。
  nlohmann::json status_payload() const;
  /// 返回感知侧数据：近期事件环形缓冲与长期记忆文件状态。
  nlohmann::json sense_payload() const;
  /// 返回指定 session 的短期对话记忆窗口。
  nlohmann::json memory_recent_payload(const std::string& session_id) const;
  /// 从 jsonl 长期记忆文件读取最近 limit 条历史。
  nlohmann::json memory_history_payload(int limit) const;
  /// 在 jsonl 与任务库中检索记忆/任务证据，支持 query 子串过滤。
  nlohmann::json memory_search_payload(const std::string& query, int limit) const;
  /// 聚合记忆检索结果并生成维护结论与证据摘要。
  nlohmann::json memory_summary_payload(const std::string& query, int limit) const;
  /// 返回近期感知事件列表。
  nlohmann::json event_recent_payload() const;
  /// 返回服务指标：请求计数、类型分布与延迟统计。
  nlohmann::json metrics_payload() const;

  /// 对话主流程：规则意图 → 本地工具 → 外部查询 → 云端策略顾问。
  /// @param cmd_id 请求命令 ID，写入任务幂等键，避免同 session 重复指令误复用旧任务。
  /// @param on_chunk 流式输出回调（可选），逐 token 推送增量文本。
  /// @param on_event 事件推送回调（可选），用于 thinking / progress 中间状态。
  nlohmann::json handle_chat(const std::string& session_id,
                             const std::string& text,
                             const std::string& cmd_id = "",
                             StreamCallback on_chunk = nullptr,
                             EventCallback on_event = nullptr,
                             const std::string& image_base64 = "");

  /// 通用 Skill Pipeline 执行：task_pipeline → 步骤循环 → shell 校验 → 结果汇总
  nlohmann::json execute_skill_pipeline(
      const nlohmann::json& pipeline,
      const std::string& session_id,
      const std::string& idem_token,
      EventCallback on_event);

  /// 端云协作汇总路由：简单结果 → 本地模板，复杂/失败结果 → 云 LLM 汇总
  std::string summarize_pipeline_result(const nlohmann::json& exec_result,
                                        const std::string& user_text,
                                        const std::string& idem_token,
                                        EventCallback on_event,
                                        bool use_zh_progress = true);

  /// 注册内置 C++ handler（write_file、media actions 等）到 skill_registry_
  void register_cpp_handlers();
  /// v0.49.1: register_cpp_handlers 按域拆分的 5 个子方法
  void register_file_shell_tools();        ///< write_file/shell_exec/process/search_files
  void register_media_agent_tools();       ///< capture/spawn_agent/bb_*/monitor_*/agent_dag
  void register_cron_tools();              ///< cron_*
  void register_skill_checkpoint_tools();  ///< skill_*/checkpoint_*/correction/goal_*/kanban_*
  void register_meta_memory_tools();       ///< summarize/kb_*/memory_*/session_*/find_tool/patrol_*
  void register_web_search_tool();         ///< v0.53.11: web_search（FC 自主搜索，auto 决策链）

  /// v0.49.1: agent 编排族 WS API 分发（从 handle_request 拆出）。
  /// @return 匹配并处理的响应；无匹配返回 null（调用方继续尝试其余 type）
  /// v0.50.4: 编排域拆分——spawn/mail/kanban/debate 四族。
  /// 返回 payload（null=未命中），调用方 finish 包装 meta。
  nlohmann::json orch_spawn(const std::string& session_id,
                            const std::string& type,
                            const nlohmann::json& req);
  nlohmann::json orch_agent_mail(const std::string& session_id,
                                 const std::string& type,
                                 const nlohmann::json& req);
  nlohmann::json orch_kanban(const std::string& session_id,
                             const std::string& type,
                             const nlohmann::json& req);
  nlohmann::json orch_debate_roles(const std::string& session_id,
                                   const std::string& type,
                                   const nlohmann::json& req);
  nlohmann::json orch_orchestrate_dag(const std::string& session_id,
                                      const std::string& type,
                                      const nlohmann::json& req);
  nlohmann::json orch_decompose(const std::string& session_id,
                                const std::string& type,
                                const nlohmann::json& req);

  /// v0.52.19: SkillRegistry→ToolRegistry 增量同步（桥接可重入）。
  /// MCP 晚连/热注册的新工具在注册后调一次即可进子代理工具区。
  /// 幂等：已存在（含真实注册如 spawn_agent）不覆盖。
  void sync_skill_to_tool_registry();

  /// v0.52.4(L3): 会话项目测试命令收集（按构建系统探测，按类不按语言）。
  std::vector<std::string> collect_project_test_commands(
      const std::string& session_id) const;

  /// v0.52.4(L3): 合成前测试 gate——绿放行/红拒绝（report 带输出）。
  bool run_project_test_gate(const std::string& session_id,
                             nlohmann::json& report);

  /// v0.52.4: 分解任务波浪执行（decompose_and_run/resume 共用；
  /// completed_in=断点：已终结子任务入结果集不重跑）。
  nlohmann::json run_decompose_waves(const std::string& session_id,
                                     const nlohmann::json& tasks,
                                     const nlohmann::json& completed_in,
                                     int parent_goal_id,
                                     const std::map<std::string, int>& taskid_to_goal,
                                     int max_concurrent);

  nlohmann::json handle_agent_orchestration(
      const std::string& session_id, const std::string& type,
      const nlohmann::json& req,
      const std::function<nlohmann::json(nlohmann::json)>& finish);

  /// v0.50.5: 构造函数 467 行按功能段拆出的初始化域方法（同类内，纯可读性，
  /// 语义零变化——各段原本就是构造函数体内按注释块分界的顺序步骤）。
  void init_local_models();       ///< 本地 ModelPool 注册（Template/Qwen/Gemma + 异步预加载）
  void init_memory_rag();         ///< RAG 记忆（embedding 选择 + MemoryManager + summarizer）
  void init_subsystems();         ///< checkpoint/goal/correction/monitor/skill 维护/凭证池/webhook/缓存
  void init_cron();               ///< 巡检配置 + CronScheduler 启动（含 156 行 cron 回调）
  void init_tools_and_context();  ///< 角色系统/总线/黑板/看板 + Skill 注册 + 插件/MCP + ctx 配置

  /// 即时执行单个本地 action（经 ActionExecutor 白名单校验）。
  nlohmann::json handle_action(const std::string& action, const nlohmann::json& args) const;
  /// 提交异步任务到 TaskEngine（含幂等键与重试）。
  nlohmann::json handle_task_submit(const nlohmann::json& req);
  /// 取消非终态任务。
  nlohmann::json handle_task_cancel(const nlohmann::json& req);
  /// 以原任务参数重新提交，生成新 task_id。
  nlohmann::json handle_task_replay(const nlohmann::json& req);
  /// 查询任务状态迁移审计轨迹。
  nlohmann::json handle_task_audit(const nlohmann::json& req);

  /// 多轮槽位续接：从用户输入填槽；填满则递归 handle_chat 执行，否则返回澄清或 null。
  nlohmann::json try_fill_slot_state(const std::string& session_id, const std::string& text);

  /// HITL 审批处理：从 paused_approvals_ 中取出待审批的 AgentLoop，执行或取消。
  nlohmann::json handle_chat_approve(const std::string& session_id,
                                     const nlohmann::json& req);

  /// 轻量对话上下文：记录上一轮 intent/route/slots，支持「深圳的呢」类续问。
  struct DialogState {
    std::string last_intent;                 ///< 上一轮识别的意图
    std::string last_route;                  ///< 上一轮实际路由
    nlohmann::json last_slots = nlohmann::json::object();  ///< 上一轮槽位
    int ttl_turns{0};                        ///< 剩余有效轮数
    static constexpr int kMaxTurns = 4;      ///< 上下文最大存活轮数
  };

  /// 读取当前 session 的对话上下文快照（供意图打分与槽位续接）。
  DialogContext snapshot_dialog_context(const std::string& session_id);
  /// 每轮对话结束后递减 TTL，过期则清除上下文。
  void tick_dialog_context(const std::string& session_id);
  /// 记录本轮 intent/route/slots，重置 TTL。
  void note_dialog_state(const std::string& session_id,
                         const std::string& intent,
                         const std::string& route,
                         const nlohmann::json& slots);

  /// 从请求中提取或自动生成 cmd_id / trace_id。
  RequestMeta extract_meta(const nlohmann::json& req);
  /// 将 cmd_id / trace_id 写回响应 JSON。
  nlohmann::json with_meta(nlohmann::json payload, const RequestMeta& meta);

  /// 将会话文本追加写入长期记忆 jsonl 文件。
  void append_long_term_memory(const std::string& session_id, const std::string& text) const;

  /// 可选决策审计：THIN_AGENT_DECISION_AUDIT=1 时写入 jsonl（路径见 THIN_AGENT_DECISION_AUDIT_PATH）。
  void append_decision_audit(const std::string& session_id,
                             const std::string& text,
                             const nlohmann::json& result,
                             std::chrono::steady_clock::time_point started_at) const;

  /// 记录感知事件到近期事件环形窗口。
  void record_event(const std::string& event);

  /// v0.53.81: 收敛 FC run 终态——必须在【已持 mu_】时调用（锁内助手，
  /// 自身不加锁；与 record_event 同款"调用方持锁"规约）。
  /// 背景：审批型 run 经 Ws 续跑/拒绝/超时三条路径结束时无人 finish_run，
  /// fc_runs.status 永久停在 'running'；重启 mark_interrupted_on_boot 把它
  /// 一律改判 'interrupted'，load_resumable 便会把【已完成的 run】当断点
  /// 续跑（幽灵续跑）。三条出口统一收敛 + 清 fc_active_runs_。
  void finalize_fc_run_locked(const std::string& session_id, const char* status);

  DemoConfigCompat cfg_;                              ///< 运行配置（模式、模型、预算等）
  std::shared_ptr<ActionExecutor> executor_;          ///< 本地动作执行器
  std::shared_ptr<TaskEngine> task_engine_;           ///< 任务引擎（可为空）
  std::shared_ptr<agent::MemoryManager> memory_manager_;  ///< RAG 记忆管理器

public:
  /// v0.53.41: 会话记忆入库门面——worker 发送完 chat_result 后异步调用
  ///(此前仅 orchestrate 路径 ingest,普通 chat 对话从不进向量记忆,
  /// RAG 注入先天缺血;且同步 ingest 的云端 embedding 秒级延迟会挡回复)。
  void memory_ingest_conversation(const std::string& session_id,
                                  const std::string& user_text,
                                  const std::string& assistant_text) {
    if (memory_manager_ && !user_text.empty() && !assistant_text.empty()) {
      memory_manager_->ingest_conversation(session_id, user_text, assistant_text);
    }
  }
  std::unique_ptr<agent::GoalManager> goal_mgr_;             ///< 长期目标管理
  std::unique_ptr<agent::ErrorCorrectionStore> correction_store_;  ///< 跨会话纠错学习
  std::unique_ptr<agent::ConversationSummarizer> summarizer_;  ///< 对话摘要压缩器
  std::unique_ptr<agent::CheckpointManager> checkpoint_mgr_;    ///< 检查点回滚
  std::unique_ptr<agent::FilesystemCheckpoint> fs_checkpoint_mgr_;  ///< 文件系统级检查点
  std::unique_ptr<agent::SkillManager> skill_manager_;         ///< 技能复用
  std::vector<std::string> matched_skill_ids_;                ///< v0.38.1: 当前请求匹配到的技能 ID（成功后 increment_use）
  std::unique_ptr<agent::AgentRoleManager> role_mgr_;          ///< v0.11.0: Agent 角色管理
  std::unique_ptr<SubAgentBus> agent_bus_;                     ///< v0.11.4: 子Agent消息总线
  std::unique_ptr<Blackboard> blackboard_;                     ///< v0.11.5: 共享上下文黑板
  std::unique_ptr<KanbanBoard> kanban_;                        ///< v0.11.6: Kanban看板
  std::unique_ptr<CronScheduler> cron_scheduler_;              ///< 定时任务调度
  /// v0.43.0: 凭证池（多 key 轮转 + 失败自动降级，lazy 加载）
  std::unique_ptr<CredentialPool> credential_pool_;
  /// v0.44.0: Webhook 客户端（HMAC 签名投递）
  std::unique_ptr<WebhookClient> webhook_client_;
  /// v0.42.1: cron 会话上下文（供 attach_to_session 回复使用）
  std::unordered_map<int, nlohmann::json> cron_session_contexts_;
  std::unique_ptr<AgentOrchestrator> orchestrator_;            ///< 多 Agent 编排（惰性启动）
  std::unique_ptr<ProactiveMonitor> proactive_monitor_;        ///< 主动监控器
  std::unique_ptr<KbSearcher> kb_searcher_;                    ///< v0.26.0: 知识库 FTS5 搜索
  std::unique_ptr<FactStore> fact_store_;                      ///< v0.27.5: 持久记忆存储
  std::unique_ptr<SessionStore> session_store_;                ///< v0.27.6: 会话搜索存储
  PatrolConfig patrol_config_;                                 ///< v0.27.7: 巡检配置
  std::unique_ptr<CurlHttpClient> http_client_;                ///< HTTP 客户端（CloudEmbeddingProvider 等组件的生命周期宿主）
  SkillRegistry skill_registry_;                               ///< Skill/Action 注册表
  std::atomic<int> sub_agent_counter_{0};                      ///< 子 Agent 计数
  std::atomic<int> next_id_counter_{0};                        ///< v0.11.6: 通用ID计数

  std::unordered_map<std::string, std::vector<ChatMessage>> session_chat_memory_;  ///< 会话消息记忆（user/assistant 角色对），token 预算驱动，非固定条数
  /// ── 跨轮上下文配置（从 chat_policy.json context 段落读取）──
  struct ContextConfig {
    int history_budget_tokens = 12000;
    int history_budget_tokens_embedded = 4000;
    int full_context_turns = 2;
    int truncated_max_chars = 200;
    bool project_scan_enabled = true;
    int project_scan_max_depth = 3;
    std::vector<std::string> project_scan_ignore = {"node_modules", ".git", "build", "__pycache__", ".pytest_cache"};
    int tool_cache_size = 5;
  };
  ContextConfig ctx_cfg_;

  /// ── 工具结果缓存（最近 K 次调用，用于②工具结果上下文）──
  struct ToolResultEntry {
    std::string action;
    std::string params_summary;   // 参数简写
    std::string result_truncated; // 截断后的输出
    std::chrono::steady_clock::time_point ts;
  };
  std::deque<ToolResultEntry> tool_result_cache_;

  /// ── 项目结构摘要（③ repo map，dev 模式下扫描一次）──
  std::string project_structure_;

  std::unordered_map<std::string, SlotState> slot_states_;  ///< 按 session 的槽位澄清状态

  std::unordered_map<std::string, DialogState> dialog_states_;  ///< 按 session 的对话上下文

  /// HITL 暂停审批：session_id → 等待审批的 AgentLoop 实例
  std::unordered_map<std::string,
                     std::unique_ptr<agent::AgentLoop>> paused_approvals_;

  std::vector<std::string> recent_events_;  ///< 近期事件环形缓冲
  size_t event_window_{32};                 ///< 事件窗口上限
  std::chrono::steady_clock::time_point start_time_{std::chrono::steady_clock::now()};  ///< v0.50.8: /stats uptime 基准

  // v0.25.9: broadcast callback (set by ws_agent_main)
  std::function<void(const nlohmann::json&)> broadcast_cb_;

  uint64_t total_requests_{0};                          ///< 累计请求数
  std::unordered_map<std::string, uint64_t> type_counters_;  ///< 按请求类型计数
  double total_latency_ms_{0.0};                        ///< 累计延迟（毫秒）
  double last_latency_ms_{0.0};                         ///< 最近一次请求延迟

  /// v0.47.1: token 用量累加（原子，多 worker 线程安全）
  std::atomic<int64_t> total_prompt_tokens_{0};
  std::atomic<int64_t> total_completion_tokens_{0};
  std::atomic<int64_t> total_api_calls_{0};

  /// v0.47.3: 流式中断 — 用户可中途喊停 chat 请求
  /// chat_abort WS 消息将 session_id 加入此集合，FC 循环每轮检查并退出
  std::unordered_set<std::string> aborted_sessions_;
  std::function<nlohmann::json()> ws_stats_provider_;  // v0.53.25 宿主层注入

  /// v0.47.5: LLM 响应缓存（LRU + TTL），省 cost
  std::unique_ptr<ResponseCache> response_cache_;

  /// v0.48.0: 会话级项目上下文（运行时动态切换 project_root + 读写模式）
  /// 用户通过 set_project / set_project_mode WS 消息控制。
  /// FC 循环和 dispatch_cpp 前注入到 thread_local，供 handler 校验路径。
  std::unordered_map<std::string, PathValidator::ProjectContext> session_projects_;

  /// v0.49.0: HITL — FC 循环危险工具待审批状态
  struct PendingApproval {
    std::string tool_name;
    std::string tool_args;                    ///< JSON 字符串
    std::vector<ChatMessage> fc_messages;     ///< FC 循环暂停时的消息快照（v0.52.2 续跑用）
    int fc_iterations_used{0};                ///< 暂停时已消耗迭代数（续跑预算=上限-已用）
    std::chrono::steady_clock::time_point created_at;
    std::string user_text;                    ///< 触发审批的原始用户输入（重放用）
    static constexpr int kTimeoutSec = 300;   ///< 5 分钟超时
  };
  std::unordered_map<std::string, PendingApproval> fc_pending_approvals_;

  /// v0.52.26: FC 运行日志（断点续跑 #2）——工具边界增量落盘，
  /// 重启后 interrupted run 可 resume。与 agent_sessions.db 同目录。
  std::unique_ptr<class FcRunStore> fc_run_store_;
  std::atomic<bool> alive_{true};  // v0.53.4: 后台插件线程存活探测
  /// 当前会话→活跃 run_id（FC 开始时创建，终态时清理）。
  std::unordered_map<std::string, std::string> fc_active_runs_;

  /// v0.29.0: webhook 事件队列 — 非阻塞入队，下轮 handle_chat 自动消费
  struct WebhookEvent {
    std::string source;
    std::string event;
    std::string payload;
  };
  std::deque<WebhookEvent> pending_webhooks_;

  std::string memory_file_path_{default_memory_jsonl_path()};  ///< 长期记忆持久化路径

  uint64_t seq_{0};        ///< 请求序号，用于生成 trace/cmd
  mutable std::mutex mu_;  ///< 保护会话状态与指标
};

}  // namespace thin_agent
