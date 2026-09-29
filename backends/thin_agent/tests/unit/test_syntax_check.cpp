#include "thin_agent/core/SyntaxChecker.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

static int failures = 0;

void check(const std::string& label, const std::string& actual,
           bool expect_empty) {
  bool ok = expect_empty ? actual.empty() : !actual.empty();
  printf("%s %s\n", ok ? "  PASS:" : "  FAIL:", label.c_str());
  if (!ok) {
    ++failures;
    printf("    expected: %s\n    got:      %s\n",
           expect_empty ? "(empty)" : "(non-empty error)",
           actual.c_str());
  } else if (!actual.empty()) {
    printf("    error message: %s\n", actual.c_str());
  }
}

int main() {
  // ── JSON: valid ──
  {
    std::ofstream out("/tmp/test_syntax_ok.json");
    out << "{\"x\": 1, \"y\": 2}";
    out.close();
    check("json valid", thin_agent::syntax_check("/tmp/test_syntax_ok.json"), true);
    std::remove("/tmp/test_syntax_ok.json");
  }

  // ── JSON: bad (trailing comma) ──
  {
    std::ofstream out("/tmp/test_syntax_bad.json");
    out << "{\"x\": 1,}";
    out.close();
    check("json bad trailing comma",
          thin_agent::syntax_check("/tmp/test_syntax_bad.json"), false);
    std::remove("/tmp/test_syntax_bad.json");
  }

  // ── JSON: bad (missing brace) ──
  {
    std::ofstream out("/tmp/test_syntax_bad2.json");
    out << "{\"x\": 1";
    out.close();
    check("json bad missing brace",
          thin_agent::syntax_check("/tmp/test_syntax_bad2.json"), false);
    std::remove("/tmp/test_syntax_bad2.json");
  }

  // ── JSON: empty file ──
  {
    std::ofstream out("/tmp/test_syntax_empty.json");
    out << "";
    out.close();
    check("json empty file",
          thin_agent::syntax_check("/tmp/test_syntax_empty.json"), true);
    std::remove("/tmp/test_syntax_empty.json");
  }

  // ── Python: valid ──
  {
    std::ofstream out("/tmp/test_syntax_ok.py");
    out << "print('hello')\n";
    out.close();
    check("python valid", thin_agent::syntax_check("/tmp/test_syntax_ok.py"), true);
    std::remove("/tmp/test_syntax_ok.py");
  }

  // ── Python: bad ──
  {
    std::ofstream out("/tmp/test_syntax_bad.py");
    out << "prnit('hello')\nx = \n";
    out.close();
    check("python bad syntax",
          thin_agent::syntax_check("/tmp/test_syntax_bad.py"), false);
    std::remove("/tmp/test_syntax_bad.py");
  }

  // ── YAML: valid ──
  {
    std::ofstream out("/tmp/test_syntax_ok.yaml");
    out << "key: value\nlist:\n  - a\n  - b\n";
    out.close();
    check("yaml valid", thin_agent::syntax_check("/tmp/test_syntax_ok.yaml"), true);
    std::remove("/tmp/test_syntax_ok.yaml");
  }

  // ── unknown extension → skip ──
  check("unknown .txt skip",
        thin_agent::syntax_check("/tmp/nonexistent.txt"), true);
  check("no extension skip",
        thin_agent::syntax_check("/tmp/nonexistent"), true);

  printf("\n%s (%d failures)\n",
         failures == 0 ? "ALL syntax_check PASSED" : "SOME FAILED",
         failures);
  return failures > 0 ? 1 : 0;
}
