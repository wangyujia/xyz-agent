// unit_utf8_truncate：UTF-8 安全截断测试

#include <cassert>
#include <iostream>
#include <string>

#include "thin_agent/core/Utf8Util.h"

static int g_failures = 0;

static void check(const std::string& name, const std::string& got,
                  const std::string& want) {
  if (got != want) {
    std::cout << "FAIL: " << name << "\n  got:  ["
              << std::string(got.begin(), got.end()) << "] len=" << got.size()
              << "\n  want: [" << std::string(want.begin(), want.end())
              << "] len=" << want.size() << "\n";
    ++g_failures;
  } else {
    std::cout << "PASS: " << name << "\n";
  }
}

// 校验结果串是合法 UTF-8
static bool is_valid_utf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = s[i];
    if (c < 0x80) {
      ++i;
    } else if ((c & 0xE0) == 0xC0) {
      if (i + 1 >= s.size() || (s[i + 1] & 0xC0) != 0x80) return false;
      i += 2;
    } else if ((c & 0xF0) == 0xE0) {
      if (i + 2 >= s.size() || (s[i + 1] & 0xC0) != 0x80 ||
          (s[i + 2] & 0xC0) != 0x80)
        return false;
      i += 3;
    } else if ((c & 0xF8) == 0xF0) {
      if (i + 3 >= s.size() || (s[i + 1] & 0xC0) != 0x80 ||
          (s[i + 2] & 0xC0) != 0x80 || (s[i + 3] & 0xC0) != 0x80)
        return false;
      i += 4;
    } else {
      return false;
    }
  }
  return true;
}

int main() {
  // 1. 短于限制 → 原样返回
  check("short_ascii", thin_agent::utf8_truncate("abc", 10), "abc");
  check("short_utf8", thin_agent::utf8_truncate("你好", 10), "你好");

  // 2. ASCII 截断
  check("ascii_exact", thin_agent::utf8_truncate("abcdef", 3), "abc");
  check("ascii_over", thin_agent::utf8_truncate("abcdef", 2), "ab");

  // 3. 中文（3 字节/字符）：截断点落在字符中间 → 回退
  // "你好世界" = 12 字节；限制 4 → 完整字符边界是 "你" (3B) → 应得 "你"
  check("cn_boundary", thin_agent::utf8_truncate("你好世界", 4), "你");
  // 限制 5 → "你" (3B) + "好" 缺 1B → 应回退为 "你"
  check("cn_mid", thin_agent::utf8_truncate("你好世界", 5), "你");
  // 限制 6 → "你好" 正好
  check("cn_exact", thin_agent::utf8_truncate("你好世界", 6), "你好");
  // 限制 1 → 落在 "你" 内部 → 空
  check("cn_1", thin_agent::utf8_truncate("你好世界", 1), "");

  // 4. emoji（4 字节）：截断在 emoji 中间 → 回退
  const std::string emoji = "a\xF0\x9F\x8E\x82" "b";  // a🎂b (6B)
  check("emoji_full", thin_agent::utf8_truncate(emoji, 6), emoji);
  check("emoji_5", thin_agent::utf8_truncate(emoji, 5), "a\xF0\x9F\x8E\x82");  // a🎂
  check("emoji_mid", thin_agent::utf8_truncate(emoji, 2), "a");
  check("emoji_partial", thin_agent::utf8_truncate(emoji, 3), "a");

  // 5. 任意截断点都不产生非法 UTF-8（穷举）
  const std::string mixed = "混合text🎂混end";
  for (size_t limit = 0; limit <= mixed.size() + 2; ++limit) {
    std::string t = thin_agent::utf8_truncate(mixed, limit);
    if (t.size() > limit || !is_valid_utf8(t)) {
      std::cout << "FAIL: invalid at limit=" << limit
                << " size=" << t.size() << "\n";
      ++g_failures;
      break;
    }
  }
  std::cout << "PASS: exhaustive validity (" << mixed.size() + 3 << " limits)\n";

  // 6. 空串
  check("empty", thin_agent::utf8_truncate("", 5), "");

  std::cout << (g_failures == 0 ? "unit_utf8_truncate: ALL PASS\n"
                                : "unit_utf8_truncate: FAILURES\n");
  return g_failures == 0 ? 0 : 1;
}
