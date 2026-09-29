#include "thin_agent/core/AgentService.h"
#include "thin_agent/llm/LlmCircuitBreaker.h"  // v0.53.25
#include "thin_agent/core/HookSystem.h"  // v0.52.29
#include "thin_agent/core/JsonCoerce.h"
#include "thin_agent/core/JsonExtract.h"

#include "thin_agent/Version.h"

#include <algorithm>
#include <cstring>
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


AgentService::~AgentService() {
  // v0.53.4: 双闸——①存活门闩（ticker 回调内自查）②注销 cron 回调
  //（锁内换 null，下轮 ticker 起彻底静默）。ASAN 实证无闸时 ticker
  // 在宿主成员析构期间回调 chat → FcRunStore::Impl use-after-free。
  alive_.store(false);
  // v0.53.73: monitor poll 线程先停——alert 回调捕获 this,不停=线程
  /// 踩过析构点(cron v0.53.4 修过同款,monitor 漏网;修一漏一第 5 例)
  if (proactive_monitor_) proactive_monitor_->stop();
  if (cron_scheduler_) cron_scheduler_->clear_callback();
  // v0.53.78: orchestrator 三段停机(worker 池 join+detached 在飞等待)
  /// ——此前 stop 不存在:execute_async per-request detach 捕 this,
  /// AgentService 析构后继续跑=UAF(第 7 例线程停机闸)
  if (orchestrator_) orchestrator_->stop();
}

AgentService::AgentService(DemoConfigCompat cfg,
                           std::shared_ptr<ActionExecutor> executor,
                           std::shared_ptr<TaskEngine> task_engine)
    : cfg_(std::move(cfg)), executor_(std::move(executor)), task_engine_(std::move(task_engine)) {
  std::filesystem::create_directories(std::filesystem::path(memory_file_path_).parent_path());

  // v0.50.5: 构造函数按功能段拆为 5 个初始化域方法（见 AgentService.h），
  // 顺序与原实现完全一致，语义零变化。
  init_local_models();
  init_memory_rag();
  init_subsystems();
  init_cron();
  init_tools_and_context();
}

void AgentService::init_local_models() {
  // 注册本地模型到 ModelPool（离线时 HybridRouter 级联使用）
  static bool models_registered = false;
  if (!models_registered) {
// Tier 0: TemplateModel — 关键词模板，0ms，零内存
auto tm = std::make_unique<thin_agent::local::TemplateModel>();
tm->load();
thin_agent::local::ModelPool::instance().add(std::move(tm));

// Tier 1: Qwen2.5-0.5B Q2_K (395MB) — 设备侧小模型
//  注册但不预加载；PC 端通过 ModelPool::load_all() 或手动 load() 激活
#ifdef THIN_AGENT_WITH_LLAMA_CPP
auto qwen = std::make_unique<thin_agent::local::GgufModel>(
    "models/gguf/qwen2.5-0.5b-instruct-q2_k.gguf",
    std::vector<thin_agent::local::ModelCapability>{
        {"chat", 10, 2048},
        {"translate", 10, 128}},
    2048, 4);
thin_agent::local::ModelPool::instance().add(std::move(qwen));

// Tier 2: Gemma-3-1B Q4_K_M (769MB) — PC 侧更强模型
//  注册但不预加载，PC 端使用时按需加载
auto gemma = std::make_unique<thin_agent::local::GgufModel>(
    "models/gguf/gemma-3-1b-it-q4_k_m.gguf",
    std::vector<thin_agent::local::ModelCapability>{
        {"chat", 20, 4096},
        {"translate", 20, 256}},
    4096, 4);
thin_agent::local::ModelPool::instance().add(std::move(gemma));
#endif

models_registered = true;

// 异步预加载 Qwen（不阻塞启动），用于后续输入翻译。
// 加载完成前收到的非 CN/EN 输入会使用 fast API 翻译，
// 加载完成后自动切换到本地翻译。
std::thread([]() {
  auto candidates = thin_agent::local::ModelPool::instance().match("translate");
  if (!candidates.empty()) {
    auto* m = candidates.front();
    if (!m->is_loaded()) m->load();
  }
}).detach();
  }
}

void AgentService::init_memory_rag() {
  // 初始化 RAG 记忆管理器：优先云端嵌入，不可用时回退本地哈希
  std::shared_ptr<agent::EmbeddingProvider> embedding;
  std::string api_key;
  api_key = resolve_api_key(credential_pool_.get(), cfg_.api_key_env, cfg_.provider);
  if (!api_key.empty() && !cfg_.api_base.empty()) {
http_client_ = std::make_unique<CurlHttpClient>(15000);
embedding = std::make_shared<agent::CloudEmbeddingProvider>(
    *http_client_, cfg_.api_base, api_key, "text-embedding-3-small", 15000);
  } else {
embedding = std::make_shared<agent::LocalHashEmbeddingProvider>(128);
  }
  // v0.53.53: 向量库按维度分文件——云(1536)/本地(128)换档后若共用
  /// 一个 db,旧 blob 与新维度错位(deserialize 截断/补零=相似度静默
  /// 劣化的垃圾结果);按维度命名天然隔离,换档各自积累互不污染
  const int emb_dim = embedding->dimension();
  const std::string dim_db = default_data_dir() + "/agent_memory_d" +
                             std::to_string(emb_dim) + ".db";
  {
    // 一次性迁移:旧统一库存在且新档库未建→rename 继承(当前档与
    /// 旧库同源时 blob 维度一致;换档场景旧库留给对应维度档用)
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string legacy_db = default_data_dir() + "/agent_memory.db";
    if (!fs::exists(dim_db, ec) && fs::exists(legacy_db, ec)) {
      fs::rename(legacy_db, dim_db, ec);
    }
  }
  memory_manager_ = std::make_shared<agent::MemoryManager>(
  embedding, dim_db, true);

  // ── 新组件初始化 ──────────────────────────────
  // v0.29.0: 注入 summarize 回调，对话超 10 轮时自动调用云端 LLM 压缩
  summarizer_ = std::make_unique<agent::ConversationSummarizer>(
  10, 4,
  [this](const std::string& prompt) -> std::string {
    if (cfg_.mode == "offline" || cfg_.provider.empty()) {
      log_event("summarizer", LogLevel::Info, "offline mode, skip cloud summarization");
      return "";
    }
    std::string key = resolve_api_key(credential_pool_.get(), cfg_.api_key_env, cfg_.provider);
    if (key.empty()) {
      log_event("summarizer", LogLevel::Warn, "no API key, skip summarization");
      return "";
    }
    try {
      std::vector<ChatMessage> msgs;
      msgs.push_back({"system", "You are a conversation summarizer. "
                       "Respond with ONLY the summary in 2-3 sentences. No preamble."});
      msgs.push_back({"user", prompt});
      CurlHttpClient http(10000);
      auto r = CloudLlmClient::chat_completion(http, cfg_, key, msgs);
      if (r.ok && !r.text.empty()) {
        try {
          auto j = nlohmann::json::parse(r.text);
          if (j.contains("choices") && j["choices"].is_array() &&
              !j["choices"].empty()) {
            auto& c = j["choices"][0]["message"];
            if (c.contains("content") && c["content"].is_string())
              return c["content"].get<std::string>();
          }
        } catch (...) {}
        return "";  // 解析失败，返回空（保留原消息）
      }
      log_event("summarizer", LogLevel::Error, "cloud call failed",
                {{"error", r.error}});
    } catch (const std::exception& e) {
      log_event("summarizer", LogLevel::Error, "exception",
                {{"what", std::string(e.what())}});
    }
    return "";
  });
  checkpoint_mgr_ = std::make_unique<agent::CheckpointManager>();
  fs_checkpoint_mgr_ = std::make_unique<agent::FilesystemCheckpoint>();
  goal_mgr_ = std::make_unique<agent::GoalManager>(default_data_dir() + "/goals.db");
  correction_store_ = std::make_unique<agent::ErrorCorrectionStore>(default_data_dir() + "/corrections.db");
  proactive_monitor_ = std::make_unique<ProactiveMonitor>();
  proactive_monitor_->on_alert([this](const nlohmann::json& alert) {
// v0.53.77: 裸调用定性——回调跑在 monitor poll 线程,加 mu_ 锁
  /// 与主线程构成互等面(lmstudio e2e 实测挂死,撤锁 3/3 PASS);
  /// 竞争窗口=recent_events_ 单次 push(vector 摊还 O(1),实践中
  /// 风险低于互等;后续若要根治须 mu_→shared_mutex 或独立事件锁
  record_event("monitor_alert: " + alert.dump());
  });
  auto skill_store = std::make_shared<agent::VectorStore>();
  skill_store->init(default_data_dir() + "/agent_skills.db", embedding->dimension());
  skill_manager_ = std::make_unique<agent::SkillManager>(embedding, skill_store);
  // v0.29.0: 启动时自动维护技能（stale 检测 → 归档 → 清理）
  {
auto result = skill_manager_->auto_maintain(30, 60);
int marked = result.value("marked_stale", 0);
int archived = result.value("archived", 0);
int cleaned = result.value("cleaned", 0);
if (marked > 0 || archived > 0 || cleaned > 0) {
  log_event("curator", LogLevel::Info, "auto-maintain",
            {{"marked_stale", marked}, {"archived", archived},
             {"cleaned", cleaned}});
}
  }
}

/// v0.53.0: PluginContext 实现——把核心服务面注入插件（窄接口，
/// 见 include/thin_agent/plugin/PluginContext.h）。
class AgentServicePluginContext : public PluginContext {
public:
  explicit AgentServicePluginContext(AgentService* svc) : svc_(svc) {}

  bool alive() const override {
    // v0.53.4: cron ticker 等插件后台线程回调前的存活门闩——
    // 宿主析构序置 false（use-after-free 生产实证：FcRunStore::Impl）
    return svc_ && svc_->alive_.load();
  }

  nlohmann::json chat(const std::string& session, const std::string& text,
                      const nlohmann::json& meta = {}) override {
    if (!svc_) return {{"type", "error"}, {"message", "no service"}};
    nlohmann::json req;
    req["type"] = "chat";
    req["text"] = text;
    if (!meta.empty()) {
      for (auto it = meta.begin(); it != meta.end(); ++it)
        req[it.key()] = it.value();
    }
    return svc_->handle_request(session, req);
  }

