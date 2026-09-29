#pragma once

#include <string>

namespace thin_agent {

/// 模型运行配置（精简版），供 Provider 层与 demo 冒烟使用。
/// 完整运行参数见 DemoConfigCompat（含预算门控、外部查询等扩展项）。
struct ModelConfig {
  std::string mode{"offline"};    ///< offline / cloud / auto
  std::string provider;           ///< 提供商标识，如 github-copilot、deepseek
  std::string model_name;         ///< 模型 ID（YAML 字段 name）
  std::string api_base;           ///< OpenAI 兼容 API 根地址（可选）
  std::string api_key_env;        ///< API Key 环境变量名
  int request_timeout_ms{15000};  ///< HTTP 请求超时（毫秒）
  std::string fallback{"offline"};  ///< 云失败时回退策略：offline / cloud

  /// 校验 mode/provider/model/timeout/fallback 组合是否合法。
  bool valid() const;
};

/// 从 config/demo.model.yaml 解析指定 profile 为 ModelConfig（基础字段子集）。
/// 文件不存在时返回默认 offline 配置；profile 未命中时各字段保持默认。
ModelConfig load_demo_profile(const std::string& yaml_path, const std::string& profile);

}  // namespace thin_agent
