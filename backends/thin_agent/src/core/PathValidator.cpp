#include "thin_agent/core/PathValidator.h"
#include "thin_agent/core/ChatPolicy.h"
#include "thin_agent/core/FuzzyPatcher.h"
#include "thin_agent/core/SyntaxChecker.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace thin_agent {

/// v0.52.7: POSIX shell 参数安全转义——单引号包裹+内部单引号转义
///（'\''），唯一能保证任意字符串作为单一字面量传递的转义法。
/// git/workflow/MCP 等外插参数拼接 shell 命令前必须过此函数。
std::string ShellArg::escape(const std::string& raw) {
  std::string out = "'";
  for (char c : raw) {
    if (c == '\'') {
      out += "'\\''";  // ' → '\''（关闭引号+转义单引号+重开引号）
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}


// ═══════════════════════════════════════════════════
// 系统黑名单 & 二进制扩展名
// ═══════════════════════════════════════════════════

const std::vector<std::string>& PathValidator::system_blacklist() {
  static const std::vector<std::string> blacklist = {
    "/etc/",
    "/boot/",
    "/lib/",
    "/lib64/",
    "/bin/",
    "/sbin/",
    "/usr/",
    "/proc/",
    "/sys/",
    "/dev/",
    "/run/",
    "/snap/",
    "/root/.ssh/",
  };
  return blacklist;
}

const std::vector<std::string>& PathValidator::binary_extensions() {
  static const std::vector<std::string> exts = {
    ".so", ".o", ".a", ".dylib", ".dll", ".exe",
  };
  return exts;
}

// ═══════════════════════════════════════════════════
// 路径规范化
// ═══════════════════════════════════════════════════

std::string PathValidator::expand_home(const std::string& path) {
  if (path.empty() || path[0] != '~') return path;
  const char* home = std::getenv("HOME");
  if (!home) home = std::getenv("USERPROFILE");
  if (!home) return path;
  std::string result = home;
  if (path.size() > 1) result += path.substr(1);  // ~ → HOME, ~/x → HOME/x
  return result;
}

std::string PathValidator::normalize(const std::string& path) {
  try {
    return normalize(path, std::filesystem::current_path().string());
  } catch (...) {
    return normalize(path, "/");
  }
}

std::string PathValidator::normalize(const std::string& path, const std::string& base_dir) {
  if (path.empty()) return base_dir;

  std::string expanded = expand_home(path);

  // 绝对化
  std::string abs_path;
  if (!expanded.empty() && expanded[0] == '/') {
    abs_path = expanded;
  } else {
    // 拼接 base_dir
    if (base_dir.empty() || base_dir.back() == '/') {
      abs_path = base_dir + expanded;
    } else {
      abs_path = base_dir + "/" + expanded;
    }
  }

  // 逐段消除 . 和 ..
  std::vector<std::string> segments;
  std::istringstream iss(abs_path);
  std::string seg;
  while (std::getline(iss, seg, '/')) {
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") {
      if (!segments.empty()) segments.pop_back();
      continue;
    }
    segments.push_back(seg);
  }

  std::string result = "/";
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i > 0) result += "/";
    result += segments[i];
  }
  // 不保留尾斜杠（is_within 内部自行处理）
  return result;
}

// ═══════════════════════════════════════════════════
// 目录包含关系判断
// ═══════════════════════════════════════════════════

bool PathValidator::is_within(const std::string& parent, const std::string& child) {
  if (parent.empty() || child.empty()) return false;
  // 确保 parent 带尾斜杠
  std::string p = parent;
  if (p.back() != '/') p += '/';
  // child 也需要带尾斜杠才能正确比较
  std::string c = child;
  if (c.back() != '/') c += '/';
  // 子路径必须以 parent/ 开头
  if (c.size() <= p.size()) {
    // child == parent（相同目录）
    return c == p;
  }
  return c.substr(0, p.size()) == p;
}

// ═══════════════════════════════════════════════════
// 系统路径检测
// ═══════════════════════════════════════════════════

bool PathValidator::is_system_path(const std::string& normalized_path) {
  // 确保 path 带尾斜杠进行前缀比较
  std::string p = normalized_path;
  if (p.back() != '/') p += '/';

  for (const auto& blocked : system_blacklist()) {
    // blocked 已带尾斜杠，如 "/etc/"
    if (p.size() >= blocked.size() && p.substr(0, blocked.size()) == blocked) {
      return true;
    }
    // 精确匹配（如 path = "/etc"）
    std::string blocked_no_slash = blocked;
    if (!blocked_no_slash.empty() && blocked_no_slash.back() == '/') {
      blocked_no_slash.pop_back();
    }
    if (normalized_path == blocked_no_slash) return true;
  }

  // ~/.ssh 检查（动态 home 目录）
  const char* home = std::getenv("HOME");
  if (home) {
    std::string ssh_dir = std::string(home) + "/.ssh";
    if (normalized_path == ssh_dir) return true;
    std::string ssh_dir_slash = ssh_dir + "/";
    if (p.substr(0, ssh_dir_slash.size()) == ssh_dir_slash) return true;
  }

  return false;
}