  void broadcast(const nlohmann::json& frame) override {
    if (svc_ && svc_->broadcast_cb_)
      svc_->broadcast_cb_(frame);
  }

  nlohmann::json session_snapshot(const std::string& session) override {
    if (!svc_) return nlohmann::json::array();
    std::lock_guard<std::mutex> lk(svc_->mu_);
    nlohmann::json arr = nlohmann::json::array();
    auto it = svc_->session_chat_memory_.find(session);
    if (it != svc_->session_chat_memory_.end()) {
      for (const auto& m : it->second)
        arr.push_back({{"role", m.role}, {"content", m.content}});
    }
    return arr;
  }

  std::string data_dir(const std::string& plugin_name) const override {
    std::string base = default_data_dir() + "/plugins";
    if (!plugin_name.empty()) base += "/" + plugin_name;
    // v0.53.1: 保证目录存在（cron.db 落库踩坑：目录缺失 sqlite open 失败
    // → db_=null → add 静默返回空 JSON）。std::filesystem 需 <filesystem>。
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
  }

  nlohmann::json config(const std::string& plugin_name) const override {
    if (!svc_) return nlohmann::json();
    // v2 服务地址注入位：核心把 subsystem 指针写给需要的插件
    // v0.53.6: code_exec 语言类白名单词表透传——chat_policy.json 的
    // interpreters 段原样递给插件（词表主权在配置文件，核心零解析）。
    if (plugin_name == "code_exec") {
      return chat_policy().value("interpreters", nlohmann::json::object());
    }
    if (plugin_name == "state") {
      nlohmann::json j;
      j["goal_manager"] = reinterpret_cast<int64_t>(svc_->goal_mgr_.get());
      j["checkpoint_manager"] =
          reinterpret_cast<int64_t>(svc_->checkpoint_mgr_.get());
      j["fs_checkpoint"] =
          reinterpret_cast<int64_t>(svc_->fs_checkpoint_mgr_.get());
      return j;
    }
    if (plugin_name == "kanban") {
      nlohmann::json j;
      j["board"] = reinterpret_cast<int64_t>(svc_->kanban_.get());
      return j;
    }
    if (plugin_name == "monitor") {
      nlohmann::json j;
      j["monitor"] = reinterpret_cast<int64_t>(svc_->proactive_monitor_.get());
      return j;
    }
    if (plugin_name == "cron") {
      nlohmann::json j;
      // cron 调度器实例随插件（回调经 ctx.chat）——核心不注入；
      // patrol 定时注册经 ctx.config("cron").patrol 传入
      j["patrol_enabled"] = svc_->patrol_config_.enabled;
      j["patrol_interval_min"] = svc_->patrol_config_.cron_interval_min;
      return j;
    }
    if (plugin_name == "meta") {
      nlohmann::json j;
      j["fact_store"] = reinterpret_cast<int64_t>(svc_->fact_store_.get());
      j["session_store"] =
          reinterpret_cast<int64_t>(svc_->session_store_.get());
      j["summarizer"] =
          reinterpret_cast<int64_t>(svc_->summarizer_.get());
      j["correction_store"] =
          reinterpret_cast<int64_t>(svc_->correction_store_.get());
      return j;
    }
    if (plugin_name == "patrol") {
      nlohmann::json j;
      const auto& p = svc_->patrol_config_;
      j["enabled"] = p.enabled;
      j["interval_min"] = p.cron_interval_min;
      j["quiet_mode"] = p.quiet_mode;
      j["disk"] = {{"enabled", p.disk.enabled}, {"path", p.disk.path},
                   {"warn_pct", p.disk.warn_pct}};
      j["memory"] = {{"enabled", p.memory.enabled},
                     {"warn_pct", p.memory.warn_pct}};
      j["process"] = {{"enabled", p.process.enabled},
                      {"names", p.process.names}};
      j["log_errors"] = {{"enabled", p.log_errors.enabled},
                         {"path", p.log_errors.log_path},
                         {"warn_count", p.log_errors.warn_count}};
      return j;
    }
    if (plugin_name == "collab") {
      nlohmann::json j;
      j["blackboard"] = reinterpret_cast<int64_t>(svc_->blackboard_.get());
      j["bus"] = reinterpret_cast<int64_t>(svc_->agent_bus_.get());
      return j;
    }
    if (plugin_name == "kb") {
      nlohmann::json j;
      j["searcher"] = reinterpret_cast<int64_t>(svc_->kb_searcher_.get());
      return j;
    }
    return nlohmann::json();
  }

  void log(const std::string& plugin_name, const std::string& level,
           const std::string& event, const nlohmann::json& fields) override {
    LogLevel lv = level == "error" ? LogLevel::Error
                  : level == "warn"  ? LogLevel::Warn
                                     : LogLevel::Info;
    log_event(plugin_name, lv, event, fields);
  }

private:
  AgentService* svc_;
};

void AgentService::init_subsystems() {
  cron_scheduler_ = std::make_unique<CronScheduler>();
  // v0.43.0: 凭证池（lazy — 文件不存在时静默跳过）
  credential_pool_ = std::make_unique<CredentialPool>();
  credential_pool_->load(default_data_dir() + "/credentials.json");
  // v0.44.0: Webhook 客户端（HMAC 签名投递）
  webhook_client_ = std::make_unique<WebhookClient>();
  response_cache_ = std::make_unique<ResponseCache>(100, 3600);  // v0.47.5: 100条/1小时TTL
  webhook_client_->load(default_config_dir() + "/webhooks.json");
  // v0.43.0: 加载持久化技能
  skill_manager_->load_from_file(default_data_dir() + "/skills.json");
  orchestrator_ = std::make_unique<AgentOrchestrator>();  // 惰性，首次 execute() 才启动线程
  kb_searcher_ = std::make_unique<KbSearcher>(default_kb_db_path());

  // v0.27.5: 持久记忆存储（热/冷两段式）
  fact_store_ = std::make_unique<FactStore>();
  fact_store_->open(default_data_dir() + "/agent_facts.db");

  // v0.27.6: 会话搜索存储
  session_store_ = std::make_unique<SessionStore>();
  session_store_->open(default_data_dir() + "/agent_sessions.db");
  // v0.52.26: FC 运行日志（断点续跑）——同库新表；boot 清扫把上次
  // 未完成的 running 标记 interrupted（可 resume），孤儿审批落盘的
  // TTL 由 chat 入口惰性清理（与内存版同策略）。
  {
    auto store = std::make_unique<FcRunStore>();
    if (store->open(default_data_dir() + "/agent_sessions.db")) {
      int swept = store->mark_interrupted_on_boot();
      if (swept > 0)
        log_event("fc-journal", LogLevel::Info, "boot sweep",
                  {{"interrupted_runs", swept}});
      fc_run_store_ = std::move(store);
    }
  }
}

