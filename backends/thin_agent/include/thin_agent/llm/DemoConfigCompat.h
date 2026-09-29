#pragma once

#include <string>
#include <vector>

namespace thin_agent {

/// 单个云 Provider 配置（多源 fallback 列表中的一项）。
struct CloudProviderConfig {
  std::string provider;      ///< 云提供商标识（openai-compatible / deepseek / github-copilot）
  std::string model_name;    ///< 该 provider 的模型名
  std::string api_base;      ///< OpenAI 兼容 API 根 URL
  std::string api_key_env;   ///< 读取 API Key 的环境变量名
  int request_timeout_ms{15000};  ///< HTTP 超时（毫秒）

  bool valid() const {
    return !provider.empty() && !model_name.empty() && !api_base.empty();
  }
};

/// v0.52.25: 按 model_name 推断上下文窗口（token）。配置显式值优先；
/// 未知模型保守 32k。GLM-5.2=128k、DeepSeek=64k、claude=200k、qwen=131072。
inline int infer_context_window(const std::string& model_name) {
  auto has = [&](const char* sub) { return model_name.find(sub) != std::string::npos; };
  if (has("glm")) return 128000;
  if (has("deepseek")) return 64000;
  if (has("claude")) return 200000;
  if (has("qwen")) return 131072;
  return 32000;
}

/// Agent 运行时完整配置，由 config/demo.model.yaml 的 profile 解析得到。
/// AgentService / CloudLlmClient / ExternalInfoClient 均依赖此结构。
struct DemoConfigCompat {
  std::string mode{"offline"};       ///< offline / cloud / auto
  std::string provider;              ///< 云提供商，见 ProviderFactory
  std::string model_name;            ///< 云模型名称
  std::string api_base;              ///< OpenAI 兼容 chat/completions 根 URL
  std::string api_key_env;           ///< 读取 API Key 的环境变量名
  int request_timeout_ms{15000};     ///< 云 HTTP 超时（毫秒）
  /// v0.52.8: completion token 预算（0=默认 2048）。思考型模型
  /// reasoning 消耗同一预算——分解等大输出任务需调大防 content 空。
  int max_completion_tokens{0};

  /// v0.52.25: 上下文窗口（token）。0=按 model_name 推断（见
  /// infer_context_window()）。决定 FC 循环中间压缩触发点。
  int context_window_tokens{0};
  std::string fallback{"offline"};   ///< 密钥缺失或调用失败时的回退：offline / cloud

  /// API 协议模式 (v0.29.0+)：
  ///   "chat_completions"    — OpenAI 兼容格式（默认）
  ///   "anthropic_messages"  — Anthropic Messages API 格式
  std::string api_mode;

  /// 多源 fallback 列表（v0.9.1+）：
  /// 按顺序尝试，第一个成功即返回；全部失败则走 fallback。
  /// 若为空则退化为单 provider 模式（兼容旧配置）。
  std::vector<CloudProviderConfig> cloud_providers;

  int budget_max_input_chars{0};     ///< 输入字符预算上限，<=0 表示不启用
  int budget_max_latency_ms{0};      ///< 延迟预算上限，<=0 表示不启用
  double budget_max_cost_cents{0.0}; ///< 估算成本预算（美分），<=0 表示不启用

  bool complex_intent_force_cloud{false};  ///< 检测到复杂意图时是否强制走云策略

  std::string external_provider{"mock"};  ///< 外部查询：mock / http 等
  std::string external_weather_url;         ///< 天气 URL 模板，支持 {city} {date}

  bool pipeline_enable_rollback_hook{false};  ///< 任务流水线失败时是否启用回滚钩子

  /// v0.39.0: 模型是否支持多模态（vision/image）。
  /// true 时 build_payload 将图片序列化为 content 数组，
  /// false（默认）时纯文本模型忽略图片字段。
  bool vision{false};

  /// 校验 mode、provider、预算字段等是否在允许范围内。
  bool valid() const;
};

/// 轻量 YAML profile 解析器，读取 config/demo.model.yaml 中指定 profile 块。
/// @throws std::runtime_error 文件打不开或 profile 不存在
DemoConfigCompat load_demo_profile_compat(const std::string& yaml_path, const std::string& profile);

}  // namespace thin_agent
