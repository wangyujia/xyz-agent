#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {

/// SecurityRedactor — 扫描工具输出中的敏感信息并替换为 [REDACTED]。
///
/// 检测模式：
/// - OpenAI/Anthropic/DeepSeek 等 API key 前缀 (sk-, sk-ant-, deepseek-)
/// - GitHub tokens (ghp_, gho_, ghu_, ghs_, ghr_)
/// - AWS access keys (AKIA, ASIA)
/// - Generic "key=..." / "token=..." / "secret=..." 赋值
/// - JWT tokens (eyJ...)
/// - Base64 高熵长字符串 (> 20 chars)
///
/// 所有方法为 static，无状态，可在任何路径安全调用。
class SecurityRedactor {
 public:
  /// 对字符串进行红标（返回副本，原文不变）。
  static std::string redact_string(const std::string& input);

  /// 递归遍历整个 JSON 对象，红标所有值为字符串的叶子节点。
  static nlohmann::json redact_json(const nlohmann::json& input);

  /// 对工具调用结果执行红标（处理 result 和 error 字段）。
  static nlohmann::json redact_tool_result(const nlohmann::json& result);

 private:
  struct SecretPattern {
    const char* name;       // 人类可读名称
    const char* regex_str;  // POSIX ERE 正表达式（用于 regcomp）
  };

  static const std::vector<SecretPattern>& patterns();
};

}  // namespace thin_agent