void AgentService::init_cron() {
  // v0.27.7: 加载巡检配置 + 注册 cron_patrol 定时任务
  patrol_config_ = PatrolConfig::from_policy();
  if (patrol_config_.enabled && cron_scheduler_) {
std::string schedule = "every " + std::to_string(patrol_config_.cron_interval_min) + "m";
cron_scheduler_->add_task("patrol_probe", schedule,
  "RUN_PATROL_PROBE",  // 占位符，实际 prompt 由 PatrolProbe::build_prompt 生成
  true);
  }

  // v0.11.0: 多 Agent 角色系统
  role_mgr_ = std::make_unique<agent::AgentRoleManager>();
  role_mgr_->register_builtins();

  // v0.11.4: 子 Agent 消息总线
  agent_bus_ = std::make_unique<SubAgentBus>();

  // v0.11.5: 共享上下文黑板
  blackboard_ = std::make_unique<Blackboard>();

  // v0.11.6: Kanban 看板
  kanban_ = std::make_unique<KanbanBoard>();

  // v0.25.9: 启动 CronScheduler（定时任务回调 → 注入 chat 管线 → LLM 自主响应）
  cron_scheduler_->start(default_data_dir() + "/cron.db",
[this](const nlohmann::json& task) {
  std::string name = task.value("name", "unnamed");
  std::string prompt = task.value("prompt", "");
  std::string workdir = task.value("workdir", "");
  bool no_agent = task.value("no_agent", false);
  {
    std::lock_guard<std::mutex> lk(mu_);
    record_event("cron_fired: " + name + " | prompt=" + prompt.substr(0, 200));
  }

  // workdir: 执行前切换目录
  std::string prev_cwd;
  if (!workdir.empty()) {
    char cwdbuf[4096];
    if (getcwd(cwdbuf, sizeof(cwdbuf))) prev_cwd = cwdbuf;
    if (::chdir(workdir.c_str()) != 0) {}  // v0.53.16: 显式消费返回值（-Wunused-result）
  }

  // v0.42.0: no_agent 模式 — 直出脚本不跑 LLM
  if (no_agent) {
    std::string script = task.value("no_agent_script", "");
    if (!script.empty()) {
      std::thread([this, name, script, workdir]() {
        std::string cmd = "timeout 30 " + script + " 2>&1";
        std::array<char, 65536> buf;
        std::string out;
        auto p = ::popen(cmd.c_str(), "r");
        if (p) {
          while (fgets(buf.data(), static_cast<int>(buf.size()), p))
            out += buf.data();
          ::pclose(p);
        }
        auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
        // 保存输出到 context_from 存储
        cron_scheduler_->save_output(name, now_ts, out);
        std::lock_guard<std::mutex> lk(mu_);
        record_event((std::string)"cron_noagent: " + name + " | output=" + out.substr(0, 2000));
      }).detach();
    }
    if (!workdir.empty() && !prev_cwd.empty()) if (::chdir(prev_cwd.c_str()) != 0) {}  // v0.53.16: 同上
    return;
  }

  // 将 cron prompt 作为 chat 消息注入 LLM 管线
  if (!prompt.empty()) {
    // v0.27.7: patrol_probe 使用真实探针数据，不走 placeholder prompt
    std::string actual_prompt = prompt;
    if (prompt == "RUN_PATROL_PROBE") {
      auto results = PatrolProbe::run_all(patrol_config_);
      actual_prompt = PatrolProbe::build_prompt(results, patrol_config_);
      if (actual_prompt.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        record_event("cron_patrol: all OK (quiet mode)");
        if (!workdir.empty() && !prev_cwd.empty()) if (::chdir(prev_cwd.c_str()) != 0) {}  // v0.53.16: 同上
        return;
      }
    }

    // v0.42.1: context_from — 将上游任务输出注入 prompt
    int context_from_id = task.value("context_from", 0);
    if (context_from_id > 0) {
      auto prev_output = cron_scheduler_->load_output(context_from_id);
      if (!prev_output.empty()) {
        actual_prompt = "【上游任务输出】\n" + prev_output + "\n\n" + actual_prompt;
      }
    }

    std::thread([this, name, actual_prompt, workdir, prev_cwd, task]() {
      try {
        auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
        std::string model_override = task.value("model", "");
        std::string provider_override = task.value("provider", "");
        // v0.42.1: skills/toolsets 过滤 + model/provider — 使用 thread_local 避免竞争
        // 构造过滤上下文
        nlohmann::json filter = nlohmann::json::object();
        std::string skills_str = task.value("skills", "");
        std::string toolsets_str = task.value("toolsets", "");
        if (!skills_str.empty())
          filter["skills"] = nlohmann::json::parse(skills_str, nullptr, false);
        if (!toolsets_str.empty())
          filter["toolsets"] = nlohmann::json::parse(toolsets_str, nullptr, false);
        if (!model_override.empty()) filter["model"] = model_override;
        if (!provider_override.empty()) filter["provider"] = provider_override;
        g_cron_filter = filter;
        // v0.42.3b: 临时覆盖 model/provider（cfg_ 是成员变量，单线程安全）
        std::string saved_model = cfg_.model_name;
        std::string saved_provider = cfg_.provider;
        if (!model_override.empty()) cfg_.model_name = model_override;
        if (!provider_override.empty()) cfg_.provider = provider_override;
        auto result = handle_chat("cron", actual_prompt);
        cfg_.model_name = saved_model;
        cfg_.provider = saved_provider;
        g_cron_filter = nullptr;

        // v0.42.1: attach_to_session — 保存会话上下文供后续回复
        int task_id = task.value("id", 0);
        nlohmann::json ctx;
        ctx["task_name"] = name;
        ctx["task_id"] = task_id;
        ctx["prompt"] = actual_prompt;
        ctx["result"] = result.value("text", "");
        ctx["timestamp"] = now_ts;
        cron_session_contexts_[task_id] = ctx;
        // 保存输出供 context_from 引用
        cron_scheduler_->save_output(std::string("task_") + std::to_string(task_id),
                                     now_ts, result.value("text", ""));
        std::string response = result.value("text", "");
        if (!response.empty()) {
          std::lock_guard<std::mutex> lk(mu_);
          record_event("cron_result: " + name + " => " + response.substr(0, 2000));
        }
        // v0.44.0: deliver_to 支持 webhook 投递（HMAC 签名）
        std::string deliver_to = task.value("deliver_to", "chat");
        if (!response.empty() && webhook_client_ && deliver_to.rfind("webhook:", 0) == 0) {
          std::string wh_target = deliver_to.substr(8);  // "webhook:" 之后的部分
          if (wh_target.find("://") != std::string::npos) {
            // 直接 URL 格式: webhook:https://hooks.example.com?secret=xxx
            std::string url = wh_target;
            std::string secret;
            auto sp = url.find("?secret=");
            if (sp != std::string::npos) {
              secret = url.substr(sp + 8);
              url = url.substr(0, sp);
            }
            webhook_client_->send_direct(url, secret, response);
          } else if (!wh_target.empty()) {
            // 配置中的 webhook 名: webhook:slack
            webhook_client_->send(wh_target, response);
          }
        }
        if (broadcast_cb_ && !response.empty()) {
          std::string clean = response;
          auto pos = clean.find("\\n\\n");
          if (pos != std::string::npos && pos < 200) {
            clean = clean.substr(pos + 2);
          }
          // v0.54.6: 改名 notify_ts——此前与外层 `now_ts` 同名（-Wshadow=local 判官命中）
          auto notify_ts = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
          broadcast_cb_({ {"type", "cron_notify"},
                          {"name", name},
                          {"result", clean},
                          {"deliver_to", task.value("deliver_to", "chat")},
                          {"task_id", task.value("id", 0)},
                          {"timestamp", notify_ts} });
        }
      } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(mu_);
        record_event((std::string)"cron_error: " + name + " | " + e.what());
      }
      if (!workdir.empty() && !prev_cwd.empty()) if (::chdir(prev_cwd.c_str()) != 0) {}  // v0.53.16: 同上
    }).detach();
  } else {
    if (!workdir.empty() && !prev_cwd.empty()) if (::chdir(prev_cwd.c_str()) != 0) {}  // v0.53.16: 同上
  }
});
}

void AgentService::init_tools_and_context() {
  // ── v0.27.7: PatrolProbe 巡检工具 ──

  // v0.12: Skill 注册表
  const auto& policy = chat_policy();
  if (policy.contains("skills") && policy.contains("whitelist")) {
skill_registry_.load_from_policy(policy["skills"], policy["whitelist"]);
  }
  register_cpp_handlers();

  // v0.29.0: 自动启动主动监控器（默认 5s 扫描间隔）
  if (proactive_monitor_) {
proactive_monitor_->start(5000);
log_event("monitor", LogLevel::Info, "proactive monitor auto-started",
          {{"interval_ms", 5000}});
  }

  // ── 插件加载 ──
  // 从 chat_policy.json plugins 段读取模式→目录映射，加载对应插件
  if (policy.contains("plugins") && policy["plugins"].is_object()) {
const auto& plug = policy["plugins"];
std::vector<std::string> dirs;

// always 目录（始终加载）
if (plug.contains("always") && plug["always"].is_array()) {
  for (const auto& d : plug["always"])
    if (d.is_string()) dirs.push_back(d.get<std::string>());
}

// 开发模式：加载 dev 目录
const char* dev_env = std::getenv("THIN_AGENT_DEV_MODE");
bool is_dev = (dev_env && std::string(dev_env) == "1");
if (is_dev && plug.contains("modes") && plug["modes"].is_object()) {
  const auto& modes = plug["modes"];
  if (modes.contains("dev") && modes["dev"].is_array()) {
    for (const auto& d : modes["dev"])
      if (d.is_string()) dirs.push_back(d.get<std::string>());
  }
}

if (!dirs.empty()) {
  // v0.53.0: context 注入（v2 插件获得窄服务面；旧插件不受影响）
  plugin_ctx_ = std::make_unique<AgentServicePluginContext>(this);
  plugin::load_plugins(skill_registry_, "~/.thin_agent/plugins", dirs,
                       plugin_ctx_.get());
}
  }

  // ── v0.29.0: MCP Server 集成 ──────────────────────────────────
  // 从 chat_policy.json 读取 mcp_servers 配置，连接外部 MCP servers，
  // 将其工具注册到 SkillRegistry。
  if (policy.contains("mcp_servers") && policy["mcp_servers"].is_array()) {
for (const auto& srv : policy["mcp_servers"]) {
  std::string name = srv.value("name", "");
  std::string transport_type = srv.value("transport", "stdio");
  bool enabled = srv.value("enabled", true);
  if (!enabled || name.empty()) continue;

  std::unique_ptr<agent::ITransport> transport;
  std::string endpoint;

  if (transport_type == "stdio") {
    endpoint = srv.value("command", "");
    auto* st = new agent::StdioTransport();
    // v0.53.7: 市面 mcpServers 标准格式——args[] 显式参数列表
    // （command+args 分离，无 shell 引号歧义）与 env{} 环境变量注入
    // （如 GITHUB_TOKEN）。两者皆可选；不配置时行为与旧版完全一致。
    if (srv.contains("args") && srv["args"].is_array()) {
      std::vector<std::string> argv_extra;
      for (const auto& a : srv["args"])
        if (a.is_string()) argv_extra.push_back(a.get<std::string>());
      st->set_args(argv_extra);
    }
    if (srv.contains("env") && srv["env"].is_object()) {
      std::map<std::string, std::string> env_extra;
      for (auto eit = srv["env"].begin(); eit != srv["env"].end(); ++eit)
        if (eit.value().is_string()) {
          std::string v = eit.value().get<std::string>();
          // v0.53.8: $VAR 展开（市面配置惯例，如 "PATH": "$PATH:/x"）——
          // 从父进程环境取值替换；未定义变量展开为空串
          std::string outv;
          outv.reserve(v.size());
          for (size_t i = 0; i < v.size();) {
            if (v[i] == '$' && i + 1 < v.size() &&
                (std::isalnum((unsigned char)v[i + 1]) || v[i + 1] == '_')) {
              size_t j = i + 1;
              while (j < v.size() &&
                     (std::isalnum((unsigned char)v[j]) || v[j] == '_')) ++j;
              const char* got = ::getenv(v.substr(i + 1, j - i - 1).c_str());
              if (got) outv += got;
              i = j;
            } else {
              outv += v[i++];
            }
          }
          env_extra[eit.key()] = std::move(outv);
        }
      st->set_env(env_extra);
    }
    transport.reset(st);
  } else if (transport_type == "http") {
    endpoint = srv.value("url", "");
    transport = std::make_unique<agent::HttpTransport>();
  }

  if (!transport || endpoint.empty()) {
    log_event("mcp", LogLevel::Warn, "skip server: no endpoint",
              {{"server", name}});
    continue;
  }

    // v0.53.7 修存量缺陷×2：
  // ①原代码先 transport->connect 再 client->connect（内部再连一次）——
  //   StdioTransport 二次 connect 必失败 → handshake failed → 全部 server
  //   被跳过。这就是 mcp_servers 配置从未生效的根因（配上即炸，空配置
  //   掩盖至今）。修：去掉预连，client->connect 一站式。
  // ②原代码 lambda 捕获 make_shared<McpClient>(std::move(*client))——
  //   move 后 transport_ 引用仍绑定循环局部的 unique_ptr 对象（循环尾
  //   析构 → use-after-free，真 e2e 实测 fs_list_directory 调用即崩）；
  //   且第二次 move 的是空壳 client（11 个工具里 10 个持空壳）。
  //   修：transport+client 同生命周期——shared_ptr<ITransport> 由
  //   shared_ptr<McpClient> 的 lambda 共同持有，注册进 handler 即长活。
  std::string client_name = srv.value("client_name", "thin_agent");
  // client 需要非 const 引用构造——用 unique_ptr 持有后共享所有权
  auto transport_sp = std::shared_ptr<agent::ITransport>(std::move(transport));
  auto client = std::make_shared<agent::McpClient>(*transport_sp, endpoint);
  if (!client->connect(client_name, std::string(kThinAgentVersion))) {
    log_event("mcp", LogLevel::Warn, "MCP handshake failed",
              {{"server", name}});
    continue;
  }

  // 注册 MCP 工具到 SkillRegistry
  auto mcp_tools = client->list_tools();
  if (mcp_tools.empty()) {  // v0.53.68: 空列表可观测(服务器真空/解析失败)
    log_event("mcp", LogLevel::Warn, "MCP server has no tools",
              {{"server", name}});
  }
  std::string prefix = srv.value("tool_prefix", "mcp_");
  for (const auto& mt : mcp_tools) {
    std::string full_name = prefix + mt.name;
    std::string full_desc = "[MCP:" + name + "] " + mt.description;

    // 注册为 C++ handler（通过 McpClient 调用）
    // v0.53.7: transport_sp 与 client 同捕获——生命周期随 handler 存续
    skill_registry_.register_cpp_handler(full_name,
      [transport_sp, client, mt_name = mt.name](const nlohmann::json& p) {
        return client->call_tool(mt_name, p);
      });

    log_event("mcp", LogLevel::Info, "registered tool",
              {{"tool", full_name}, {"server", name}});
  }

  log_event("mcp", LogLevel::Info, "connected",
            {{"server", name}, {"tools", mcp_tools.size()}});

  // v0.52.19: MCP 工具晚连/热注册后增量同步进 ToolRegistry——
  // 构造期一次性桥接对运行中连接的 MCP server 无效（子代理看不到
  // 新工具）。
  sync_skill_to_tool_registry();
}
  }

  // ── v0.52.14: SkillRegistry → ToolRegistry 桥接 ────────────────
  // 背景：AgentLoop（spawn 子代理）只从 ToolRegistry 取工具，而插件/
  // MCP/内置 action 全部注册在 SkillRegistry——子代理的工具声明区块
  // 里只有 spawn_agent（真 e2e 实测：子代理自述"规则文本提到 code_*
  // 但未实际定义"，波次零交付的根本原因）。
  // 修法：注册源收敛后一次性同步——SkillRegistry 的每个 action 在
  // ToolRegistry 里缺失时注册桥接 handler（dispatch_cpp 转发），
  // 已存在（如 spawn_agent）不覆盖。schema 从 build_tools_schema
  // 的 OpenAI function 声明重建（参数 required 集来自声明）。
  { sync_skill_to_tool_registry(); }

  // ── 跨轮上下文配置（从 chat_policy.json context 段落读取）──
  if (policy.contains("context") && policy["context"].is_object()) {
const auto& ctx = policy["context"];
ctx_cfg_.history_budget_tokens = ctx.value("history_budget_tokens", 12000);
ctx_cfg_.history_budget_tokens_embedded = ctx.value("history_budget_tokens_embedded", 4000);
ctx_cfg_.full_context_turns = ctx.value("full_context_turns", 2);
ctx_cfg_.truncated_max_chars = ctx.value("truncated_max_chars", 200);
ctx_cfg_.tool_cache_size = ctx.value("tool_cache_size", 5);
ctx_cfg_.project_scan_enabled = ctx.value("project_scan_enabled", true);
ctx_cfg_.project_scan_max_depth = ctx.value("project_scan_max_depth", 3);

// 是否嵌入式模式（决定用收缩预算）
const char* emb_env = std::getenv("THIN_AGENT_EMBEDDED_MODE");
bool is_embedded = (emb_env && std::string(emb_env) == "1");
if (is_embedded) {
  ctx_cfg_.history_budget_tokens = ctx_cfg_.history_budget_tokens_embedded;
}

// ③ 项目结构扫描（dev 模式 + 启用扫描）
{
  const char* dev_env2 = std::getenv("THIN_AGENT_DEV_MODE");
  bool dev_mode = (dev_env2 && std::string(dev_env2) == "1");
  if (dev_mode && ctx_cfg_.project_scan_enabled) {
    project_structure_ = scan_project_structure(
        std::filesystem::current_path().string(),
        ctx_cfg_.project_scan_max_depth);
  }
}
  }
}