// ═══════════════════════════════════════════════════
// 核心层校验（黑名单）
// ═══════════════════════════════════════════════════

PathValidator::Result PathValidator::validate_core(const std::string& path, Op op) {
  Result r;
  r.normalized_path = normalize(path);

  // 写操作：检查系统路径
  if (op == Op::Write) {
    if (is_system_path(r.normalized_path)) {
      r.allowed = false;
      r.reason = "blocked_system_path: " + r.normalized_path;
      return r;
    }

    // 覆盖已有二进制文件
    std::error_code ec;
    if (std::filesystem::exists(r.normalized_path, ec)) {
      std::string lower = r.normalized_path;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      for (const auto& ext : binary_extensions()) {
        if (lower.size() >= ext.size() &&
            lower.substr(lower.size() - ext.size()) == ext) {
          r.allowed = false;
          r.reason = "blocked_binary_overwrite: " + r.normalized_path;
          return r;
        }
      }
    }
  }

  r.allowed = true;
  return r;
}

// ═══════════════════════════════════════════════════
// 项目级校验（黑名单 + 白名单）
// ═══════════════════════════════════════════════════

PathValidator::Result PathValidator::validate_project(const std::string& path, Op op,
                                                       const ProjectContext& ctx) {
  Result r;
  r.normalized_path = normalize(path);

  // 1. 先过核心黑名单（始终生效）
  if (op == Op::Write && is_system_path(r.normalized_path)) {
    r.allowed = false;
    r.reason = "blocked_system_path: " + r.normalized_path;
    return r;
  }

  // 2. 无项目上下文 → 退化到核心策略
  if (!ctx.valid || ctx.project_root.empty()) {
    if (op == Op::Write) {
      // 覆盖已有二进制
      std::error_code ec;
      if (std::filesystem::exists(r.normalized_path, ec)) {
        std::string lower = r.normalized_path;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const auto& ext : binary_extensions()) {
          if (lower.size() >= ext.size() &&
              lower.substr(lower.size() - ext.size()) == ext) {
            r.allowed = false;
            r.reason = "blocked_binary_overwrite: " + r.normalized_path;
            return r;
          }
        }
      }
    }
    r.allowed = true;
    return r;
  }

  // 3. 有项目上下文 → 白名单模式
  bool in_project = is_within(ctx.project_root, r.normalized_path);

  if (in_project) {
    // 在项目目录内
    if (op == Op::Write && ctx.read_only) {
      r.allowed = false;
      r.reason = "project_read_only: " + r.normalized_path +
                 " (project=" + ctx.project_root + ")";
      return r;
    }
    r.allowed = true;
    return r;
  }

  // 在项目目录外
  if (op == Op::Write) {
    r.allowed = false;
    r.reason = "outside_project_root: " + r.normalized_path +
               " (project=" + ctx.project_root + ")";
    return r;
  }

  // 读操作：项目外允许（核心黑名单已过）
  r.allowed = true;
  return r;
}

// ═══════════════════════════════════════════════════
// thread_local 会话上下文
// ═══════════════════════════════════════════════════

ProjectContextTLS& ProjectContextTLS::current() {
  thread_local ProjectContextTLS instance;
  return instance;
}

void ProjectContextTLS::set(const PathValidator::ProjectContext& ctx) {
  current().ctx = ctx;
}

void ProjectContextTLS::clear() {
  current().ctx = {};
}

// ═══════════════════════════════════════════════════
// 统一安全文件操作
// ═══════════════════════════════════════════════════

namespace {

/// 内部：用当前 TLS 上下文做写路径校验
PathValidator::Result validate_write_with_tls(const std::string& path) {
  auto& tls = ProjectContextTLS::current();
  if (tls.ctx.valid) {
    return PathValidator::validate_project(path, PathValidator::Op::Write, tls.ctx);
  }
  return PathValidator::validate_core(path, PathValidator::Op::Write);
}

/// 内部：用当前 TLS 上下文做读路径校验
PathValidator::Result validate_read_with_tls(const std::string& path) {
  auto& tls = ProjectContextTLS::current();
  if (tls.ctx.valid) {
    return PathValidator::validate_project(path, PathValidator::Op::Read, tls.ctx);
  }
  return PathValidator::validate_core(path, PathValidator::Op::Read);
}

}  // anonymous namespace

