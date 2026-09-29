/// unit_code_exec：Python 代码执行插件测试
///
/// 测试 thin_agent::code_exec::handle_code_exec

#include <cstdlib>
#include <iostream>
#include <string>

#include "thin_agent/plugin/PluginInterface.h"
#include "../../src/plugin/skills/code_exec.cpp"

#define ASSERT(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << std::endl; return 1; } \
} while (0)

#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { std::cerr << "FAIL: " << msg << " (" << (a) << " != " << (b) << ")" << std::endl; return 1; } \
} while (0)

#define TEST(name) int test_##name()

using namespace nlohmann;

// skip flag
static bool g_python3_ok = false;

static void check_python3() {
  g_python3_ok = (system("python3 --version > /dev/null 2>&1") == 0);
}

/// Empty code should fail
TEST(empty_code) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", ""}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(!result["success"].get<bool>(), "empty code should fail");
  ASSERT_EQ(result["error"].get<std::string>(), "code_required", "error msg mismatch");
  std::cout << "pass: empty_code" << std::endl;
  return 0;
}

/// Simple print statement
TEST(simple_print) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", "print('hello world')"}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(result["success"].get<bool>(), "simple print should succeed");
  ASSERT_EQ(result["exit_code"].get<int>(), 0, "exit code should be 0");
  std::string output = result["output"].get<std::string>();
  ASSERT(output.find("hello world") != std::string::npos, "output should contain 'hello world'");
  std::cout << "pass: simple_print" << std::endl;
  return 0;
}

/// JSON output
TEST(json_output) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", "import json; print(json.dumps({'a': 1, 'b': [2,3]}))"}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(result["success"].get<bool>(), "json print should succeed");
  ASSERT_EQ(result["exit_code"].get<int>(), 0, "exit code should be 0");
  std::string output = result["output"].get<std::string>();
  ASSERT(output.find("\"a\"") != std::string::npos, "should contain 'a'");
  ASSERT(output.find("\"b\"") != std::string::npos, "should contain 'b'");
  std::cout << "pass: json_output" << std::endl;
  return 0;
}

/// Math computation
TEST(math_computation) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", "import math; print(math.factorial(10))"}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(result["success"].get<bool>(), "math should succeed");
  ASSERT_EQ(result["exit_code"].get<int>(), 0, "exit code should be 0");
  std::string output = result["output"].get<std::string>();
  ASSERT(output.find("3628800") != std::string::npos, "should contain 3628800 (10!)");
  std::cout << "pass: math_computation" << std::endl;
  return 0;
}

/// Syntax error
TEST(syntax_error) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", "print("}};  // deliberate syntax error
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(!result["success"].get<bool>(), "syntax error should fail");
  ASSERT(result["exit_code"].get<int>() != 0, "exit code should be non-zero");
  std::string output = result["output"].get<std::string>();
  ASSERT(output.find("Error") != std::string::npos, "should contain 'Error'");
  std::cout << "pass: syntax_error" << std::endl;
  return 0;
}

/// stderr capture
TEST(stderr_capture) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", "import sys; sys.stderr.write('err msg\\n'); print('ok')"}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(result["success"].get<bool>(), "script should succeed");
  std::string output = result["output"].get<std::string>();
  ASSERT(output.find("err msg") != std::string::npos, "should capture stderr");
  ASSERT(output.find("ok") != std::string::npos, "should capture stdout");
  std::cout << "pass: stderr_capture" << std::endl;
  return 0;
}

/// Timeout handling
TEST(timeout_handling) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", "while True: pass"}, {"timeout", 2}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(!result["success"].get<bool>(), "infinite loop should be killed");
  ASSERT_EQ(result["error"].get<std::string>(), "timeout", "error should be timeout");
  ASSERT(result["elapsed_ms"].get<int>() >= 1000, "should have waited at least 1s");
  std::cout << "pass: timeout_handling" << std::endl;
  return 0;
}

/// elapsed_ms reported
TEST(elapsed_time) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", "print('fast')"}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(result["success"].get<bool>(), "fast script should succeed");
  ASSERT(result["elapsed_ms"].get<int>() > 0, "elapsed_ms should be positive");
  std::cout << "pass: elapsed_time" << std::endl;
  return 0;
}

/// Non-zero exit code
TEST(non_zero_exit) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", "import sys; print('failed'); sys.exit(3)"}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(!result["success"].get<bool>(), "should report failure");
  ASSERT_EQ(result["exit_code"].get<int>(), 3, "exit code should be 3");
  std::string output = result["output"].get<std::string>();
  ASSERT(output.find("failed") != std::string::npos, "should capture stdout before exit");
  std::cout << "pass: non_zero_exit" << std::endl;
  return 0;
}

/// Output truncation (50KB+)
TEST(output_truncation) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  // Generate 55KB of output (exceeds 50KB limit)
  json params = {{"code", "print('x' * 55000)"}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(result["success"].get<bool>(), "script should succeed");
  std::string output = result["output"].get<std::string>();
  // Should be truncated at ~50KB
  ASSERT(output.size() <= 51000, "output should be truncated");
  ASSERT(output.find("[output truncated at 50KB]") != std::string::npos, "should have truncation marker");
  std::cout << "pass: output_truncation (size=" << output.size() << ")" << std::endl;
  return 0;
}

/// os module usage
TEST(os_module) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", "import os; print(os.getcwd())"}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(result["success"].get<bool>(), "os.getcwd should succeed");
  std::string output = result["output"].get<std::string>();
  ASSERT(!output.empty(), "should have some output");
  std::cout << "pass: os_module" << std::endl;
  return 0;
}