void AgentService::record_event(const std::string& event) {
  // v0.53.77: 写侧规约=调用方持 mu_ 锁(与读侧 sense_payload 对齐;
  /// 本体加锁会与既有持锁调用点死锁——mu_ 非递归);裸调用两处已补
  recent_events_.push_back(event);
  if (recent_events_.size() > event_window_) {
    recent_events_.erase(recent_events_.begin(),
                         recent_events_.begin() + static_cast<long>(recent_events_.size() - event_window_));
  }
}

void AgentService::finalize_fc_run_locked(const std::string& session_id,
                                          const char* status) {
  // v0.53.81: 锁内助手（调用方持 mu_，自身不加锁）——收敛 FC run 终态。
  // 审批型 run 此前在三条出口（Ws 续跑完成/拒绝/孤儿超时）无人 finish_run，
  // fc_runs.status 永久 'running'；重启 mark_interrupted_on_boot 一律改判
  // 'interrupted'，load_resumable 于是把【已完成的 run】当断点续跑
  // （幽灵续跑：重放 journal 工具调用）。此处统一收敛 + 清活跃表。
  auto rit = fc_active_runs_.find(session_id);
  if (rit == fc_active_runs_.end()) return;
  if (fc_run_store_) fc_run_store_->finish_run(rit->second, status);
  fc_active_runs_.erase(rit);
}

void AgentService::append_long_term_memory(const std::string& session_id, const std::string& text) const {
  nlohmann::json row = {
      {"session_id", session_id},
      {"text", text},
  };
  std::ofstream out(memory_file_path_, std::ios::app);
  if (!out.is_open()) return;
  out << row.dump() << "\n";
}

AgentService::RequestMeta AgentService::extract_meta(const nlohmann::json& req) {
  std::lock_guard<std::mutex> lk(mu_);
  ++seq_;
  RequestMeta m;
  if (req.contains("cmd_id") && req["cmd_id"].is_string()) {
    m.cmd_id = req["cmd_id"].get<std::string>();
  } else {
    m.cmd_id = next_auto_cmd(seq_);
  }
  if (req.contains("trace_id") && req["trace_id"].is_string()) {
    m.trace_id = req["trace_id"].get<std::string>();
  } else {
    m.trace_id = next_auto_trace(seq_);
  }
  return m;
}

nlohmann::json AgentService::with_meta(nlohmann::json payload, const RequestMeta& meta) {
  payload["cmd_id"] = meta.cmd_id;
  payload["trace_id"] = meta.trace_id;
  return payload;
}

// v0.53.28: 会话记忆持久化——路径（sid 清洗防路径穿越；哈希保唯一）
std::string AgentService::session_file_path(const std::string& session_id) const {
  std::string clean;
  for (char c : session_id) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
      clean += c;
  }
  if (clean.empty()) clean = "anon";
  std::hash<std::string> h;
  const std::string base_dir = default_config_dir() + "/sessions";
  (void)system(("mkdir -p '" + base_dir + "'").c_str());
  return base_dir + "/" + clean + "-" + std::to_string(h(session_id) & 0xffffffff) + ".jsonl";
}

void AgentService::persist_session_msg(const std::string& session_id,
                                       const ChatMessage& msg) {
  try {
    const std::string path = session_file_path(session_id);
    std::ofstream out(path, std::ios::app);
    if (!out) return;
    out << nlohmann::json{{"role", msg.role}, {"content", msg.content},
                          {"ts", std::time(nullptr)}}
               .dump()
        << "\n";
  } catch (...) { /* 静默失败不影响主流程 */ }
}

void AgentService::restore_session_msgs(const std::string& session_id) {
  const std::string path = session_file_path(session_id);
  std::ifstream in(path);
  if (!in) return;
  std::vector<ChatMessage> loaded;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    try {
      auto row = nlohmann::json::parse(line);
      loaded.push_back(ChatMessage{row.value("role", "user"),
                                   row.value("content", "")});
    } catch (...) { /* 跳过坏行 */ }
  }
  if (loaded.empty()) return;
  if (loaded.size() > 200)  // cap：与客户端展示层一致
    loaded.erase(loaded.begin(), loaded.end() - 200);
  std::lock_guard<std::mutex> lk(mu_);
  auto& mem = session_chat_memory_[session_id];
  if (mem.empty()) {  // 只在内存空时回灌（避免重复）
    mem = std::move(loaded);
    record_event("session_restored:" + session_id + " n=" +
                 std::to_string(mem.size()));
  }
}

void AgentService::drop_session_file(const std::string& session_id) {
  (void)std::remove(session_file_path(session_id).c_str());
}

void AgentService::on_session_open(const std::string& session_id) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    (void)session_chat_memory_.try_emplace(session_id, std::vector<ChatMessage>{});
    record_event("session_open:" + session_id);
  }
  restore_session_msgs(session_id);  // v0.53.28: 重启回灌
  // v0.52.29: session_start 钩子（锁外）
  HookSystem::instance().dispatch(HookEvent::SessionStart,
                                  {{"session", session_id}});
}

void AgentService::on_session_close(const std::string& session_id) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    session_chat_memory_.erase(session_id);
    slot_states_.erase(session_id);  // 清理多轮槽位状态
    dialog_states_.erase(session_id);
    session_projects_.erase(session_id);      // v0.48.0: 项目上下文
    fc_pending_approvals_.erase(session_id);  // v0.49.0: HITL 待审批
    record_event("session_close:" + session_id);
  }
  // v0.52.29: session_end 钩子（锁外）
  HookSystem::instance().dispatch(HookEvent::SessionEnd,
                                  {{"session", session_id}});
}

