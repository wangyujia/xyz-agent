#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "thin_agent/llm/DemoConfigCompat.h"

// 前向声明（避免循环依赖）
namespace thin_agent { class IHttpClient; }

namespace thin_agent {

/// OpenAI 兼容 chat/completions 单条消息。
struct ChatMessage {
  std::string role;            ///< system / user / assistant / tool
  std::string content;         ///< 消息正文
  std::string tool_call_id;    ///< tool 消息的 call_id（function calling 必需）
  std::string name;            ///< tool 消息的 tool name（可选）
  /// assistant 消息的 tool_calls（可为空 array）
  nlohmann::json tool_calls = nlohmann::json::array();
  /// v0.39.0: 可选的图片 base64（空=纯文本），序列化时 content 变数组
  std::string image_base64;
};

/// 云模型 chat completion 调用结果。
struct CloudChatResult {
  bool ok{false};           ///< 是否成功解析出 assistant 文本
  std::string text;         ///< choices[0].message.content
  int http_status{0};       ///< HTTP 状态码
  std::string error;        ///< 失败原因（curl / JSON / 业务错误）
  /// v0.38.1: FC 循环中执行过的工具名序列（按调用顺序，用于技能提取）
  std::vector<std::string> tool_names_used;
  /// v0.53.30: 回放中途被 abort（全文已生成保留在 text——流式回放中断标志）
  bool aborted{false};
  /// v0.47.1: API 返回的 token 用量（0 表示未解析到/非流式末包无 usage）
  struct Usage {
    int prompt_tokens{0};
    int completion_tokens{0};
    int total_tokens{0};
  } usage;
  /// v0.49.0: HITL — 危险工具待审批（FC 循环遇到 Dangerous 工具且用户未批准）
  bool needs_approval{false};
  std::string pending_tool_name;   ///< 待审批的工具名
  std::string pending_tool_args;   ///< 待审批的工具参数（JSON 字符串）
  /// v0.52.2: 方案 C — 暂停时的 FC 消息快照与迭代进度（供 approve 续跑）
  std::vector<ChatMessage> fc_messages_snapshot;  ///< 含本轮 assistant tool_calls
  int fc_iterations_used{0};                       ///< 已消耗的迭代轮数
};

/// 云大模型 HTTP 客户端：OpenAI 兼容 POST /chat/completions。
/// 单测可通过环境变量 THIN_AGENT_TEST_CLOUD_RESPONSE 注入 mock 响应。
class CloudLlmClient {
 public:
  /// 发起一次 chat completion 请求。
  /// @param http     HTTP 客户端（生产用 CurlHttpClient，测试用 MockHttpClient）
  /// @param cfg      含 provider、model、api_base、timeout 等
  /// @param api_key  Bearer Token；空时尝试 THIN_AGENT_CLOUD_TEST_KEY
  /// @param messages 对话消息列表（含 system prompt）
  /// @param mock_env_var  单测 mock 环境变量名（空串跳过；与 IHttpClient 二选一）
  /// @param response_format  可选：{{\\\"type\\\":\\\"json_object\\\"}} 强制 JSON 输出
  static CloudChatResult chat_completion(IHttpClient& http,
                                         const DemoConfigCompat& cfg,
                                         const std::string& api_key,
                                         const std::vector<ChatMessage>& messages,
                                         const char* mock_env_var = "THIN_AGENT_TEST_CLOUD_RESPONSE",
                                         const nlohmann::json& response_format = nlohmann::json::object());

  /// 多源 fallback：按 cloud_providers 列表顺序尝试，第一个成功即返回。
  /// 若 cloud_providers 为空则回退到传统单 provider 调用。
  /// @param cfg        含 cloud_providers 列表的配置
  /// @param messages   对话消息
  /// @param mock_env_var  单测 mock 环境变量名
  /// @param response_format  可选 JSON 输出格式约束
  /// @return 第一个成功的响应；全部失败则返回最后一个错误（含错误摘要）。
  static CloudChatResult chat_completion_with_fallback(
      IHttpClient& http,
      const DemoConfigCompat& cfg,
      const std::vector<ChatMessage>& messages,
      const char* mock_env_var = "THIN_AGENT_TEST_CLOUD_RESPONSE",
      const nlohmann::json& response_format = nlohmann::json::object());

  /// 带原生 function calling 的 chat completion。
  /// 将 tools (OpenAI format) 直接传给 LLM，解析返回的 tool_calls。
  /// @param cfg      运行配置
  /// @param api_key  Bearer Token
  /// @param messages 对话消息列表
  /// @param tools    OpenAI function calling tools 数组
  /// @param stream   是否启用 SSE streaming（默认 false）
  /// @param out_stream_chunks 如果 stream=true，将每个 chunk 追加到此向量
  static CloudChatResult chat_completion_with_tools(
      IHttpClient& http,
      const DemoConfigCompat& cfg,
      const std::string& api_key,
      const std::vector<ChatMessage>& messages,
      const nlohmann::json& tools,
      bool stream = false,
      std::vector<std::string>* out_stream_chunks = nullptr,
      std::function<void(const std::string&)> on_token = nullptr,
      /// v0.53.38: 思考流透传——SSE delta.reasoning_content 增量回调
      std::function<void(const std::string&)> on_reasoning = nullptr);

