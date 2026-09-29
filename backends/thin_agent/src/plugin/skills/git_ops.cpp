/// libskill_git.so — Git 操作插件
///
/// 注册的 handler:
///   git_status — 返回工作区状态（staged / unstaged / untracked）
///   git_diff   — 返回 unified diff
///   git_log    — 返回最近 N 次提交
///
/// 仅在 --dev 模式下加载。所有 handler 返回统一 JSON 格式:
///   { "success": true/false, "output": "...", "error": "..." }

#include "thin_agent/plugin/PluginInterface.h"

#include "thin_agent/core/PathValidator.h"

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace thin_agent {
namespace git_ops {

using json = nlohmann::json;

/// 执行 shell 命令并捕获 stdout
static std::string shell_capture(const std::string& cmd) {
  std::string result;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(
      ::popen(cmd.c_str(), "r"), pclose);
  if (!pipe) return "";
  char buf[4096];
  while (::fgets(buf, sizeof(buf), pipe.get())) {
    result += buf;
  }
  return result;
}

// v0.54.26: 带**退出码**的捕获版本。判定成败只看退出码，**不得扫描输出里的 "fatal:"/"error:"** ——
// `git show`/`git log`/`git diff` 的输出包含 **diff 正文与提交信息**，其中的字面量会让正常结果被
// 判成失败。实测根因：本仓 v0.54.24 的提交 diff 里恰有一行 `TA_LOG_LINE("fatal: " << e.what());`
// ⇒ `git show HEAD` 被判 commit_not_found，unit_git_ops.show_head 确定性变红。
// 注：pclose 返回子进程状态（0=成功）；在 Windows 上 _pclose 返回的即退出码，语义一致。
static std::string shell_capture_status(const std::string& cmd, int* exit_code) {
  std::string result;
  FILE* p = ::popen(cmd.c_str(), "r");
  if (p == nullptr) {
    if (exit_code != nullptr) *exit_code = -1;
    return result;
  }
  char buf[4096];
  while (::fgets(buf, sizeof(buf), p)) {
    result += buf;
  }
  const int st = ::pclose(p);
  if (exit_code != nullptr) *exit_code = st;
  return result;
}

// ── Handler: git_status ──
// 返回工作区状态：staged / unstaged / untracked 文件列表
    json handle_git_status(const json& params) {
  (void)params;
  std::string out = shell_capture("git status --porcelain 2>&1");
  if (out.empty()) {
    return {
      {"success", true},
      {"output", "Working tree clean"},
      {"staged", json::array()},
      {"unstaged", json::array()},
      {"untracked", json::array()}
    };
  }

  json staged = json::array();
  json unstaged = json::array();
  json untracked = json::array();

  // --porcelain: XY filename  (X=staging, Y=worktree)
  // ?? = untracked
  std::istringstream iss(out);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.size() < 4) continue;  // v0.52.7: 短行防护（porcelain 行恒 ≥4）
    std::string status = line.substr(0, 2);
    std::string file = line.substr(3);
    // trim trailing whitespace
    while (!file.empty() && (file.back() == ' ' || file.back() == '\r'))
      file.pop_back();

    if (status == "??") {
      untracked.push_back(file);
    } else {
      char x = status[0];  // staging area
      char y = status[1];  // worktree
      if (x != ' ' && x != '?') {
        staged.push_back({{"file", file}, {"status", std::string(1, x)}});
      }
      if (y != ' ' && y != '?') {
        unstaged.push_back({{"file", file}, {"status", std::string(1, y)}});
      }
    }
  }

  int total = staged.size() + unstaged.size() + untracked.size();
  std::string summary = std::to_string(total) + " changes";
  if (staged.size() > 0) summary += ", " + std::to_string(staged.size()) + " staged";
  if (unstaged.size() > 0) summary += ", " + std::to_string(unstaged.size()) + " unstaged";
  if (untracked.size() > 0) summary += ", " + std::to_string(untracked.size()) + " untracked";

  return {
    {"success", true},
    {"output", summary},
    {"staged", staged},
    {"unstaged", unstaged},
    {"untracked", untracked}
  };
}

