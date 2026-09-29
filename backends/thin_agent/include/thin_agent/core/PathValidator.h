#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {

/// 文件路径安全校验器。
///
/// 两层策略：
/// 1. 核心层黑名单：堵死系统关键路径（/etc, /boot, ~/.ssh, ...），所有模式生效。
/// 2. 项目级白名单：当会话设置了 ProjectContext 时，写操作必须在 project_root 内，
///    且 mode 必须为 rw。读操作在 project_root 内放行。
///
/// 路径规范化策略：
/// - 先绝对化（相对路径拼接 CWD）
/// - 逐段消除 "." 和 ".."
/// - 不调用 realpath()（目标文件可能不存在）
/// - 结果以 "/" 结尾判断前缀（防 /home/user_evil 绕过 /home/user）
class PathValidator {
 public:
  /// 操作类型
  enum class Op { Read, Write };

  /// 项目上下文（会话级，运行时可变）
  struct ProjectContext {
    std::string project_root;  ///< 规范化后的绝对路径（无尾斜杠）
    bool read_only = true;     ///< true=只读, false=读写
    bool valid = false;        ///< 是否已设置

    bool operator==(const ProjectContext& o) const {
      return project_root == o.project_root && read_only == o.read_only && valid == o.valid;
    }
  };

  /// 校验结果
  struct Result {
    bool allowed = false;
    std::string reason;           ///< 拒绝原因（allowed=false 时有意义）
    std::string normalized_path;  ///< 规范化后的路径
  };

  /// 核心层黑名单检查（始终生效，与 ProjectContext 无关）。
  /// 堵死系统关键路径：/etc, /boot, /lib, /bin, /usr, /sbin, /proc, /sys, /dev,
  /// ~/.ssh, /root/.ssh, 以及覆盖已存在二进制文件。
  static bool is_system_path(const std::string& normalized_path);

  /// 核心层完整校验（黑名单）。所有 write_file / code_write_file 调用。
  /// @param path 原始路径（可相对）
  /// @param op   操作类型
  static Result validate_core(const std::string& path, Op op);

  /// 项目级完整校验（黑名单 + 白名单）。用于有 ProjectContext 的场景。
  /// @param path    原始路径
  /// @param op      操作类型
  /// @param ctx     当前会话的项目上下文
  static Result validate_project(const std::string& path, Op op,
                                  const ProjectContext& ctx);

  /// 路径规范化（不调用 realpath）：绝对化 + 消除 ./../ + 去除多余斜杠。
  static std::string normalize(const std::string& path);

  /// 路径规范化（带 base_dir 参数，用于测试）。
  static std::string normalize(const std::string& path, const std::string& base_dir);

  /// 判断 child 是否在 parent 目录内（两者须已规范化）。
  /// "/home/user" 是 "/home/user/project" 的 parent → true
  /// "/home/user_evil" 不是 "/home/user" 的 child → false
  static bool is_within(const std::string& parent, const std::string& child);

  /// 扩展 ~ → $HOME
  static std::string expand_home(const std::string& path);

 private:
  /// 系统关键路径前缀列表（规范化后，带尾斜杠用于精确前缀匹配）
  static const std::vector<std::string>& system_blacklist();

  /// 二进制文件扩展名（覆盖已有二进制 = 危险）
  static const std::vector<std::string>& binary_extensions();
};

// ── thread_local 会话上下文（供 plugin handler 隐式获取）──────────

/// 当前线程的项目上下文。AgentService 在 handle_request 入口统一设置，
/// plugin handler 通过 current_project() 读取。
/// 空时表示无项目上下文（退化到核心层黑名单策略）。
struct ProjectContextTLS {
  PathValidator::ProjectContext ctx;

  static ProjectContextTLS& current();
  static void set(const PathValidator::ProjectContext& ctx);
  static void clear();
};

/// v0.50.3: RAII 守卫 — 构造时设置 TLS 项目上下文，析构时清理。
/// 消除跨请求 TLS 残留（worker 线程复用时旧会话上下文泄漏到下一请求）
/// 与异常路径不清理两个缺陷。handle_request 入口统一使用。
class ProjectContextGuard {
 public:
  explicit ProjectContextGuard(const PathValidator::ProjectContext& ctx) {
    ProjectContextTLS::set(ctx);
    active_ = true;
  }
  explicit ProjectContextGuard(std::nullptr_t) {
    // 无项目上下文的会话：显式清空（防残留），无需 active 标记
    ProjectContextTLS::clear();
  }
  ~ProjectContextGuard() {
    if (active_) ProjectContextTLS::clear();
  }
  ProjectContextGuard(const ProjectContextGuard&) = delete;
  ProjectContextGuard& operator=(const ProjectContextGuard&) = delete;

