// unit_fuzzy_patcher：FuzzyPatcher 全策略覆盖测试

#include <iostream>
#include <string>

#include "thin_agent/core/FuzzyPatcher.h"

static int g_failures = 0;
static void check(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAIL: " << msg << '\n'; ++g_failures; }
}

int main() {
  using thin_agent::FuzzyPatcher;
  FuzzyPatcher fp;

  // ── exact match ──
  {
    std::string c = "hello world\nfoo bar\nbaz qux\n";
    auto m = fp.find(c, "foo bar");
    check(m.found && m.pos == 12 && m.strategy_name == "exact", "exact match");
    auto r = fp.patch(c, "foo bar", "replaced");
    check(r.success && c.find("replaced") != std::string::npos, "exact patch");
  }

  // ── non-unique → not found ──
  {
    std::string c = "int x = 1;\nint y = 2;\nint x = 1;\n";
    auto m = fp.find(c, "int x = 1;");
    check(!m.found, "non-unique safe");
  }

  // ── strip_leading ──
  {
    std::string c = "  int main() {\n    return 0;\n  }\n";
    auto m = fp.find(c, "int main() {\n  return 0;\n}");
    check(m.found && m.strategy_name == "strip_leading", "strip_leading find");
    auto r = fp.patch(c, "int main() {\n  return 0;\n}", "int main() {\n  return 42;\n}");
    check(r.success && c.find("return 42") != std::string::npos, "strip_leading patch");
    check(c.find("  int main()") != std::string::npos, "strip_leading indent preserved");
  }

  // ── strip_leading deep ──
  {
    std::string c = "        if (cond) {\n            foo();\n        }\n";
    auto m = fp.find(c, "if (cond) {\n    foo();\n}");
    check(m.found && m.strategy_name == "strip_leading", "strip_leading deep");
  }

  // ── strip_trailing ──
  {
    std::string c = "hello   \nworld\t\n";
    auto m = fp.find(c, "hello\nworld\n");
    check(m.found && m.strategy_name == "strip_trailing", "strip_trailing find");
    auto r = fp.patch(c, "hello\nworld\n", "hi\nearth\n");
    check(r.success && c.find("hi") != std::string::npos, "strip_trailing patch");
  }

  // ── strip_blank_lines ──
  {
    std::string c = "hello\nworld\n";
    auto m = fp.find(c, "\n\nhello\nworld\n\n");
    check(m.found && m.strategy_name == "strip_blank_lines", "strip_blank_lines find");
    auto r = fp.patch(c, "\n\nhello\nworld\n\n", "hi\nearth\n");
    check(r.success && c == "hi\nearth\n", "strip_blank_lines patch");
  }

  // ── strip_leading + blank_lines ──
  {
    std::string c = "    hello\n    world\n";
    auto m = fp.find(c, "\n\nhello\nworld\n\n");
    check(m.found && m.strategy_name == "strip_leading+blanks", "combo find");
    auto r = fp.patch(c, "\n\nhello\nworld\n\n", "hi\nearth\n");
    check(r.success, "combo patch");
    check(c == "    hi\n    earth\n", "combo indent preserved");
  }

  // ── all strategies fail ──
  {
    std::string c = "abc\ndef\n";
    auto m = fp.find(c, "xyz");
    check(!m.found, "all fail find");
    auto r = fp.patch(c, "xyz", "nope");
    check(!r.success && r.output.find("not_found") != std::string::npos, "all fail patch");
  }

  // ── replace_all exact ──
  {
    std::string c = "// TODO\ncode\n// TODO\nmore\n// TODO\n";
    auto r = fp.patch(c, "// TODO\n", "", true);
    check(r.success && r.replacements == 3, "replace_all exact");
    check(c.find("// TODO") == std::string::npos, "replace_all removed all");
  }

  // ── replace_all fuzzy ──
  {
    std::string c = "// TODO\ncode\n// TODO  \nmore\n// TODO\n";
    auto r = fp.patch(c, "// TODO", "// DONE", true);
    check(r.success && r.replacements >= 2, "replace_all fuzzy count");
    check(c.find("// DONE") != std::string::npos, "replace_all fuzzy done");
  }

  // ── tab indent ──
  {
    std::string c = "\t\tint x = 1;\n\t\treturn x;\n";
    auto m = fp.find(c, "int x = 1;\nreturn x;");
    check(m.found && m.strategy_name == "strip_leading", "tab indent");
  }

  // ── priority: exact wins ──
  {
    std::string c = "int x = 1;\n";
    auto m = fp.find(c, "int x = 1;");
    check(m.found && m.strategy_name == "exact", "priority exact");
  }

  if (g_failures) {
    std::cerr << g_failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "unit:test_fuzzy_patcher PASS\n";
  return 0;
}