WriteFileResult safe_write_file(const std::string& path,
                                const std::string& content,
                                WriteFileOptions opts) {
  WriteFileResult r;

  // 1. 路径校验
  auto vr = validate_write_with_tls(path);
  if (!vr.allowed) {
    r.error = vr.reason;
    return r;
  }
  r.normalized_path = vr.normalized_path;

  // 2. 建父目录
  if (opts.create_parent) {
    std::error_code ec;
    auto parent = std::filesystem::path(r.normalized_path).parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
      std::filesystem::create_directories(parent, ec);
      // create_directories 失败不立即返回——ofstream 会报错
    }
  }

  // 3. 写入
  {
    std::ofstream out(r.normalized_path);
    if (!out.is_open()) {
      r.error = "cannot_open:" + path;
      return r;
    }
    out << content;
    out.close();
  }

  // 4. 验证回读
  if (opts.verify) {
    std::ifstream verify(r.normalized_path, std::ios::binary | std::ios::ate);
    if (verify.is_open()) {
      r.bytes_written = static_cast<size_t>(verify.tellg());
    } else {
      r.bytes_written = content.size();
    }
  } else {
    r.bytes_written = content.size();
  }

  // 5. 语法检查
  if (opts.syntax_check) {
    r.syntax_error = syntax_check(r.normalized_path, content);
  }

  r.success = true;
  return r;
}

