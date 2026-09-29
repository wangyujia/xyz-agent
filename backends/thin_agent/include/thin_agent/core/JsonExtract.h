#pragma once

#include <string>

#include "nlohmann/json.hpp"

namespace thin_agent {

/// v0.52.3: 从 LLM 原始输出提取含目标键的 JSON 对象。
///
/// 背景：decompose 提示词要求"JSON only"，但实测 GLM 常包 markdown
/// fence（```json ... ```）或前后说明文字；旧实现 find('{')/rfind('}')
/// 对"尾随 } 的说明文字"会截出非法片段（e2e 实锤解析失败）。
///
/// 算法：平衡括号扫描——从每个 '{' 起找字符串感知（转义敏感）的配对
/// '}'，首个可完整解析且含 required_key 数组的段胜出。O(n·k)，
/// n=文本长，k= '{' 个数（LLM 输出 k 小，实际近似线性）。
inline nlohmann::json extract_json_object_with_array(
    const std::string& raw, const std::string& required_key) {
  auto try_parse = [&](size_t s, size_t e) -> nlohmann::json {
    try {
      auto j = nlohmann::json::parse(raw.substr(s, e - s + 1));
      if (j.contains(required_key) && j[required_key].is_array()) return j;
    } catch (...) {}
    return nlohmann::json();
  };
  for (size_t s = raw.find('{'); s != std::string::npos;
       s = raw.find('{', s + 1)) {
    int depth = 0;
    bool in_str = false;
    char prev = 0;
    for (size_t i = s; i < raw.size(); ++i) {
      char c = raw[i];
      if (in_str) {
        if (c == '"' && prev != '\\') in_str = false;
      } else if (c == '"') {
        in_str = true;
      } else if (c == '{') {
        ++depth;
      } else if (c == '}') {
        if (--depth == 0) {
          auto j = try_parse(s, i);
          if (!j.is_null()) return j;
          break;  // 该 '{' 的配对段解析失败（或无 tasks）——试下一个 '{'
        }
      }
      prev = c;
    }
  }
  return nlohmann::json();
}

}  // namespace thin_agent
