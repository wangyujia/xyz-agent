#pragma once

#include <string>

#include "thin_agent/agent/ToolRegistry.h"

namespace thin_agent {
namespace agent {

/// 工具调用结果错误检测器：基于规则判断是否需要重试。
class ErrorDetector {
 public:
  /// 检测工具调用结果是否异常。
  /// @return true 如果结果看起来像错误（需要重试）
  static bool is_tool_error(const ToolCallResult& result) {
    if (!result.ok) return true;

    // 提取文本内容
    std::string r;
    if (result.result.is_string()) {
      r = result.result.get<std::string>();
    } else if (!result.result.is_null()) {
      r = result.result.dump();
    }

    // 空结果 / 无意义返回值
    if (r.empty()) return true;
    if (r == "{}" || r == "[]" || r == "null") return true;
    if (r.size() < 3 && r != "ok" && r != "no") return true;

    return false;
  }

  /// 生成修正提示, 注入到下次 LLM 调用的 system context。
  static std::string correction_hint(const std::string& tool_name,
                                      const ToolCallResult& result) {
    if (!result.ok) {
      return "Previous call to '" + tool_name + "' failed: " +
             result.error + ". Please check parameters and try a corrected call.";
    }
    return "The result from '" + tool_name +
           "' was empty or malformed. Please re-evaluate and adjust the call.";
  }
};

}  // namespace agent
}  // namespace thin_agent