DialogContext AgentService::snapshot_dialog_context(const std::string& session_id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = dialog_states_.find(session_id);
  if (it == dialog_states_.end()) return {};
  return DialogContext{
      it->second.last_intent,
      it->second.last_route,
      it->second.last_slots,
      it->second.ttl_turns,
  };
}

void AgentService::tick_dialog_context(const std::string& session_id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = dialog_states_.find(session_id);
  if (it == dialog_states_.end()) return;
  it->second.ttl_turns--;
  if (it->second.ttl_turns <= 0) dialog_states_.erase(it);
}

void AgentService::note_dialog_state(const std::string& session_id,
                                     const std::string& intent,
                                     const std::string& route,
                                     const nlohmann::json& slots) {
  if (intent.empty()) return;
  std::lock_guard<std::mutex> lk(mu_);
  dialog_states_[session_id] = DialogState{
      intent,
      route,
      slots.is_object() ? sanitize_json_slots(slots) : nlohmann::json::object(),
      DialogState::kMaxTurns,
  };
}

nlohmann::json AgentService::sense_payload() const {
  std::vector<std::string> ev;
  {
    std::lock_guard<std::mutex> lk(mu_);
    ev = recent_events_;
  }

  nlohmann::json recent = nlohmann::json::array();
  for (const auto& e : ev) recent.push_back(e);

  return {
      {"recent_events", recent},
      {"memory_file", memory_file_path_},
      {"memory_file_exists", std::filesystem::exists(memory_file_path_)},
  };
}

nlohmann::json AgentService::status_payload() const {
  // v0.53.25: 补运行态（与 /stats 对齐——WS 通道也能看到真实状态）
  int active_sessions = 0;
  bool circuit_open = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    active_sessions = static_cast<int>(session_chat_memory_.size());
  }
  for (const auto& [ep, st] : LlmCircuitBreaker::instance().snapshot())
    if (st.value("open", false)) circuit_open = true;
  return {
      {"mode", cfg_.mode},
      {"provider", normalize_provider(cfg_.provider)},
      {"model", cfg_.model_name},
      {"fallback", cfg_.fallback},
      {"uptime_seconds", std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - start_time_)
                             .count()},
      {"active_sessions", active_sessions},
      {"circuit_open", circuit_open},
      {"allow_actions", {"health_report", "collect_logs", "switch_mode", "capture_photo", "start_recording", "stop_recording", "fetch_capture_results"}},
      {"sense", sense_payload()},
  };
}

nlohmann::json AgentService::memory_recent_payload(const std::string& session_id) const {
  std::vector<ChatMessage> mem;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = session_chat_memory_.find(session_id);
    if (it != session_chat_memory_.end()) mem = it->second;
  }
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& x : mem) arr.push_back(x.content);
  return {
      {"type", "memory_recent_result"},
      {"session_id", session_id},
      {"memory", arr},
      {"memory_size", arr.size()},
  };
}

nlohmann::json AgentService::memory_history_payload(int limit) const {
  if (limit <= 0) limit = 20;
  if (limit > 200) limit = 200;

  std::deque<nlohmann::json> buf;
  std::ifstream in(memory_file_path_);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    try {
      auto row = nlohmann::json::parse(line);
      buf.push_back(row);
      if (static_cast<int>(buf.size()) > limit) buf.pop_front();
    } catch (...) {
      // skip malformed line
    }
  }

  nlohmann::json arr = nlohmann::json::array();
  for (const auto& it : buf) arr.push_back(it);

  return {
      {"type", "memory_history_result"},
      {"memory_file", memory_file_path_},
      {"history", arr},
      {"history_size", arr.size()},
  };
}

nlohmann::json AgentService::memory_search_payload(const std::string& query, int limit) const {
  if (limit <= 0) limit = 20;
  if (limit > 200) limit = 200;

  // v0.53.38: 向量语义检索优先——此前显式 memory_search 走 jsonl 线性扫,
  // 而向量库(agent_memory.db)只在 FC 自动注入(build_context)用到。
  // 空 query 不走向量(全量语义无意义),保留旧行为。
  if (memory_manager_ && !query.empty()) {
    nlohmann::json j;
    j["results"] = nlohmann::json::array();
    bool pushed_any = false;
    for (const auto& r : memory_manager_->recall(query, limit)) {
      nlohmann::json row;
      row["text"] = r.entry.content;
      row["source"] = r.entry.source.empty() ? "memory_vector" : r.entry.source;
      row["similarity"] = r.similarity;
      if (!r.entry.session_id.empty()) row["session"] = r.entry.session_id;
      row["created_at"] = r.entry.created_at;
      j["results"].push_back(std::move(row));
      pushed_any = true;
    }
    if (pushed_any) {
      j["retriever"] = "vector";  // 告知客户端走了语义检索
      return j;
    }
    // 向量零命中→落到下方 jsonl 子串扫(宽召回兜底)
  }

  const std::string q = to_lower_copy(query);
  std::deque<nlohmann::json> buf;
  auto push_hit = [&](nlohmann::json row) {
    if (!row.contains("source")) row["source"] = "memory_jsonl";
    buf.push_back(std::move(row));
    if (static_cast<int>(buf.size()) > limit) buf.pop_front();
  };

  std::ifstream in(memory_file_path_);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    try {
      auto row = nlohmann::json::parse(line);
      const std::string text = row.value("text", "");
      const std::string text_lower = to_lower_copy(text);
      if (q.empty() || text_lower.find(q) != std::string::npos) {
        push_hit(std::move(row));
      }
    } catch (...) {
      // skip malformed line
    }
  }

  if (task_engine_) {
    int list_limit_applied = 0;
    auto tasks = task_engine_->list_tasks(200, "", "", &list_limit_applied);
    if (tasks.is_array()) {
      for (const auto& t : tasks) {
        const std::string task_id = t.value("task_id", "");
        const std::string action = t.value("action", "");
        const std::string state = t.value("state", "");
        const std::string message = t.value("message", "");
        const std::string task_text =
            "task " + task_id + " action=" + action + " state=" + state + " message=" + message;
        const std::string task_text_lower = to_lower_copy(task_text);
        if (q.empty() || task_text_lower.find(q) != std::string::npos) {
          push_hit({
              {"source", "task_sqlite"},
              {"kind", "task"},
              {"task_id", task_id},
              {"action", action},
              {"state", state},
              {"message", message},
              {"created_at", t.value("created_at", "")},
              {"updated_at", t.value("updated_at", "")},
              {"row_ref", "task:" + task_id + "#" + t.value("updated_at", t.value("created_at", std::string("")) )},
              {"text", task_text},
          });
        }

        if (task_id.empty()) continue;
        auto audits = task_engine_->list_task_audits(task_id, 10);
        const auto& audit_arr = audits["audits"];
        if (!audit_arr.is_array()) continue;
        for (const auto& a : audit_arr) {
          const std::string from_state = a.value("from_state", "");
          const std::string to_state = a.value("to_state", "");
          const int code = a.value("code", -1);
          const std::string message_audit = a.value("message", "");
          const std::string audit_text =
              "audit " + task_id + " " + from_state + "->" + to_state +
              " code=" + std::to_string(code) + " message=" + message_audit;
          const std::string audit_text_lower = to_lower_copy(audit_text);
          if (!q.empty() && audit_text_lower.find(q) == std::string::npos) continue;
          push_hit({
              {"source", "task_sqlite"},
              {"kind", "task_audit"},
              {"task_id", task_id},
              {"audit_id", a.value("audit_id", -1)},
              {"from_state", from_state},
              {"to_state", to_state},
              {"code", code},
              {"message", message_audit},
              {"created_at", a.value("created_at", "")},
              {"row_ref", "audit:" + task_id + "#" + std::to_string(a.value("audit_id", -1))},
              {"text", audit_text},
          });
        }
      }
    }
  }

  nlohmann::json arr = nlohmann::json::array();
  for (const auto& it : buf) arr.push_back(it);

  return {
      {"type", "memory_search_result"},
      {"memory_file", memory_file_path_},
      {"query", query},
      {"limit_applied", limit},
      {"results", arr},
      {"result_size", arr.size()},
  };
}

nlohmann::json AgentService::memory_summary_payload(const std::string& query, int limit) const {
  auto search = memory_search_payload(query, limit);
  const auto& results = search["results"];

  std::unordered_map<std::string, int> session_hits;
  std::unordered_map<std::string, int> source_hits;
  std::vector<std::string> snippets;
  for (const auto& it : results) {
    const std::string sid = it.value("session_id", "");
    if (!sid.empty()) session_hits[sid] += 1;
    const std::string src = it.value("source", "memory_jsonl");
    source_hits[src] += 1;
    const std::string txt = it.value("text", "");
    if (!txt.empty()) snippets.push_back(txt);
  }

  nlohmann::json evidence_latest_basis = nlohmann::json::object();
  auto evidence_render = build_task_audit_evidence_render_from_results(results, query, search.value("limit_applied", limit));
  if (evidence_render.contains("latest_basis") && evidence_render["latest_basis"].is_object()) {
    evidence_latest_basis = evidence_render["latest_basis"];
  }
  const int limit_applied = search.value("limit_applied", limit);
  const nlohmann::json memory_replay_hint = build_memory_replay_hint(
      evidence_latest_basis,
      query,
      limit_applied,
      "memory_summary");

  std::vector<std::pair<std::string, int>> ranking(session_hits.begin(), session_hits.end());
  std::sort(ranking.begin(), ranking.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;
  });

  nlohmann::json top_sessions = nlohmann::json::array();
  const size_t top_n = std::min<size_t>(3, ranking.size());
  for (size_t i = 0; i < top_n; ++i) {
    top_sessions.push_back({{"session_id", ranking[i].first}, {"hits", ranking[i].second}});
  }

  nlohmann::json source_counts = nlohmann::json::object();
  for (const auto& [src, cnt] : source_hits) source_counts[src] = cnt;

  std::string summary;
  if (snippets.empty()) {
    summary = "未检索到匹配记忆。";
  } else {
    summary = "命中" + std::to_string(snippets.size()) + "条记忆";
    if (!query.empty()) summary += "（query=" + query + "）";
    summary += "。";
    const size_t pick_n = std::min<size_t>(3, snippets.size());
    for (size_t i = 0; i < pick_n; ++i) {
      summary += " #" + std::to_string(i + 1) + ": " + snippets[i];
    }
  }

  return {
      {"type", "memory_summary_result"},
      {"query", query},
      {"limit_applied", search["limit_applied"]},
      {"hit_count", search["result_size"]},
      {"top_sessions", top_sessions},
      {"source_counts", source_counts},
      {"summary", summary},
      {"evidence_latest_basis", evidence_latest_basis},
      {"memory_replay_hint", memory_replay_hint},
  };
}

