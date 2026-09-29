#pragma once

#include <string>
#include <vector>

namespace thin_agent {

/// SandboxExecutor — 跨平台进程隔离沙箱
///
/// 将 shell_exec 的 popen() 替换为容器级隔离执行：
///   Linux   → clone(CLONE_NEWPID|NEWNS|NEWNET) + cgroups v2
///   macOS   → sandbox-exec profile + setrlimit
///   Windows → CreateJobObject + JOBOBJECT_EXTENDED_LIMITS
///   兜底    → fork+setsid+setrlimit（无 namespace 但有限制）
///
/// 用法：
///   SandboxExecutor::Config cfg;
///   cfg.timeout_ms = 10000;
///   cfg.max_memory_mb = 128;
///   auto result = SandboxExecutor::execute("gcc -o a.out test.c", cfg);
class SandboxExecutor {
 public:
  struct Config {
    /// 总执行超时（毫秒），默认 30s。0 = 不限制。
    int timeout_ms = 30000;

    /// 输出截断字节数，默认 64KB。
    int max_output_bytes = 65536;

    /// 最大内存（MB），默认 256MB。0 = 不限制。
    int max_memory_mb = 256;

    /// 是否允许网络访问。false = 隔离网络。
    bool allow_network = false;

    /// 当前目录外的可写路径（仅 Linux 原生沙箱支持其他平台用 fallback）。
    std::vector<std::string> writable_paths;

    /// 环境变量（继承父进程基础上追加）。空 = 只继承基础变量。
    std::vector<std::string> extra_env;
  };

  struct Result {
    int exit_code = -1;
    std::string output;
    int elapsed_ms = 0;
    bool timed_out = false;
    bool killed_by_oom = false;
    std::string error;  // 空 = 无错误
  };

  /// 执行命令，自动选择平台最佳沙箱。
  static Result execute(const std::string& command, const Config& config);

  /// 当前平台是否支持原生沙箱（有 namespace 或 OS 级隔离）。
  /// false 表示使用 fallback（仅有资源限制，无文件系统/网络隔离）。
  static bool native_sandbox_available();

 private:
  // 平台特定实现（见 .cpp 中的 #ifdef 分支）
  static Result execute_linux(const std::string& command, const Config& config);
  static Result execute_macos(const std::string& command, const Config& config);
  static Result execute_windows(const std::string& command, const Config& config);
  static Result execute_fallback(const std::string& command, const Config& config);
};

}  // namespace thin_agent
