#pragma once

#include <string>

#include "nlohmann/json.hpp"

namespace thin_agent {

/// v0.52.1: JSON 参数宽容取值——模型传 "20"（字符串）时兼容转 int。
/// 根因：nlohmann 的 value<int>() 遇 string 类型抛 type_error.302，
/// 生产实测（e2e chat-26）GLM 传 {"limit":"5"} 导致 session_recent
/// 工具报 Execution error，FC 收敛追踪判定"错误率 62%"终止任务。
/// 同类 12+ 处 .value("limit", N) 统一收口到本函数。
inline int json_coerce_int(const nlohmann::json& j, const char* key,
                           int default_val) {
  if (!j.is_object() || !j.contains(key)) return default_val;
  const auto& v = j[key];
  if (v.is_number_integer()) return v.get<int>();
  if (v.is_number_unsigned()) return static_cast<int>(v.get<uint64_t>());
  if (v.is_number_float()) return static_cast<int>(v.get<double>());  // 截断
  if (v.is_string()) {
    try {
      return std::stoi(v.get<std::string>());
    } catch (...) {
      return default_val;
    }
  }
  return default_val;
}

}  // namespace thin_agent

/// v0.53.2: UTF-8 安全截断——中文字符 3 字节，substr 切半会产出非法
/// 序列，nlohmann dump（strict 模式）直接抛 type_error.316。
/// 生产实证：reply_text preview 日志 substr(0,200) 切断 0x88 尾巴，
/// cron patrol 链路周期性打崩服务（gdb 栈：LogEvent.h:88 dump）。
inline std::string utf8_safe_truncate(const std::string& s, size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  size_t end = max_bytes;
  // 回退至 UTF-8 序列边界（续字节 10xxxxxx 不能做首字节）
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) {
    --end;
  }
  // end 处可能是多字节首字节——再回退一个完整字符
  if (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) != 0x80) {
    size_t lead = end;
    unsigned char c = static_cast<unsigned char>(s[lead]);
    size_t need = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2
                : (c & 0xF0) == 0xE0 ? 3 : 4;
    if (lead + need > max_bytes) {
      // 该字符放不下——回退到它之前
      end = lead;
    }
  }
  return s.substr(0, end);
}