 private:
  bool active_ = false;
};

// ── 统一安全文件操作（消除三处 write_file / 两处 read_file 重复）──────

/// safe_write_file 的行为选项
struct WriteFileOptions {
  bool create_parent = true;   ///< 自动创建父目录
  bool verify = true;          ///< 写入后验证回读字节数
  bool syntax_check = false;   ///< 写入后做语法检查（code_dev 场景）
};

/// safe_write_file 的返回值
struct WriteFileResult {
  bool success = false;
  std::string error;           ///< 失败原因
  std::string normalized_path; ///< 规范化后的路径
  size_t bytes_written = 0;    ///< 实际写入字节数
  std::string syntax_error;    ///< 语法检查结果（syntax_check=true 时）
};

/// 统一安全写入：路径校验（TLS上下文）→ 建父目录 → 写入 → 验证 → 语法检查。
/// 三处 write_file（execute_one_tool / register_cpp_handlers / code_dev）共用。
WriteFileResult safe_write_file(const std::string& path,
                                const std::string& content,
                                WriteFileOptions opts = {});

/// safe_read_file 的返回值
struct ReadFileResult {
  bool success = false;
  std::string error;
  std::string normalized_path;
  std::string content;
};

/// 统一安全读取：路径校验（TLS上下文）→ 读取文件全部内容。
ReadFileResult safe_read_file(const std::string& path);

/// 统一安全读取（带行号分页，用于 code_read_file）。
/// offset 从 1 开始，limit 上限 2000。
struct ReadFilePagedResult {
  bool success = false;
  std::string error;
  std::string normalized_path;
  std::string output;          ///< 带行号的文本（N|content\n）
  int total_lines = 0;
  int start_line = 0;
  int end_line = 0;
};
ReadFilePagedResult safe_read_file_paged(const std::string& path,
                                         int offset = 1, int limit = 500);

/// 统一安全写入（patch 模式）：路径校验 → 读取 → 模糊匹配替换 → 写回 → 语法检查。
/// 用于 code_dev code_patch，避免与 write_file 重复校验逻辑。
struct PatchFileResult {
  bool success = false;
  std::string error;
  std::string normalized_path;
  std::string output;          ///< 操作摘要
  int replacements = 0;
  std::string strategy;
  std::string syntax_error;
};
PatchFileResult safe_patch_file(const std::string& path,
                                const std::string& old_str,
                                const std::string& new_str,
                                bool replace_all = false);

// ── v0.49.0: 统一风险分级框架（FC 循环 + AgentLoop 共用）─────────

/// 工具风险等级
enum class ToolRisk {
  Safe,       ///< 只读/无副作用，直接执行
  Mutating,   ///< 有写副作用，但被 PathValidator/白名单约束住
  Dangerous,  ///< 需要 HITL 人工确认（shell 执行、删除等不可逆操作）
};

/// 工具风险分级（内置表 + ProjectContext 启发 + 默认 Mutating）。
/// 三层合一：静态工具表 + 会话级项目模式（TLS）+ 默认兜底。
/// FC 循环（execute_one_tool 前）与 AgentLoop（dangerous 检查）共用。
ToolRisk tool_risk_of(const std::string& name,
                      const nlohmann::json& args = nlohmann::json::object());

/// v0.52.0: shell 命令参数级风险分级（default-deny）。
/// deny 子串命中（sudo/curl/$() 等）→ Dangerous；多命令拼接逐段判首词，
/// 任一段非构建工具链白名单（编译器/解释器/测试运行器，多语言）→
/// Dangerous；全段白名单 → Mutating（项目内免审批）。词表外置
/// chat_policy.json risk_grading.shell_exec（代码默认值兜底）。
/// v0.52.7: shell 参数安全转义（单引号包裹法）——git/workflow/MCP
/// 等外插参数拼命令前必须过此（POSIX 唯一安全转义，无 eval 语义）。
struct ShellArg {
  static std::string escape(const std::string& raw);
};

ToolRisk grade_shell_risk(const std::string& command);

/// 风险等级转字符串（日志/审计用）
const char* tool_risk_str(ToolRisk r);

}  // namespace thin_agent