// ── Handler: git_diff ──
// 返回 unified diff。参数: staged(默认false) / file(可选)
    json handle_git_diff(const json& params) {
  bool staged = params.value("staged", false);
  std::string file = params.value("file", "");

  std::string cmd = "git diff";
  if (staged) cmd += " --staged";
  if (!file.empty()) cmd += " -- " + ShellArg::escape(file);  // v0.52.7: 注入防护
  cmd += " 2>&1";

  std::string out = shell_capture(cmd);
  if (out.empty()) {
    return {{"success", true}, {"output", "(no changes)"}, {"diff", ""}};
  }

  return {
    {"success", true},
    {"output", out},
    {"diff", out}
  };
}

// ── Handler: git_log ──
// 返回最近 N 次提交。参数: n(默认10)
    json handle_git_log(const json& params) {
  int n = params.value("n", 10);
  if (n < 1) n = 1;
  if (n > 100) n = 100;

  std::string cmd = "git log --oneline -" + std::to_string(n) + " 2>&1";
  std::string out = shell_capture(cmd);

  json commits = json::array();
  std::istringstream iss(out);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.empty()) continue;
    // 7-char hash + space + message
    if (line.size() >= 8) {
      std::string hash = line.substr(0, 7);
      std::string msg = line.substr(8);
      commits.push_back({{"hash", hash}, {"message", msg}});
    }
  }

  return {
    {"success", true},
    {"output", out},
    {"commits", commits},
    {"count", commits.size()}
  };
}

// ── Handler: git_add ──
// 暂存指定文件。参数: files (文件路径数组)
    json handle_git_add(const json& params) {
  std::string files_arg;
  if (params.contains("files") && params["files"].is_array()) {
    for (const auto& f : params["files"]) {
      if (f.is_string()) files_arg += " " + ShellArg::escape(f.get<std::string>());  // v0.52.7
    }
  }
  if (files_arg.empty()) {
    return {{"success", false}, {"error", "files_required"}};
  }

  // v0.54.26: 判定成败**只看退出码**。旧实现扫输出里有没有 "error" —— 但 git 失败时输出的是
  // **`fatal: ...`（不含 "error"）** ⇒ **真失败被漏判成 success**（实测：`git add <不存在路径>`
  // 返回 success=true 而什么都没暂存；与 v0.54.22 修掉的 write_stdin"假报成功"同族）。
  int add_status = 0;
  std::string out = shell_capture_status("git add" + files_arg + " 2>&1", &add_status);
  const bool ok = (add_status == 0);
  return {
    {"success", ok},
    {"output", ok ? "Files staged successfully" : out}
  };
}

// ── Handler: git_commit ──
// 提交暂存的改动。参数: message (提交信息)
    json handle_git_commit(const json& params) {
  std::string msg = params.value("message", "");
  if (msg.empty()) return {{"success", false}, {"error", "message_required"}};

  // v0.52.7: 整体单引号转义（原双引号包裹+内部转义不防 $() 注入——
  // 双引号内命令替换照样展开；单引号包裹是 POSIX 唯一字面量保证）
  std::string out = shell_capture("git commit -m " + ShellArg::escape(msg) + " 2>&1");

  // 提取 commit hash（格式: [main abc1234] message）
  std::string hash;
  auto bracket = out.find(']');
  if (bracket != std::string::npos) {
    auto space = out.rfind(' ', bracket);
    if (space != std::string::npos)
      hash = out.substr(space + 1, bracket - space - 1);
  }

  bool ok = !out.empty() && out.find("nothing to commit") == std::string::npos;
  return {
    {"success", ok},
    {"output", out},
    {"hash", hash}
  };
}