ReadFileResult safe_read_file(const std::string& path) {
  ReadFileResult r;

  auto vr = validate_read_with_tls(path);
  if (!vr.allowed) {
    r.error = vr.reason;
    return r;
  }
  r.normalized_path = vr.normalized_path;

  std::ifstream in(r.normalized_path);
  if (!in.is_open()) {
    r.error = "cannot_open:" + path;
    return r;
  }
  r.content.assign((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  r.success = true;
  return r;
}

ReadFilePagedResult safe_read_file_paged(const std::string& path,
                                         int offset, int limit) {
  ReadFilePagedResult r;

  if (offset < 1) offset = 1;
  if (limit < 1) limit = 1;
  if (limit > 2000) limit = 2000;

  auto vr = validate_read_with_tls(path);
  if (!vr.allowed) {
    r.error = vr.reason;
    return r;
  }
  r.normalized_path = vr.normalized_path;

  std::ifstream in(r.normalized_path);
  if (!in.is_open()) {
    r.error = "cannot_open:" + path;
    return r;
  }

  std::string line;
  int line_no = 0;
  int total_lines = 0;
  std::ostringstream out;
  int start = offset;
  int end = offset + limit - 1;

  while (std::getline(in, line)) {
    ++total_lines;
    if (total_lines >= start && total_lines <= end) {
      out << total_lines << "|" << line << "\n";
    }
    if (total_lines > end) break;
  }

  r.output = out.str();
  r.total_lines = total_lines;
  r.start_line = std::min(start, total_lines);
  r.end_line = std::min(end, total_lines);
  r.success = true;
  return r;
}

PatchFileResult safe_patch_file(const std::string& path,
                                const std::string& old_str,
                                const std::string& new_str,
                                bool replace_all) {
  PatchFileResult r;

  auto vr = validate_write_with_tls(path);
  if (!vr.allowed) {
    r.error = vr.reason;
    return r;
  }
  r.normalized_path = vr.normalized_path;

  // 读取
  std::ifstream in(r.normalized_path);
  if (!in.is_open()) {
    r.error = "cannot_open:" + path;
    return r;
  }
  std::string content((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  in.close();

  // 模糊匹配替换（patch 原地修改 content）
  static const FuzzyPatcher patcher;
  auto result = patcher.patch(content, old_str, new_str, replace_all);
  if (!result.success) {
    r.error = result.output;
    return r;
  }

  // 写回
  std::ofstream out(r.normalized_path);
  if (!out.is_open()) {
    r.error = "cannot_write:" + path;
    return r;
  }
  out << content;
  out.close();

  r.output = result.output + " in " + r.normalized_path;
  r.replacements = result.replacements;
  r.strategy = result.strategy;
  r.syntax_error = syntax_check(r.normalized_path, content);
  r.success = true;
  return r;
}

// ═══════════════════════════════════════════════════
// v0.49.0: 统一风险分级框架
// ═══════════════════════════════════════════════════

namespace {

/// v0.52.0: 首词提取——取第一个空白前的词并去掉路径前缀。
/// "/usr/bin/g++ -Wall" → "g++"；"cd /x && g++" 段内 "cd"。
std::string first_word_of(const std::string& segment) {
  size_t start = segment.find_first_not_of(" \t\n");
  if (start == std::string::npos) return "";
  size_t end = segment.find_first_of(" \t\n", start);
  std::string w = segment.substr(start, end == std::string::npos
                                         ? std::string::npos : end - start);
  const size_t slash = w.rfind('/');
  if (slash != std::string::npos) w = w.substr(slash + 1);
  return w;
}

/// v0.52.0: deny 子串表（chat_policy risk_grading.shell_exec.deny_substrings，
/// 代码内默认兜底）。命中即 Dangerous——不可逆/外传/命令替换/提权。
std::vector<std::string> risk_deny_substrings() {
  auto v = policy_string_list("risk_grading.shell_exec.deny_substrings");
  if (!v.empty()) return v;
  return {
      "$(", "${", "sudo", "su ", "rm -rf /", "rm -rf ~", "dd ", "chmod",
      "chown", "mkfs", "mount", "umount", "curl", "wget", "nc ", "ncat",
      "ssh", "scp", "sftp", "ftp", "telnet", "git push", "git reset",
      "git clean", "kill", "killall", "pkill", "shutdown", "reboot",
      "poweroff", "halt", "systemctl", "service", "crontab", "iptables",
      "nft", "setenforce", "nsenter", "chroot",
  };
}

/// v0.52.0: allow 首词表——"编译器/解释器/测试运行器"这个类（多语言，
/// 非 C/C++ 特例；chat_policy 可覆盖，默认值兜底）。
std::vector<std::string> risk_allow_first_words() {
  auto v = policy_string_list("risk_grading.shell_exec.allow_first_words");
  if (!v.empty()) return v;
  return {
      // C/C++
      "g++", "gcc", "cc", "c++", "make", "cmake", "ctest", "ninja", "clang",
      "clang++", "gdb",
      // Python
      "python3", "python", "pytest", "pip", "pip3", "uv", "poetry",
      // JS/TS
      "node", "npm", "npx", "yarn", "pnpm", "bun", "deno", "tsc", "jest",
      "vitest", "mocha",
      // Java/Kotlin
      "java", "javac", "mvn", "gradle", "kotlin", "kotlinc",
      // Go / Rust / 其它静态语言
      "go", "rustc", "cargo", "dotnet", "swiftc", "zig",
      // 脚本语言
      "ruby", "gem", "php", "perl", "lua", "Rscript", "bash", "sh",
      // 查看与文件操作（既有 whitelist 同源）
      "ls", "cat", "grep", "find", "head", "tail", "wc", "mkdir", "cp",
      "mv", "echo", "file", "du", "ps", "uname", "sleep", "date", "time",
      "pwd", "whoami", "hostname", "uptime", "env", "which", "stat",
      "touch", "test", "true", "false", "basename", "dirname", "sort",
      "uniq", "cut", "tr", "diff", "tee", "seq", "md5sum", "sha256sum",
      "realpath", "readlink", "sed", "awk", "tar", "gzip", "gunzip",
      "jq", "timeout", "xargs", "watch", "git",
  };
}

}  // namespace

/// v0.52.0: shell 命令风险分级（default-deny，规则见 tool_risk_of 注释）。
ToolRisk grade_shell_risk(const std::string& command) {
  if (command.empty()) return ToolRisk::Dangerous;  // 空命令无意义，保守拦
  // 引号内的内容同样参与判定（保守：混入即拦）
  const auto deny = risk_deny_substrings();
  for (const auto& d : deny) {
    if (command.find(d) != std::string::npos) return ToolRisk::Dangerous;
  }

  // 多命令拼接：&& || ; | 分段（含引号内分隔符的保守近似——分段过宽
  // 只会让某些段首词非白名单从而升级 Dangerous，不会放过危险命令）
  std::vector<std::string> segments;
  std::string cur;
  for (size_t i = 0; i < command.size(); ++i) {
    const char c = command[i];
    if (c == '&' && i + 1 < command.size() && command[i + 1] == '&') {
      segments.push_back(cur); cur.clear(); ++i;
    } else if (c == '|' && i + 1 < command.size() && command[i + 1] == '|') {
      segments.push_back(cur); cur.clear(); ++i;
    } else if (c == ';' || c == '|') {
      segments.push_back(cur); cur.clear();
    } else {
      cur += c;
    }
  }
  segments.push_back(cur);

  const auto allow = risk_allow_first_words();
  const std::unordered_set<std::string> allow_set(allow.begin(), allow.end());
  for (const auto& seg : segments) {
    const std::string fw = first_word_of(seg);
    if (fw.empty()) continue;
    if (fw == "cd") continue;  // 目录切换本身无副作用，看下一段
    if (!allow_set.count(fw)) return ToolRisk::Dangerous;
  }
  return ToolRisk::Mutating;
}

ToolRisk tool_risk_of(const std::string& name, const nlohmann::json& args) {
  // ═══ v0.52.0: shell_exec 参数级风险分级（default-deny）═══
  // 编程 agent 的核心高频操作（编译/测试/运行解释器）与真正危险命令
  // 同级导致审批疲劳（实测一个编程任务批 5+ 次）。分级规则：
  //   1. deny 子串命中（sudo/curl/$(... 等不可逆/外传/命令替换）→ Dangerous
  //   2. 多命令拼接（&&/||/;/|）逐段判定，任一段非 allow → Dangerous
  //   3. 单命令首词 ∈ 构建工具链白名单（编译器/解释器/测试运行器，
  //      多语言：C/C++/Python/JS/Java/Go/Rust/...）→ Mutating 免审批
  //   4. 其余（含 ./产物运行——不可静态分析）→ Dangerous
  // 词表外置 chat_policy.json risk_grading（代码内默认值兜底）。
  // 沙箱兜底：shell_exec 本就在隔离沙箱（默认 CLONE_NEWNET 断网 +
  // seccomp deny network），免审批不降低实际安全边界。
  if (name == "shell_exec" || name == "execute_code") {
    const std::string cmd = args.value("command", std::string());
    if (cmd.empty()) return ToolRisk::Dangerous;
    return grade_shell_risk(cmd);
  }
  //（args 用于 git_checkout 分支判定等参数级启发）

  // 只读工具 → Safe
  static const std::unordered_set<std::string> kSafeTools = {
      "list_dir", "read_file", "search_code", "code_read_file", "code_search",
      "kb_search", "kb_status", "status", "memory_recent", "memory_search",
      "memory_history", "event_recent", "metrics", "usage_stats", "get_project",
      "skill_list", "skill_stats", "task_list", "task_get", "task_audit",
      "checkpoint_list", "cron_list", "cron_stats", "correction_stats",
      "bb_read", "bb_keys", "kanban_status", "agent_inbox", "monitor_status",
      "trace_list", "cache_stats", "project_info", "hello", "ping",
  };
  if (kSafeTools.count(name)) return ToolRisk::Safe;

  // 危险工具 → Dangerous（不可逆/任意代码执行/系统级操作）
  // v0.52.7: 补 code_exec（任意 Python，与 execute_code 同类）；
  // git_checkout 恢复文件模式 = 丢弃未提交改动（与 reset/clean 同级
  // 不可逆）——按参数分支判定
  static const std::unordered_set<std::string> kDangerousTools = {
      "shell_exec",            // 任意 shell（即使白名单约束，仍需确认）
      "execute_code",          // 任意 Python 代码
      "code_exec",             // v0.52.7: 任意 Python（code_exec 插件）
      "git_reset", "git_clean", "git_push",  // 不可逆 git 操作
      "delete_file", "rm",
      "task_cancel",           // 取消他人任务
  };
  if (kDangerousTools.count(name)) return ToolRisk::Dangerous;
  // v0.52.7: git_checkout 恢复文件（branch=false）丢弃工作区改动——
  // 不可逆，Dangerous；切分支（branch=true）保留改动，Mutating
  if (name == "git_checkout") {
    const bool is_branch = args.value("branch", false);
    if (!is_branch) return ToolRisk::Dangerous;
    return ToolRisk::Mutating;
  }

  // 写文件族 → 会话启发：项目 rw 模式内 = Mutating（白名单约束）；
  // 无项目上下文 = Dangerous（核心黑名单是唯一防线，升级为需确认）
  if (name == "write_file" || name == "code_write_file" || name == "code_patch") {
    auto& tls = ProjectContextTLS::current();
    if (tls.ctx.valid && !tls.ctx.read_only) return ToolRisk::Mutating;
    return ToolRisk::Dangerous;
  }

  // 其余（capture/git_commit/workflow 等）→ Mutating
  return ToolRisk::Mutating;
}

const char* tool_risk_str(ToolRisk r) {
  switch (r) {
    case ToolRisk::Safe: return "safe";
    case ToolRisk::Mutating: return "mutating";
    case ToolRisk::Dangerous: return "dangerous";
  }
  return "unknown";
}

}  // namespace thin_agent