nlohmann::json AgentService::event_recent_payload() const {
  nlohmann::json arr = nlohmann::json::array();
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& e : recent_events_) arr.push_back(e);
  }
  return {
      {"type", "event_recent_result"},
      {"events", arr},
      {"event_size", arr.size()},
  };
}

nlohmann::json AgentService::metrics_payload() const {
  nlohmann::json by_type = nlohmann::json::object();
  double avg = 0.0;
  double last = 0.0;
  int type_count = 0;
  int event_count = 0;
  int event_window_limit = 0;
  int history_budget_tokens = 0;
  int tool_cache_entries = 0;
  int active_session_count = 0;
  uint64_t total = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    total = total_requests_;
    last = last_latency_ms_;
    for (const auto& [k, v] : type_counters_) {
      by_type[k] = v;
    }
    if (total > 0) {
      avg = total_latency_ms_ / static_cast<double>(total);
    }
    type_count = static_cast<int>(type_counters_.size());
    event_count = static_cast<int>(recent_events_.size());
    event_window_limit = static_cast<int>(event_window_);
    history_budget_tokens = ctx_cfg_.history_budget_tokens;
    tool_cache_entries = static_cast<int>(tool_result_cache_.size());
    active_session_count = static_cast<int>(session_chat_memory_.size());
  }
  return {
      {"type", "metrics_result"},
      {"data",
       {
           {"total_requests", total},
           {"by_type", by_type},
           {"type_count", type_count},
           {"event_count", event_count},
           {"event_window_limit", event_window_limit},
           {"history_budget_tokens", history_budget_tokens},
           {"tool_cache_entries", tool_cache_entries},
           {"active_session_count", active_session_count},
           {"last_latency_ms", last},
           {"avg_latency_ms", avg},
       }},
  };
}

nlohmann::json AgentService::agent_tool_status() const {
  return const_cast<AgentService*>(this)->status_payload();
}

nlohmann::json AgentService::agent_tool_memory_recent(const std::string& session_id) const {
  return const_cast<AgentService*>(this)->memory_recent_payload(session_id);
}

nlohmann::json AgentService::agent_tool_memory_search(const std::string& query, int limit) const {
  return const_cast<AgentService*>(this)->memory_search_payload(query, limit);
}

nlohmann::json AgentService::agent_tool_event_recent() const {
  return const_cast<AgentService*>(this)->event_recent_payload();
}

nlohmann::json AgentService::hello(const std::string& session_id) const {
  return {
      {"type", "hello"},
      {"agent", "thin_agent_cpp"},
      {"version", kThinAgentVersion},
      {"session_id", session_id},
      {"status", status_payload()},
  };
}

/// 多轮槽位续接入口，见 AgentService.h。
nlohmann::json AgentService::try_fill_slot_state(const std::string& session_id, const std::string& text) {
  auto it = slot_states_.find(session_id);
  if (it == slot_states_.end() || it->second.pending_intent.empty()) {
    return nullptr;
  }

  auto& st = it->second;
  st.clarify_turns++;

  // 超时重置
  if (st.clarify_turns > SlotState::kMaxClarifyTurns) {
    slot_states_.erase(it);
    return nullptr;
  }

  const std::string norm = normalize_text_for_intent(text);
  const std::string& intent = st.pending_intent;

  if (should_abort_slot_for_intent_switch(norm, intent)) {
    slot_states_.erase(it);
    return nullptr;
  }

  // 检查用户是否在纠正/切换意图：太长的文本可能不是槽值
  bool slot_filled = false;

  // 根据 pending intent 尝试从文本提取槽值
  IntentSpec pending_spec;
  const bool has_pending_spec = load_intent_spec(intent, &pending_spec) && pending_spec.kind == "external";
  if (has_pending_spec) {
    if (pending_spec.slot_detector == "weather_city") {
      const std::string city = resolve_weather_city_candidate(text, norm);
      if (!city.empty()) {
        st.filled_slots["city"] = city;
        st.missing_slots.erase(
            std::remove(st.missing_slots.begin(), st.missing_slots.end(), "city"),
            st.missing_slots.end());
        slot_filled = true;
      }
    } else if (pending_spec.slot_detector == "news_topic") {
      std::string topic;
      for (const auto& suffix : news_followup_suffixes()) {
        auto pos = text.find(suffix);
        if (pos != std::string::npos && pos > 0) {
          topic = text.substr(0, pos);
          break;
        }
      }
      if (topic.empty() && text.size() <= 12) {
        topic = text;
      }
      if (!topic.empty()) {
        // v0.53.16: 全角标点用字符串后缀比较——多字节字面量在 '' 中是 implementation-defined
  // 多字符常量（-Wmultichar，碰巧按字节序列工作但写法危险）
  auto ends_with_fullwidth = [](const std::string& s, const char* fw) {
    size_t n = std::strlen(fw);
    return s.size() >= n && s.compare(s.size() - n, n, fw) == 0;
  };
  while (!topic.empty() &&
         (topic.back() == ' ' || topic.back() == '?' ||
          ends_with_fullwidth(topic, "\xEF\xBC\x9F") /* ？ */)) {
          topic.pop_back();
        }
        st.filled_slots["topic_or_scope"] = topic;
        st.missing_slots.erase(
            std::remove(st.missing_slots.begin(), st.missing_slots.end(), "topic_or_scope"),
            st.missing_slots.end());
        slot_filled = true;
      }
    }
  } else if (intent == "general") {
    if (text.size() >= 3) {
      st.filled_slots["query"] = text;
      st.missing_slots.clear();
      slot_filled = true;
    }
  } else if (intent == "weather_advice") {
    std::string norm_text = text;
    for (auto& c : norm_text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!norm_text.empty() && (norm_text.back() == ' ' || norm_text.back() == '!' || norm_text.back() == '?')) {
      norm_text.pop_back();
    }
    bool confirmed = false;
    for (const auto& kw : weather_confirm_replies()) {
      if (norm_text == kw) {
        confirmed = true;
        break;
      }
    }
    if (confirmed) {
      slot_filled = true;
    }
  }

  // 如果用户输入未能填槽 → 清除状态，让正常意图匹配接管
  if (!slot_filled) {
    slot_states_.erase(it);
    return nullptr;
  }

  // 如果还有缺失槽位 → 继续追问
  if (!st.missing_slots.empty()) {
    std::string ask;
    const std::string slot_query_lang = detect_query_language(text);
    const std::string slot_tpl_lang = pick_template_lang(slot_query_lang, text);
    if (has_pending_spec && !pending_spec.clarify_template_key.empty()) {
      ask = policy_text_for_lang(pending_spec.clarify_template_key, slot_tpl_lang, "");
    } else {
      ask = policy_text("general.clarify", "");
    }
    ask = apply_reply_translation_if_needed(cfg_, std::move(ask), slot_query_lang, slot_tpl_lang, text);
    return nlohmann::json{
        {"type", "chat_result"},
        {"mode_used", "local-agent"},
        {"intent_backend", "rules"},
        {"memory_size", st.session_snapshot.size()},
        {"decision", {{"route", "local_clarify_slot"}, {"reason", "slot_filling_retry"}, {"policy", "clarify"}, {"intent", intent}, {"confidence", 0.5}}},
        {"tool_calls", nlohmann::json::array()},
        {"decision_trace", nlohmann::json::array({
            {{"layer", "slot"}, {"input", text}, {"output", nlohmann::json{{"pending_intent", intent}, {"filled", st.filled_slots}, {"missing", st.missing_slots}}}},
            {{"layer", "policy"}, {"input", nlohmann::json{{"reason", "slot_filling_retry"}, {"route", "local_clarify_slot"}, {"policy", "clarify"}}}, {"output", nlohmann::json{{"route", "local_clarify_slot"}, {"policy", "clarify"}}}}
        })},
        {"text", ask},
        {"observation", {{"slot_state", {{"pending_intent", intent}, {"filled", st.filled_slots}, {"missing", st.missing_slots}, {"turns", st.clarify_turns}}},
                          {"query_lang", slot_query_lang},
                          {"template_lang", slot_tpl_lang}}},
    };
  }

  // 所有槽位填满 → 执行 pending intent，清除状态
  const auto filled = st.filled_slots;
  const auto snapshot = st.session_snapshot;
  slot_states_.erase(it);

  // 构造一个等效的 chat text 让后续流程识别
  if (has_pending_spec) {
    const std::string slot_val = filled.value(pending_spec.primary_slot, "");
    const std::string suffix = !pending_spec.query_suffix.empty()
                                   ? pending_spec.query_suffix
                                   : (pending_spec.slot_detector == "weather_city"
                                          ? weather_query_suffix()
                                          : news_query_suffix());
    return handle_chat(session_id, slot_val + suffix);
  } else if (intent == "general") {
    const std::string query = filled.value("query", text);
    return handle_chat(session_id, query);
  } else if (intent == "weather_advice") {
    const std::string city = filled.value("city", "");
    const int temp = filled.value("temp_c", 25);
    const std::string cond = filled.value("condition", "未知");
    const int humidity = filled.value("humidity", 60);

    const std::string query_lang = filled.value("query_lang", detect_query_language(text));
    const std::string tpl_lang = pick_template_lang(query_lang, text);
    const std::string advice_lang = (tpl_lang == "zh" || tpl_lang == "zh-TW") ? "zh" : "en";
    std::string advice = render_weather_advice(city, temp, cond, humidity, advice_lang);
    advice = apply_reply_translation_if_needed(cfg_, std::move(advice), query_lang, tpl_lang, text);

    note_dialog_state(session_id, "weather_advice", "local_weather_advice", filled);
    return nlohmann::json{
        {"type", "chat_result"},
        {"mode_used", "local-agent"},
        {"intent_backend", "rules"},
        {"memory_size", snapshot.size()},
        {"text", advice},
        {"decision", {{"route", "local_weather_advice"}, {"reason", "user_confirmed_suggestion"}, {"policy", "execute"}, {"intent", "weather_advice"}, {"confidence", 0.95}}},
        {"tool_calls", nlohmann::json::array()},
        {"observation", {{"advice_context", {{"city", city}, {"temp", temp}, {"condition", cond}, {"humidity", humidity}}},
                          {"query_lang", query_lang},
                          {"template_lang", tpl_lang}}},
        {"decision_trace", nlohmann::json::array({
            {{"layer", "slot"}, {"input", text}, {"output", nlohmann::json{{"pending_intent", "weather_advice"}, {"filled", filled}}}},
            {{"layer", "policy"}, {"input", nlohmann::json{{"reason", "user_confirmed_suggestion"}, {"route", "local_weather_advice"}, {"policy", "execute"}}}, {"output", nlohmann::json{{"route", "local_weather_advice"}, {"policy", "execute"}}}}
        })},
    };
  }

  return nullptr;
}

