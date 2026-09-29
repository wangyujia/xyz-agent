#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {
namespace agent {

/// 单个工具参数的 JSON Schema 描述。
struct ToolParameter {
  std::string name;         ///< 参数名
  std::string type;         ///< JSON 类型：string / integer / boolean / object / array
  std::string description;  ///< 参数用途说明
  bool required = false;    ///< 是否必须
  nlohmann::json enum_values;  ///< 可选枚举值列表
};

/// 工具定义：name + description + parameters + 执行回调。
struct ToolSchema {
  std::string name;                ///< 工具唯一标识（如 "weather"）
  std::string description;         ///< 工具功能描述（注入 LLM system prompt）
  std::vector<ToolParameter> parameters;  ///< 参数列表

  /// 执行回调：接收已校验的 JSON 参数，返回 JSON 结果。
  /// 返回格式：{"ok": bool, "result": ..., "error": "..."}
  std::function<nlohmann::json(const nlohmann::json& params)> execute;

  bool dangerous = false;  ///< 是否需要人工确认后才执行

  /// 序列化为 OpenAI function calling 格式。
  nlohmann::json to_openai_function() const;

  /// 序列化为简洁文本描述（用于不支持 function calling 的模型）。
  std::string to_text_description() const;

  /// 校验 JSON 参数，返回错误列表；空列表表示通过。
  std::vector<std::string> validate_params(const nlohmann::json& params) const;
};

/// 工具执行结果。
struct ToolCallResult {
  bool ok = false;
  nlohmann::json result;
  std::string error;
  std::string tool_name;

  /// v0.52.21: 策略性终态错误——重试无意义且会引发重试风暴
  ///（嵌套护栏拒绝后 AgentLoop 自我纠错重试→每轮再 spawn→
  /// 线程乘积爆炸，ASAN 实测 T1801 + heap-buffer-overflow）。
  /// 语义前缀约定：策略/资源上限类错误以 "policy:" 开头标记。
  bool policy_terminal() const {
    return error.rfind("policy:", 0) == 0 ||
           error.find("nesting limit") != std::string::npos;
  }
};

}  // namespace agent
}  // namespace thin_agent