// ── Handler: git_show ──
// 查看某次提交的完整 diff。参数: commit (hash 或 HEAD~N)
    json handle_git_show(const json& params) {
  std::string commit = params.value("commit", "HEAD");
  // 防止命令注入 — 只允许字母数字、~、^、-、.
  for (char c : commit) {
    if (!std::isalnum(static_cast<unsigned char>(c)) &&
        c != '~' && c != '^' && c != '-' && c != '.') {
      return {{"success", false}, {"error", "invalid_commit_ref"}};
    }
  }
  int show_status = 0;
  std::string out = shell_capture_status("git show " + commit + " 2>&1", &show_status);
  if (show_status != 0 || out.empty()) {
    return {{"success", false}, {"output", out}, {"error", "commit_not_found"}};
  }
  return {{"success", true}, {"output", out}};
}

// ── Handler: git_checkout ──
// 切换分支或恢复文件。参数: target (分支名 / 文件路径), branch (bool, 默认false=恢复文件)
    json handle_git_checkout(const json& params) {
  std::string target = params.value("target", "");
  if (target.empty()) return {{"success", false}, {"error", "target_required"}};

  bool is_branch = params.value("branch", false);
  // v0.52.7: 白名单校验（字母数字/._/-/斜杠/空格）——git ref 与路径
  // 字符集之外的（元字符/引号/$）直接拒，杜绝对注入面
  for (char c : target) {
    if (!std::isalnum(static_cast<unsigned char>(c)) &&
        c != '.' && c != '_' && c != '-' && c != '/' && c != ' ') {
      return {{"success", false}, {"error", "invalid_target_chars"}};
    }
  }
  std::string cmd = is_branch ? "git checkout " + ShellArg::escape(target)
                              : "git checkout -- " + ShellArg::escape(target);
  int co_status = 0;
  std::string out = shell_capture_status(cmd + " 2>&1", &co_status);

  if (co_status != 0) {
    return {{"success", false}, {"output", out}};
  }
  return {{"success", true}, {"output", out.empty() ? "OK" : out}};
}

// ── Handler: git_branch ──
// 列出分支。参数: remote (bool, 默认false), all (bool, 默认true=包含本地+远程)
    json handle_git_branch(const json& params) {
  bool remote = params.value("remote", false);
  std::string cmd = remote ? "git branch -r" : "git branch";
  cmd += " 2>&1";
  std::string out = shell_capture(cmd);

  json branches = json::array();
  std::istringstream iss(out);
  std::string line;
  while (std::getline(iss, line)) {
    // trim whitespace
    size_t start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) continue;
    line = line.substr(start);
    // 标记当前分支 (*)
    bool current = !line.empty() && line[0] == '*';
    if (current) line = line.substr(2);
    branches.push_back({{"name", line}, {"current", current}});
  }

  return {
    {"success", true},
    {"output", out},
    {"branches", branches},
    {"count", branches.size()}
  };
}

}  // namespace git_ops
}  // namespace thin_agent

// ── 插件入口 ──

extern "C" const char* thin_agent_plugin_init(
    thin_agent::SkillRegistry& registry) {

  registry.register_cpp_handler("git_status",   thin_agent::git_ops::handle_git_status);
  registry.register_cpp_handler("git_diff",     thin_agent::git_ops::handle_git_diff);
  registry.register_cpp_handler("git_log",      thin_agent::git_ops::handle_git_log);
  registry.register_cpp_handler("git_add",      thin_agent::git_ops::handle_git_add);
  registry.register_cpp_handler("git_commit",   thin_agent::git_ops::handle_git_commit);
  registry.register_cpp_handler("git_show",     thin_agent::git_ops::handle_git_show);
  registry.register_cpp_handler("git_checkout", thin_agent::git_ops::handle_git_checkout);
  registry.register_cpp_handler("git_branch",   thin_agent::git_ops::handle_git_branch);

  return "git_ops";
}