/// 统一请求分发入口，见 AgentService.h。
// v0.47.1: token 用量统计查询（线程安全，原子读取）
nlohmann::json AgentService::usage_stats() const {
  int64_t pt = total_prompt_tokens_.load(std::memory_order_relaxed);
  int64_t ct = total_completion_tokens_.load(std::memory_order_relaxed);
  int64_t calls = total_api_calls_.load(std::memory_order_relaxed);
  return {
      {"prompt_tokens", pt},
      {"completion_tokens", ct},
      {"total_tokens", pt + ct},
      {"api_calls", calls},
  };
}

// v0.50.8: 运维汇总统计（/stats HTTP 口的数据源，复用现有各子系统 stats）
nlohmann::json AgentService::ops_stats() const {
  nlohmann::json j;
  j["version"] = std::string(kThinAgentVersion);
  j["uptime_seconds"] = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - start_time_)
                            .count();
  j["usage"] = usage_stats();
  if (response_cache_) j["cache"] = response_cache_->stats();
  if (cron_scheduler_) j["cron"] = cron_scheduler_->stats();
  // v0.53.25: 实时状态卡数据源——熔断全量快照+宿主层队列回调+活跃会话
  {
    nlohmann::json cb = nlohmann::json::array();
    for (const auto& [ep, st] : LlmCircuitBreaker::instance().snapshot()) {
      auto row = st;
      row["endpoint"] = ep;
      cb.push_back(row);
    }
    j["circuit"] = cb;
  }
  if (ws_stats_provider_) {  // v0.53.25: 宿主层注入（队列深度/worker/丢弃）
    nlohmann::json q;
    try {
      q = ws_stats_provider_();
    } catch (...) { /* 回调异常不拖垮 /stats */ }
    if (!q.is_null()) j["ws"] = q;
  }
  {
    nlohmann::json arr = nlohmann::json::array();
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [sid, mem] : session_chat_memory_)
      arr.push_back({{"session", sid}, {"messages", mem.size()},
                     {"project_ctx", session_projects_.count(sid) > 0}});
    j["sessions"] = {{"active", arr.size()}, {"list", arr}};
  }
  return j;
}


// v0.53.35: Prometheus 文本格式（https://prometheus.io/docs/instrumenting/exposition_formats）
std::string AgentService::metrics_prometheus() const {
  const auto j = ops_stats();
  std::string out;
  auto emit = [&out](const std::string& name, const std::string& type,
                     const std::string& help, const std::string& value,
                     const std::string& labels = "") {
    out += "# HELP " + name + " " + help + "\n";
    out += "# TYPE " + name + " " + type + "\n";
    out += name + labels + " " + value + "\n";
  };
  auto num = [&](const char* section, const char* key) -> std::string {
    // 整数优先（counter/gauge 整值直出，避免 0.000000）
    const auto& v = j.at(section).at(key);
    if (v.is_number_integer())
      return std::to_string(v.get<long>());
    return std::to_string(v.get<double>());
  };
  try {
    emit("thin_agent_version_info", "gauge", "Build version info", "1",
         "{version=\"" + j.at("version").get<std::string>() + "\"}");
    emit("thin_agent_uptime_seconds", "gauge", "Process uptime",
         std::to_string(j.at("uptime_seconds").get<long>()));
    emit("thin_agent_api_calls_total", "counter", "LLM API calls",
         num("usage", "api_calls"));
    emit("thin_agent_prompt_tokens_total", "counter", "Prompt tokens consumed",
         num("usage", "prompt_tokens"));
    emit("thin_agent_completion_tokens_total", "counter",
         "Completion tokens consumed", num("usage", "completion_tokens"));
    if (j.contains("cache")) {
      emit("thin_agent_cache_hits_total", "counter", "Response cache hits",
           num("cache", "hits"));
      emit("thin_agent_cache_misses_total", "counter", "Response cache misses",
           num("cache", "misses"));
      emit("thin_agent_cache_entries", "gauge", "Response cache entries",
           num("cache", "entries"));
    }
    if (j.contains("cron")) {
      emit("thin_agent_cron_total", "counter", "Cron jobs executed",
           num("cron", "total"));
      emit("thin_agent_cron_enabled", "gauge", "Cron jobs enabled",
           num("cron", "enabled"));
    }
    if (j.contains("ws")) {
      emit("thin_agent_ws_queue_depth", "gauge", "Chat queue depth",
           num("ws", "queue_depth"));
      emit("thin_agent_ws_workers", "gauge", "Chat workers",
           num("ws", "workers"));
      if (j.at("ws").contains("dropped")) {  // 可选键——缺失不炸整体导出
        emit("thin_agent_ws_queue_dropped_total", "counter",
             "Chat queue drops (capacity exceeded)", num("ws", "dropped"));
      }
    }
    if (j.contains("sessions")) {  // v0.53.35: 逐段容错——一段缺失不拖垮全表
      emit("thin_agent_sessions_active", "gauge", "Active chat sessions",
           std::to_string(j.at("sessions").at("active").get<size_t>()));
    }
    if (j.contains("circuit")) {
      for (const auto& cb : j.at("circuit")) {
        const std::string ep = cb.at("endpoint").get<std::string>();
        // label 值转义：反斜杠/引号
        std::string esc;
        for (char c : ep) {
          if (c == '\\' || c == '"') esc += '\\';
          esc += c;
        }
        std::string state = "closed";
        try { state = cb.at("state").get<std::string>(); } catch (...) {}
        emit("thin_agent_circuit_state", "gauge",
             "LLM endpoint circuit state (0=closed 1=half-open 2=open)",
             state == "open" ? "2" : (state == "half_open" ? "1" : "0"),
             "{endpoint=\"" + esc + "\"}");
      }
    }
  } catch (const std::exception& e) {
    out += "# export error: " + std::string(e.what()) + "\n";
  }
  return out;
}

// v0.47.3: 中断指定 session 的 chat 请求
void AgentService::abort_chat(const std::string& session_id) {
  std::lock_guard<std::mutex> lk(mu_);
  aborted_sessions_.insert(session_id);
  log_event("abort", LogLevel::Info, "session marked as aborted",
            {{"session", session_id}});
}

// v0.47.3: 检查并清除中断标志（FC 循环调用）
bool AgentService::check_and_clear_abort(const std::string& session_id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = aborted_sessions_.find(session_id);
  if (it != aborted_sessions_.end()) {
    aborted_sessions_.erase(it);
    return true;
  }
  return false;
}

/// v0.50.4: 分解/合成域（agent_decompose/decompose_and_run/agent_synthesize）。


/// v0.52.4(L3): 收集会话项目的测试命令（按构建系统存在性探测——
/// 按类不按语言：ctest/pytest/go/npm/cargo/maven/gradle，新语言只
/// 改此表不动结构）。无项目上下文或无已知测试系统 → 空数组（跳过）。
std::vector<std::string> AgentService::collect_project_test_commands(
    const std::string& session_id) const {
  std::vector<std::string> cmds;
  PathValidator::ProjectContext pc;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = session_projects_.find(session_id);
    if (it == session_projects_.end() || !it->second.valid) return cmds;
    pc = it->second;
  }
  const std::string root = pc.project_root;
  namespace fs = std::filesystem;
  std::error_code ec;

  // CMake+ctest：build 目录存在即跑（注意 -C 指定配置）
  if (fs::exists(root + "/CMakeLists.txt", ec)) {
    for (const char* bd : {"build", "cmake-build-debug", "_build"}) {
      if (fs::is_directory(root + "/" + bd, ec)) {
        cmds.push_back("ctest --test-dir " + root + "/" + bd +
                       " --output-on-failure");
        break;
      }
    }
  }
  // Python：pytest 入口存在
  if (fs::exists(root + "/pytest.ini", ec) ||
      fs::exists(root + "/pyproject.toml", ec) ||
      fs::exists(root + "/tests", ec))
    cmds.push_back("python3 -m pytest " + root + " -q --no-header -x");
  // Go
  if (fs::exists(root + "/go.mod", ec))
    cmds.push_back("go test " + root + "/...");
  // Node
  if (fs::exists(root + "/package.json", ec))
    cmds.push_back("npm --prefix " + root + " test");
  // Rust
  if (fs::exists(root + "/Cargo.toml", ec))
    cmds.push_back("cargo test --manifest-path " + root + "/Cargo.toml");
  return cmds;
}

