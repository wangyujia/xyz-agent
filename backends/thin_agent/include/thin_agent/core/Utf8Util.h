#pragma once

#include <string>

namespace thin_agent {

/// v0.45.2: UTF-8 安全截断。
///
/// 按字节截断字符串到不超过 max_bytes，且**不会切断多字节 UTF-8 字符**。
/// 背景：AgentService 工具结果缓存对输出做 substr(0, 500) 盲截断，
/// 若截断点落在中文/emoji 等多字节字符中间，会产生非法 UTF-8 序列，
/// 后续 nlohmann::json dump 时抛 type_error.316，导致整个云调用失败。
///
/// 规则：检查第一个被丢弃的字节 s[n]（n = max_bytes < s.size()）。
/// 若 s[n] 是 continuation byte（0b10xxxxxx），说明 n 落在某个多字节
/// 字符的内部 → 回退到该字符的起始字节之前，保证不产生半截字符。
inline std::string utf8_truncate(const std::string& s, size_t max_bytes) {
  if (s.size() <= max_bytes) return s;

  size_t n = max_bytes;
  // s[n] 是第一个被丢弃的字节。若是 continuation → 截断点在字符内部，回退。
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) {
    --n;
  }
  return s.substr(0, n);
}

}  // namespace thin_agent