/// re module usage
TEST(re_module) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", "import re; m = re.search(r'([0-9]+)', 'abc123def'); print(m.group(1) if m else 'no')"}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(result["success"].get<bool>(), "regex should succeed");
  std::string output = result["output"].get<std::string>();
  ASSERT(output.find("123") != std::string::npos, "should extract '123'");
  std::cout << "pass: re_module" << std::endl;
  return 0;
}

/// timeout clamp: negative → 1
TEST(timeout_clamp_min) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", "print('ok')"}, {"timeout", -5}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(result["success"].get<bool>(), "should succeed with clamped timeout");
  // Should complete quickly despite -5 input (clamped to 1)
  ASSERT(result["elapsed_ms"].get<int>() < 5000, "should not wait excessively long");
  std::cout << "pass: timeout_clamp_min" << std::endl;
  return 0;
}

/// timeout clamp: >120 → 120
TEST(timeout_clamp_max) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", "print('ok')"}, {"timeout", 9999}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(result["success"].get<bool>(), "should succeed with clamped timeout");
  // Should complete quickly (script is fast, timeout 120 is irrelevant)
  ASSERT(result["elapsed_ms"].get<int>() < 5000, "should not wait excessively long");
  std::cout << "pass: timeout_clamp_max" << std::endl;
  return 0;
}

/// Runtime error (ZeroDivisionError)
TEST(runtime_error) {
  if (!g_python3_ok) { std::cout << "SKIP: python3 not available" << std::endl; return 0; }
  json params = {{"code", "print('before'); 1/0; print('after')"}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(!result["success"].get<bool>(), "runtime error should fail");
  ASSERT(result["exit_code"].get<int>() != 0, "exit code should be non-zero");
  std::string output = result["output"].get<std::string>();
  ASSERT(output.find("ZeroDivisionError") != std::string::npos, "should contain ZeroDivisionError");
  ASSERT(output.find("before") != std::string::npos, "should capture stdout before crash");
  std::cout << "pass: runtime_error" << std::endl;
  return 0;
}

// ── v0.53.6 语言类白名单 ──

TEST(node_hello) {
  if (system("node --version > /dev/null 2>&1") != 0) {
    std::cout << "skip: node_hello (node 不可用)" << std::endl;
    return 0;
  }
  json params = {{"code", "console.log('node:' + (21 * 2))"},
                 {"language", "node"}, {"timeout", 15}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(result["success"].get<bool>(), "node exec should succeed");
  std::string output = result["output"].get<std::string>();
  ASSERT(output.find("node:42") != std::string::npos, "node output should contain node:42");
  std::cout << "pass: node_hello" << std::endl;
  return 0;
}

TEST(unsupported_language) {
  json params = {{"code", "print(1)"}, {"language", "cobol"}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(!result["success"].get<bool>(), "unsupported lang should fail");
  ASSERT(result["error"].get<std::string>() == "unsupported_language",
         "error should be unsupported_language");
  ASSERT(result["supported"].get<std::string>().find("python") != std::string::npos,
         "supported list should mention python");
  std::cout << "pass: unsupported_language" << std::endl;
  return 0;
}

TEST(language_default_python) {
  json params = {{"code", "import sys; print(sys.version_info[0])"}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(result["success"].get<bool>(), "default should be python");
  std::string output = result["output"].get<std::string>();
  ASSERT(output.find("3") != std::string::npos, "python3 expected");
  std::cout << "pass: language_default_python" << std::endl;
  return 0;
}

TEST(wordlist_override) {
  // 词表合并语义：注入新语言 ruby（即使环境无 ruby 也能查表）+ 覆盖 ext
  thin_agent::code_exec::InterpreterSpec spec{"ruby", ".rb", {}};
  thin_agent::code_exec::g_interpreters["ruby"] = spec;
  json params = {{"code", "puts 'hi'"}, {"language", "ruby"}, {"timeout", 5}};
  auto result = thin_agent::code_exec::handle_code_exec(params);
  // ruby 未装：execvp 失败 exit 127——但已过白名单（错误不是 unsupported_language）
  std::string err = result.value("error", "");
  bool passed_gate = result.contains("success") &&
                     err != "unsupported_language";
  ASSERT(passed_gate, "ruby should pass whitelist (may fail exec)");
  thin_agent::code_exec::g_interpreters.erase("ruby");
  json again = thin_agent::code_exec::handle_code_exec(params);
  ASSERT(again.value("error", "") == "unsupported_language",
         "erased lang should be rejected");
  std::cout << "pass: wordlist_override" << std::endl;
  return 0;
}

int main() {
  check_python3();

  int fails = 0;
  #define RUN(name) do { \
    std::cout << "test: " #name "..." << std::endl; \
    if (test_##name() != 0) ++fails; \
  } while (0)

  RUN(empty_code);
  RUN(simple_print);
  RUN(json_output);
  RUN(math_computation);
  RUN(syntax_error);
  RUN(stderr_capture);
  RUN(timeout_handling);
  RUN(elapsed_time);
  RUN(non_zero_exit);
  RUN(output_truncation);
  RUN(os_module);
  RUN(re_module);
  RUN(timeout_clamp_min);
  RUN(timeout_clamp_max);
  RUN(runtime_error);
  RUN(node_hello);
  RUN(unsupported_language);
  RUN(language_default_python);
  RUN(wordlist_override);

  std::cout << (fails == 0 ? "ALL PASS" : "SOME FAILED") << " (" << fails << " failures)" << std::endl;
  return fails;
}
