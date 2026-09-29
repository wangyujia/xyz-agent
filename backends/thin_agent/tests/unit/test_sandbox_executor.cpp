/// test_sandbox_executor.cpp — SandboxExecutor 单元测试
#include <cassert>
#include <iostream>
#include <string>

#include "thin_agent/core/SandboxExecutor.h"

using namespace thin_agent;

// ── 基础执行测试 ──

static void test_fallback_echo() {
  SandboxExecutor::Config cfg;
  cfg.timeout_ms = 5000;
  cfg.max_output_bytes = 4096;
  cfg.allow_network = true;  // echo 不需要网络但 Fallback 不隔离网络

  auto result = SandboxExecutor::execute("echo hello_sandbox", cfg);
  assert(result.exit_code == 0);
  assert(result.error.empty());
  assert(result.output.find("hello_sandbox") != std::string::npos);
  assert(!result.timed_out);
  std::cout << "PASS: test_fallback_echo (output: " << result.output.substr(0, 30) << ")\n";
}

static void test_fallback_exit_code() {
  SandboxExecutor::Config cfg;
  cfg.timeout_ms = 5000;
  cfg.allow_network = true;

  auto result = SandboxExecutor::execute("exit 42", cfg);
  assert(result.exit_code == 42);
  assert(result.error.empty());
  std::cout << "PASS: test_fallback_exit_code\n";
}

static void test_fallback_stderr() {
  SandboxExecutor::Config cfg;
  cfg.timeout_ms = 5000;
  cfg.allow_network = true;

  // stderr 应该也被捕获到 output 中
  auto result = SandboxExecutor::execute("echo stdout_text && echo stderr_text >&2", cfg);
  assert(result.exit_code == 0);
  assert(result.output.find("stdout_text") != std::string::npos);
  assert(result.output.find("stderr_text") != std::string::npos);
  std::cout << "PASS: test_fallback_stderr\n";
}

static void test_timeout() {
  SandboxExecutor::Config cfg;
  cfg.timeout_ms = 500;
  cfg.allow_network = true;

  auto result = SandboxExecutor::execute("sleep 10", cfg);
  assert(result.timed_out);
  assert(result.output.find("timeout") != std::string::npos);
  std::cout << "PASS: test_timeout\n";
}

// v0.45.8: 嵌套派生超时 — 子进程再起后台死循环（模型 shell 里 bash
// monitor.sh 场景）。修复前：无 setpgid → kill(-pid) 失败 → waitpid
// 永久阻塞（worker 卡死）。修复后：setpgid(0,0) 使子进程成为进程组
// 组长，超时能杀整个进程组。
static void test_timeout_nested_child() {
  SandboxExecutor::Config cfg;
  cfg.timeout_ms = 800;
  cfg.allow_network = true;

  // bash 内再起 while 死循环（会派生孙进程）
  auto result = SandboxExecutor::execute(
      "bash -c 'while true; do sleep 1; done'", cfg);
  assert(result.timed_out);
  assert(result.elapsed_ms < 5000);  // 不被卡死（5s 上限）
  std::cout << "PASS: test_timeout_nested_child (elapsed="
            << result.elapsed_ms << "ms)\n";
}

static void test_output_truncation() {
  SandboxExecutor::Config cfg;
  cfg.timeout_ms = 5000;
  cfg.max_output_bytes = 100;
  cfg.allow_network = true;

  auto result = SandboxExecutor::execute(
      "python3 -c \"print('x' * 10000)\" 2>/dev/null || "
      "yes x 2>/dev/null | head -c 10000", cfg);
  assert(result.output.size() <= 130);  // 100 + "...[truncated]\n"
  assert(result.output.find("truncated") != std::string::npos);
  std::cout << "PASS: test_output_truncation\n";
}

static void test_fork_bomb_separate_namespace() {
  // 验证沙箱内的 fork 不会影响父进程
  SandboxExecutor::Config cfg;
  cfg.timeout_ms = 1000;
  cfg.allow_network = true;

  // 在沙箱里 fork 两个短命子进程，确保主进程不受影响
  auto result = SandboxExecutor::execute(
      "(sleep 0.1 & sleep 0.1 & wait) && echo fork_ok", cfg);
  assert(result.exit_code == 0);
  assert(result.output.find("fork_ok") != std::string::npos);
  std::cout << "PASS: test_fork_bomb_separate_namespace\n";
}

static void test_native_availability() {
  bool available = SandboxExecutor::native_sandbox_available();
  // 在 Linux 上应为 true，其他平台取决于实现
  std::cout << "PASS: test_native_availability (native=" << available << ")\n";
}

static void test_large_output() {
  SandboxExecutor::Config cfg;
  cfg.timeout_ms = 5000;
  cfg.max_output_bytes = 65536;
  cfg.allow_network = true;

  auto result = SandboxExecutor::execute("seq 1 1000", cfg);
  assert(result.exit_code == 0);
  assert(result.output.find("1000") != std::string::npos);
  assert(result.output.find("1") != std::string::npos);
  std::cout << "PASS: test_large_output\n";
}

int main() {
  std::cout << "=== SandboxExecutor Tests ===\n";

  test_native_availability();
  test_fallback_echo();
  test_fallback_exit_code();
  test_fallback_stderr();
  test_timeout();
  test_timeout_nested_child();
  test_output_truncation();
  test_large_output();
  test_fork_bomb_separate_namespace();

  std::cout << "\nALL TESTS PASSED\n";
  return 0;
}