nlohmann::json AgentService::goal_auto_reason() {
  nlohmann::json results = nlohmann::json::array();

  if (!goal_mgr_) {
    return {{"ok", false}, {"error", "no goal manager"}};
  }

  auto stalled = goal_mgr_->stalled_goals(1);  // 1 小时未更新 = 滞停
  auto active = goal_mgr_->list("active");

  // 优先级: 滞停目标先处理
  std::vector<agent::Goal> to_process;
  for (const auto& g : stalled) to_process.push_back(g);
  for (const auto& g : active) {
    bool already = false;
    for (const auto& s : stalled)
      if (s.id == g.id) { already = true; break; }
    if (!already) to_process.push_back(g);
  }

  // v0.53.99 (R86): **单次调用处理上限**。此前对 to_process 里**每一个** goal 都串行跑一遍
  // 完整 chat/FC 管线（handle_request），无上限、无单项预算 → 单请求耗时随目标数线性增长
  // （实测：本机数个目标 = 42~44s；20 个目标即分钟级），期间独占一个 WS worker。
  // 这是"批量串行执行无上限"缺陷（与 R74/R75 无界队列同族：只是这里的"界"是时间）。
  // 现在：默认最多处理 3 个（THIN_AGENT_GOAL_AUTO_REASON_MAX 可调），并在返回值里**如实
  // 回报** 已处理数 / 是否截断 / 剩余数——不做静默丢弃。
  int max_goals = 3;
  if (const char* e = std::getenv("THIN_AGENT_GOAL_AUTO_REASON_MAX")) {
    const int v = std::atoi(e);
    if (v > 0) max_goals = v;
  }
  const int total = static_cast<int>(to_process.size());
  if (total > max_goals) to_process.resize(static_cast<size_t>(max_goals));

  for (const auto& goal : to_process) {
    // 为每个目标生成推理提示
    std::string prompt =
        "[GOAL_DRIVEN_TASK]\n"
        "You are working on the following goal: \"" +
        goal.description + "\"\n"
        "Current progress: " + std::to_string(goal.progress_pct) + "%\n"
        "Last updated: " + goal.updated_at + "\n"
        "Please analyze what needs to be done next, then take concrete actions "
        "using available tools to make progress on this goal.\n"
        "[/GOAL_DRIVEN_TASK]";

    // 通过内部 chat 管线执行
    nlohmann::json req;
    req["type"] = "chat";
    req["text"] = prompt;
    req["goal_id"] = goal.id;

    auto result = handle_request("goal_drive_" + std::to_string(goal.id), req);

    nlohmann::json entry;
    entry["goal_id"] = goal.id;
    entry["goal_description"] = goal.description;
    entry["was_stalled"] = (goal.progress_pct < 100);
    entry["chat_result_type"] = result.value("type", "unknown");

    // 如果 chat 返回了 decision，尝试更新进度
    if (result.contains("decision")) {
      goal_mgr_->update_progress(goal.id,
          std::min(goal.progress_pct + 10, 100));
    }

    results.push_back(entry);
  }

  // v0.53.99: 截断要**可见**（调用方/UI 能区分"就这些目标"与"还有没处理的"）
  nlohmann::json out;
  out["results"] = results;
  out["processed"] = static_cast<int>(results.size());
  out["total_goals"] = total;
  out["truncated"] = total > static_cast<int>(results.size());
  out["remaining"] = total > static_cast<int>(results.size())
                         ? total - static_cast<int>(results.size())
                         : 0;
  if (out["truncated"].get<bool>()) {
    std::cerr << "[goal_auto_reason] truncated: processed " << out["processed"]
              << " of " << total << " goals (max per call = " << max_goals
              << "; raise THIN_AGENT_GOAL_AUTO_REASON_MAX to process more)\n";
  }
  return out;
}

  // ── workflow handler 已迁移至 libskill_workflow.so 插件 ──

std::string AgentService::summarize_pipeline_result(
    const nlohmann::json& exec_result,
    const std::string& user_text,
    const std::string& idem_token,
    EventCallback on_event,
    bool use_zh_progress) {

  bool failed = exec_result.value("failed", false);
  auto steps = exec_result.value("pipeline_steps", nlohmann::json::array());
  size_t step_count = steps.size();
  size_t total_output = 0;
  for (const auto& s : steps) {
    if (s.contains("output") && s["output"].is_string()) {
      total_output += s["output"].get<std::string>().size();
    }
  }

  // 路由判定：简单结果 → 本地模板，复杂/失败 → 云 LLM 汇总
  bool is_simple = !failed && step_count <= 2 && total_output < 500;

  // 输出截断上限，从 chat_policy.json 读取（默认 8000 字符）
  int trunc_limit = 8000;
  {
    std::string cfg_val = policy_text("pipeline.output_truncate_chars", "");
    if (!cfg_val.empty()) {
      try { trunc_limit = std::stoi(cfg_val); } catch (...) {}
    }
  }

  if (is_simple) {
    // 本地模板摘要
    // 从 params 提取关键参数展示
    auto param_detail = [](const std::string& action, const nlohmann::json& params) -> std::string {
      if (action == "read_file" || action == "write_file")
        return params.value("path", params.value("file", ""));
      if (action == "list_dir")
        return params.value("dir", params.value("path", ""));
      if (action == "search_code")
        return params.value("pattern", "");
      if (action == "search_files")
        return params.value("pattern", params.value("glob", ""));
      if (action == "shell_exec")
        return params.value("command", params.value("cmd", ""));
      if (action == "capture_photo")
        return (params.contains("count") ? params["count"].dump() : "1") + " 张";
      if (action == "start_recording")
        return params.value("mode", "normal");
      return "";
    };
    std::ostringstream oss;
    oss << "执行完成（" << step_count << " 个步骤）：\n\n";
    for (size_t i = 0; i < steps.size(); ++i) {
      const auto& s = steps[i];
      std::string action = s.value("action", "?");
      bool ok = s.value("success", false);
      std::string detail = s.contains("params") ? param_detail(action, s["params"]) : "";
      oss << (i + 1) << ". " << action;
      if (!detail.empty()) oss << ": " << detail;
      oss << " → " << (ok ? "✅" : "❌")
          << (s.contains("error") ? " " + s["error"].get<std::string>() : "") << "\n";
      if (ok && s.contains("output") && s["output"].is_string()) {
        std::string out = s["output"].get<std::string>();
        if (out.size() > static_cast<size_t>(trunc_limit)) out = out.substr(0, trunc_limit) + "\n...[truncated]";
        oss << "   ```\n   " << out << "\n   ```\n";
      }
    }
    return oss.str();
  }

  // 复杂/失败 → 云 LLM 汇总
  if (on_event) {
    on_event("thinking", {{"tier", 3}, {"msg", progress_msg("summarizing_result", use_zh_progress)}, {"cmd_id", idem_token}});
  }

  std::string summary_prompt;
  if (failed) {
    summary_prompt = use_zh_progress
      ? "用户请求执行多步骤任务，但中途失败。以下是执行详情。请用中文输出一段简洁的诊断回复，"
        "告诉用户哪些步骤完成了、哪一步失败、失败原因是什么，以及建议下一步怎么做。\n\n"
        "用户原始请求: " + user_text + "\n\n"
        "执行结果 JSON:\n" + exec_result.dump(2)
      : "The user requested a multi-step task, but it partially failed. Here are the execution details. "
        "Output a concise diagnostic reply in the same language as the user's request, "
        "telling them which steps completed, which step failed, why, and what to do next.\n\n"
        "User request: " + user_text + "\n\n"
        "Execution result JSON:\n" + exec_result.dump(2);
  } else {
    summary_prompt = use_zh_progress
      ? "用户请求执行多步骤任务，全部成功。以下是完整的执行结果。请用中文输出一段简洁的总结回复，"
        "提取关键信息呈现给用户。\n\n"
        "用户原始请求: " + user_text + "\n\n"
        "执行结果 JSON:\n" + exec_result.dump(2)
      : "The user requested a multi-step task, all steps succeeded. Here are the execution results. "
        "Output a concise summary in the same language as the user's request, "
        "extracting key information for the user.\n\n"
        "User request: " + user_text + "\n\n"
        "Execution result JSON:\n" + exec_result.dump(2);
  }

  const std::string system_prompt = use_zh_progress
    ? "你是 thin_agent，用中文回复，简洁专业。"
    : "You are thin_agent. Reply in the same language as the user's request. Be concise and professional.";

  const std::vector<ChatMessage> messages = {
    ChatMessage{"system", system_prompt},
    ChatMessage{"user", summary_prompt},
  };

  CurlHttpClient http(cfg_.request_timeout_ms);  // v0.52.3: 超时对齐配置
  const auto cloud = CloudLlmClient::chat_completion_with_fallback(
      http, cfg_, messages, "", nlohmann::json::object());

  if (cloud.ok && !cloud.text.empty()) {
    return cloud.text;
  }

  // fallback: 本地摘要
  auto param_detail = [](const std::string& action, const nlohmann::json& params) -> std::string {
    if (action == "read_file" || action == "write_file")
      return params.value("path", params.value("file", ""));
    if (action == "list_dir")
      return params.value("dir", params.value("path", ""));
    if (action == "search_code")
      return params.value("pattern", "");
    if (action == "shell_exec")
      return params.value("command", params.value("cmd", ""));
    return "";
  };
  std::ostringstream oss;
  if (failed) {
    oss << "执行时出错：\n";
  } else {
    oss << "执行完成（" << step_count << " 步）：\n";
  }
  for (size_t i = 0; i < steps.size(); ++i) {
    const auto& s = steps[i];
    std::string action = s.value("action", "?");
    std::string detail = s.contains("params") ? param_detail(action, s["params"]) : "";
    oss << "  " << (i + 1) << ". " << action;
    if (!detail.empty()) oss << ": " << detail;
    oss << " → " << (s.value("success", false) ? "✅" : "❌")
        << (s.contains("error") ? " " + s["error"].get<std::string>() : "") << "\n";
    if (s.contains("output") && s["output"].is_string()) {
      std::string o = s["output"].get<std::string>();
      if (o.size() > static_cast<size_t>(trunc_limit)) o = o.substr(0, trunc_limit) + "\n...[truncated]";
      oss << "     " << o << "\n";
    }
  }
  return oss.str();
}

// ── v0.29.0: 非阻塞 webhook 入队 ──────────────────────────────────

void AgentService::enqueue_webhook(const std::string& source,
                                   const std::string& event,
                                   const std::string& payload) {
  std::lock_guard<std::mutex> lock(mu_);
  pending_webhooks_.push_back({source, event, payload});
  if (pending_webhooks_.size() > 50) {
    pending_webhooks_.pop_front();  // 防止内存泄漏
  }
}

}  // namespace thin_agent