  /// v0.53.8: 工具调用链多源 fallback——与 chat_completion_with_fallback
  /// 同款逐源语义，此前仅纯文本链享受 cloud_providers 冗余（FC 主力链
  /// 单源：GLM 熔断=复杂任务全歇）。cloud_providers 为空时与
  /// chat_completion_with_tools 行为完全一致（零开销短路）。
  static CloudChatResult chat_completion_with_tools_fallback(
      IHttpClient& http,
      const DemoConfigCompat& cfg,
      const std::string& api_key,
      const std::vector<ChatMessage>& messages,
      const nlohmann::json& tools,
      bool stream = false,
      std::vector<std::string>* out_stream_chunks = nullptr,
      /// v0.53.33: 真逐 token——非空时 SSE 增量解析直推（网络到达即转发）。
      /// 仅 stream=true 且 http 为 CurlHttpClient 时生效；fallback 换源后
      /// 逐源重置（token 不跨源混流）。
      std::function<void(const std::string&)> on_token = nullptr,
      /// v0.53.38: 思考流透传——SSE delta.reasoning_content 增量回调
      std::function<void(const std::string&)> on_reasoning = nullptr);

  /// 从 LLM 响应中解析 tool_calls。
  struct ParsedToolCall {
    std::string id;
    std::string name;
    nlohmann::json arguments;
  };
  static std::vector<ParsedToolCall> parse_tool_calls(
      const std::string& response_json);

  /// 意图分类结果。
  struct IntentClassifyResult {
    bool ok = false;
    std::string intent;         ///< 分类后的意图标签
    double confidence = 0.0;    ///< 置信度 0-1
    nlohmann::json slots;       ///< 槽位（城市、主题等）
    std::string error;          ///< 错误信息
    std::string model_used;     ///< 实际使用的模型
    int latency_ms = 0;         ///< 延迟（毫秒）
  };

  /// 云意图分类：用 Flash 模型理解用户意图，返回结构化 JSON。
  /// @param cfg     运行配置（含 cloud_providers）
  /// @param text    用户原始输入
  /// @param lang    用户语言（zh/en），选择对应 prompt
  static IntentClassifyResult classify_intent(IHttpClient& http,
                                               const DemoConfigCompat& cfg,
                                               const std::string& text,
                                               const std::string& lang = "zh");

  // ── 可测试的纯函数（提取自 curl 实现，零外部依赖） ──────────

  /// 构建 chat/completions 请求体（不含 HTTP 传输）。
  /// @param messages        对话消息列表
  /// @param model_name      模型名称
  /// @param max_tokens      最大输出 token 数
  /// @param response_format 可选 JSON 输出格式约束
  /// @param tools           可选 function calling tools 数组
  /// @param stream          是否启用 streaming
  /// @param vision          是否支持多模态（image → content 数组序列化）
  static std::string build_request_body(
      const std::vector<ChatMessage>& messages,
      const std::string& model_name,
      int max_tokens = 2048,
      const nlohmann::json& response_format = nlohmann::json::object(),
      const nlohmann::json& tools = nullptr,
      bool stream = false,
      bool vision = false);

  /// 解析 chat/completions 的 HTTP JSON 响应。
  /// @param json_body   响应正文（JSON 字符串）
  /// @param http_status HTTP 状态码
  static CloudChatResult parse_response(const std::string& json_body,
                                        int http_status);

  /// 解析 Server-Sent Events (SSE) 流式响应。
  /// @param sse_text  完整的 SSE 文本（多行 "data: ..."）
  /// @param out_chunks 可选：将每个解析成功的 chunk JSON 追加到此向量
  static CloudChatResult parse_sse_stream(
      const std::string& sse_text,
      std::vector<std::string>* out_chunks = nullptr);

  // ── Anthropic Messages API format conversion (v0.29.0+) ──────────

  static std::string build_anthropic_request_body(
      const std::vector<ChatMessage>& messages,
      const std::string& model_name,
      int max_tokens = 4096,
      const nlohmann::json& tools = nlohmann::json::array());

  static CloudChatResult parse_anthropic_response(
      const std::string& json_body, int http_status);

  static bool is_anthropic_mode(const DemoConfigCompat& cfg);

  static std::string infer_chat_endpoint_for_mode(const DemoConfigCompat& cfg);
};

}  // namespace thin_agent
