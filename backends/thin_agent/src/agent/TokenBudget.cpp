#include "thin_agent/agent/TokenBudget.h"

#include <cctype>
#include <sstream>

namespace thin_agent {

// ── 启发式 token 估算 ──────────────────────────────────────────
//
// 无外部依赖的快速估算。实际 BPE tokenizer 会因词汇表覆盖率有所不同，
// 但中英文混合场景下误差通常在 ±15% 以内，足够用于预算决策。
//
// CJK 统一表意文字范围（U+4E00–U+9FFF, U+3400–U+4DBF, U+F900–U+FAFF）
// 以及全角标点（U+3000–U+303F, U+FF00–U+FFEF）统一按 CJK 处理。

namespace {
bool is_cjk_char(unsigned int cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x3000 && cp <= 0x303F) ||   // CJK 标点
         (cp >= 0xFF00 && cp <= 0xFFEF) ||   // 全角字符
         (cp >= 0x2E80 && cp <= 0x2FDF) ||   // CJK 部首
         (cp >= 0x20000 && cp <= 0x2FFFF);   // 扩展 B-F
}

/// 从一个 UTF-8 序列中解码一个 code point。
unsigned int decode_utf8(const char*& p, const char* end) {
  if (p >= end) return 0;
  unsigned char c = static_cast<unsigned char>(*p);
  unsigned int cp = 0;
  int len = 0;

  if ((c & 0x80) == 0) {
    cp = c; len = 1;
  } else if ((c & 0xE0) == 0xC0) {
    cp = c & 0x1F; len = 2;
  } else if ((c & 0xF0) == 0xE0) {
    cp = c & 0x0F; len = 3;
  } else if ((c & 0xF8) == 0xF0) {
    cp = c & 0x07; len = 4;
  } else {
    // 无效头字节
    p++; return 0;
  }

  if (p + len > end) { p++; return 0; }
  for (int i = 1; i < len; i++) {
    cp = (cp << 6) | (static_cast<unsigned char>(p[i]) & 0x3F);
  }
  p += len;
  return cp;
}
}  // namespace

int TokenBudget::estimate_text(const std::string& text) {
  if (text.empty()) return 0;

  int cjk_chars = 0, ascii_chars = 0;
  const char* p = text.data();
  const char* end = p + text.size();

  while (p < end) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x80) {
      ascii_chars++;
      p++;
    } else {
      unsigned int cp = decode_utf8(p, end);
      if (cp == 0) {
        // 无效 UTF-8 字节，跳过
        p++;
        ascii_chars++;
      } else if (is_cjk_char(cp)) {
        cjk_chars++;
      } else {
        // 非 CJK 非 ASCII（如希腊文、阿拉伯文等），按 ASCII 处理
        ascii_chars++;
      }
    }
  }

  // CJK: ~2 字符 / token；ASCII: ~4 字符 / token
  return (cjk_chars / 2) + (ascii_chars / 4) + 1;  // +1 向上取整
}

int TokenBudget::estimate_message(const ChatMessage& msg) {
  int tokens = estimate_text(msg.role);       // "system"/"user" 等 ~1 token
  tokens += estimate_text(msg.content);

  // tool_calls JSON 也占 token
  if (!msg.tool_calls.is_null() && msg.tool_calls.is_array() &&
      !msg.tool_calls.empty()) {
    tokens += estimate_text(msg.tool_calls.dump());
  }
  // tool_call_id + name 的 JSON 帧开销
  if (!msg.tool_call_id.empty()) tokens += 2;
  if (!msg.name.empty()) tokens += estimate_text(msg.name) + 2;

  return tokens;
}

int TokenBudget::estimate_messages(const std::vector<ChatMessage>& msgs) {
  int total = 0;
  for (const auto& m : msgs) {
    total += estimate_message(m);
  }
  return total;
}

void TokenBudget::recalc(const std::vector<ChatMessage>& msgs) {
  msg_tokens = estimate_messages(msgs);
}

std::string TokenBudget::status_line() const {
  std::ostringstream oss;
  oss << "[token-budget] " << total() << "/" << context_window
      << " (" << static_cast<int>(usage_pct() * 100) << "% used) "
      << "system=" << system_tokens << " msgs=" << msg_tokens
      << " remaining=" << remaining()
      << " calls=" << total_api_calls;
  return oss.str();
}

}  // namespace thin_agent
