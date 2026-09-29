#pragma once

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "thin_agent/agent/ToolRegistry.h"
#include "thin_agent/llm/CloudLlmClient.h"
#include "thin_agent/llm/DemoConfigCompat.h"

namespace thin_agent {
namespace agent {

class EmbeddingProvider;
class MemoryManager;
class SkillManager;      // v0.39.1: 前向声明用于技能注入

/// Agent 循环配置。
struct AgentLoopConfig {
  int max_turns = 10;
  int max_context_chars = 8000;
  int request_timeout_ms = 30000;
  bool verbose_trace = false;

  // 上下文压缩
  bool enable_compression = true;  ///< 启用摘要压缩
  int compress_after_turns = 4;    ///< 超过此轮数触发压缩

  // Function calling
  bool use_native_fc = true;  ///< 使用 LLM 原生 function calling

  // 人工介入
  bool human_in_the_loop = false;  ///< 危险工具需确认

  // 流式
  bool stream_output = false;  ///< 启用 SSE streaming

  // v0.9.3: 自我纠错
  int max_retries = 2;  ///< 工具调用失败时最大重试次数

  // v0.11.0: 子 Agent 角色工具白名单（空 = 全部可用）
  std::unordered_set<std::string> allowed_tools;
};

/// Agent 循环结果。
struct AgentLoopResult {
  bool ok = false;
  std::string final_answer;
  int turns_used = 0;
  std::vector<ToolCallResult> tool_calls;
  std::string error;
  nlohmann::json trace = nlohmann::json::array();

  // 新增
  bool needs_approval = false;     ///< 需要人工确认
  std::string approval_prompt;     ///< 确认提示文案
  std::string pending_tool_name;   ///< 待确认的工具名
  nlohmann::json pending_tool_params;  ///< 待确认的参数

  /// v0.52.12: 降级标记——最终答案来自本地兜底而非云端 LLM 原始
  /// 输出（ok=true + degraded=true）。spawn 波次据此判任务失败，
  /// 阻止离线兜底文案被当作业务交付落库（假绿终结）。
  bool degraded = false;
};

class AgentLoop {
 public:
  AgentLoop(const DemoConfigCompat& cfg,
            const std::string& api_key,
            ToolRegistry& registry);

  AgentLoop(const DemoConfigCompat& cfg,
            const std::string& api_key,
            ToolRegistry& registry,
            AgentLoopConfig loop_cfg);

  /// 设置记忆管理器（用于上下文增强）。
  void set_memory_manager(MemoryManager* mgr) { memory_mgr_ = mgr; }
  /// v0.38.1: 设置技能管理器 — 子 Agent 注入匹配技能
  void set_skill_manager(SkillManager* mgr) { skill_mgr_ = mgr; }

  struct SubAgentConfig {
    bool enable_checkpoint{false};
    bool enable_approval_before_tool{false};
    bool enable_summarizer{false};
    int budget_max_context_chars{0};
  };

  AgentLoopResult run(const std::string& user_query,
                      const std::string& system_prompt,
                      const nlohmann::json& memory_context = nlohmann::json::object(),
                      /// v0.53.48: 续跑支持——HITL 批准后从暂停现场继续
                      ///（此前 continue_after_approval 调 run("") 重开空对话,
                      /// paused_messages_ 全丢=批准前的工具链断档）
                      std::vector<ChatMessage> resume_messages = {});

  /// 人工确认后继续执行。
  AgentLoopResult continue_after_approval(bool approved);

  // v0.11.0: 工具白名单（public for testing）
  bool is_tool_allowed(const std::string& name) const;
  nlohmann::json filter_tools_json(const nlohmann::json& all_tools) const;
  std::string filter_tools_prompt(const std::string& raw_prompt) const;

  /// v0.53.5: 子代理工具钩子闸（tool_pre 否决；error 非空=被拒）
  ToolCallResult hook_gate(const std::string& name,
                           const nlohmann::json& args) const;

 private:
  nlohmann::json call_llm_native(const std::vector<ChatMessage>& messages,
                                  const nlohmann::json& tools_json);
  nlohmann::json call_llm_text(const std::vector<ChatMessage>& messages);
  nlohmann::json parse_llm_output(const std::string& raw_output);
  ChatMessage build_tool_observation(const std::vector<ToolCallResult>& results);
  int estimate_chars(const std::vector<ChatMessage>& messages);
  std::vector<ChatMessage> compress_messages(const std::vector<ChatMessage>& messages);

  const DemoConfigCompat& cfg_;
  std::string api_key_;
  ToolRegistry& registry_;
  AgentLoopConfig loop_cfg_;
  MemoryManager* memory_mgr_ = nullptr;
  SkillManager* skill_mgr_ = nullptr;   ///< v0.39.1: 子Agent技能匹配注入

  // 人工确认暂停状态
  std::vector<ChatMessage> paused_messages_;
  std::string paused_tool_name_;
  nlohmann::json paused_tool_params_;
  std::string paused_session_trace_id_;
};

}  // namespace agent
}  // namespace thin_agent
