#include "thin_agent/core/WorkflowManager.h"
#include "thin_agent/core/PathValidator.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unistd.h>

namespace thin_agent {

namespace fs = std::filesystem;

// ── 路径常量 ──

std::string WorkflowManager::workflows_dir() {
  const char* home = std::getenv("HOME");
  return (home ? std::string(home) : "/root") + "/.thin_agent/workflows";
}
std::string WorkflowManager::builtin_dir()  { return workflows_dir() + "/builtin"; }
std::string WorkflowManager::user_dir()     { return workflows_dir() + "/user"; }
std::string WorkflowManager::current_link() { return workflows_dir() + "/_current"; }
std::string WorkflowManager::session_path() { return workflows_dir() + "/_session.json"; }

std::string WorkflowManager::name_from_path(const std::string& path) {
  return fs::path(path).stem().string();
}

// ── 单例 ──

WorkflowManager& WorkflowManager::instance() {
  static WorkflowManager mgr;
  return mgr;
}

// ── JSON ↔ WorkflowDef ──

WorkflowDef WorkflowManager::load(const std::string& path) {
  WorkflowDef wf;
  std::ifstream in(path);
  if (!in.is_open()) return wf;
  try {
    auto j = nlohmann::json::parse(in);
    wf.name        = j.value("name", name_from_path(path));
    wf.description = j.value("description", "");
    wf.domain      = j.value("domain", "programming");
    wf.max_retries = j.value("max_retries", 3);
    if (j.contains("post_code_steps") && j["post_code_steps"].is_array()) {
      for (const auto& s : j["post_code_steps"]) {
        WorkflowStep step;
        step.cmd  = s.value("cmd", "");
        step.desc = s.value("desc", "");
        step.max_retries = s.value("max_retries", 0);
        wf.post_code_steps.push_back(step);
      }
    }
  } catch (...) {}
  return wf;
}

bool WorkflowManager::save(const std::string& path, const WorkflowDef& wf) {
  nlohmann::json j;
  j["name"]        = wf.name;
  j["description"] = wf.description;
  j["domain"]      = wf.domain;
  j["max_retries"] = wf.max_retries;
  j["post_code_steps"] = nlohmann::json::array();
  for (const auto& s : wf.post_code_steps) {
    nlohmann::json sj;
    sj["cmd"]   = s.cmd;
    sj["desc"]  = s.desc;
    if (s.max_retries > 0) sj["max_retries"] = s.max_retries;
    j["post_code_steps"].push_back(sj);
  }
  std::ofstream out(path);
  if (!out.is_open()) return false;
  out << j.dump(2) << "\n";
  return true;
}

// ── 列表 ──

std::vector<WorkflowDef> WorkflowManager::list_all() {
  std::vector<WorkflowDef> result;
  std::string dirs[] = {builtin_dir(), user_dir()};
  for (const auto& dir : dirs) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (!ec && entry.path().extension() == ".json") {
        result.push_back(load(entry.path().string()));
      }
    }
  }
  return result;
}

// ── 查看 ──

WorkflowDef WorkflowManager::view(const std::string& name) {
  std::string udir = user_dir();
  std::string bdir = builtin_dir();
  for (const auto* dir : {&udir, &bdir}) {
    std::string path = *dir + "/" + name + ".json";
    if (fs::exists(path)) return load(path);
  }
  return WorkflowDef{};
}

// ── 创建 ──

bool WorkflowManager::create(const WorkflowDef& wf) {
  fs::create_directories(user_dir());
  return save(user_dir() + "/" + wf.name + ".json", wf);
}

// ── 切换 ──

bool WorkflowManager::set_current(const std::string& name) {
  std::string udir = user_dir();
  std::string bdir = builtin_dir();
  std::string target;
  for (const auto* dir : {&udir, &bdir}) {
    std::string p = *dir + "/" + name + ".json";
    if (fs::exists(p)) { target = p; break; }
  }
  if (target.empty()) return false;

  // 更新符号链接（v0.54.16: 改用 std::filesystem —— 原来的裸 `unlink()`/`symlink()` 是
  // POSIX 名，**MSVC 没有**（MinGW 亦无 symlink）⇒ Windows 侧编不过。`fs::create_symlink`
  // 两平台都由标准库映射到各自系统调用，且失败以 error_code 报出而不抛异常：
  // Windows 上需开发者模式/SeCreateSymbolicLinkPrivilege，无权限时如实返回 false。
  std::string link = current_link();
  std::error_code ec;
  fs::remove(link, ec);            // 不存在时 fs::remove 返回 false 且不置错
  ec.clear();
  fs::create_symlink(target, link, ec);
  return !ec;
}

// ── 删除 ──

bool WorkflowManager::remove(const std::string& name) {
  std::string path = user_dir() + "/" + name + ".json";
  return fs::remove(path);
}

// ── 当前工作流（四层优先级）──

WorkflowDef WorkflowManager::current(const std::string& project_dir) {
  // Layer 4: 会话覆盖
  if (fs::exists(session_path())) return load(session_path());

  // Layer 3: 项目配置
  if (!project_dir.empty()) {
    std::string project_wf = project_dir + "/.thin_agent/workflow.json";
    if (fs::exists(project_wf)) return load(project_wf);
  }

  // Layer 2→1: _current 链接指向的用户或内置工作流
  std::string link = current_link();
  if (fs::exists(link)) {
    // v0.54.16: 用 error_code 重载——此前 `fs::read_symlink(link)` 在 `_current` **不是符号链接**
    // （例如被普通文件顶掉、或 Windows 上 create_symlink 因权限失败后留下的残file）时**直接抛异常**
    // 穿透 current()，把一次"工作流取默认值"变成崩溃。现在读不到就如实回落到内置 generic。
    std::error_code ec;
    const auto tgt = fs::read_symlink(link, ec);
    if (!ec) return load(tgt.string());
  }

  // 兜底：内置 generic
  return load(builtin_dir() + "/generic.json");
}

// ── Workflow 注入（统一占位符 {WORKFLOW_STEPS} 替换）──

/// 从 workflow JSON 生成完整的工作流文本（步骤4-N + retry + commit）。
/// 语言无关 — step 命令可以是任意语言。
std::string WorkflowManager::steps_to_prompt_text(const WorkflowDef& wf, bool zh) const {
  std::string text;
  int step_num = 4;
  for (const auto& s : wf.post_code_steps) {
    if (zh) {
      text += std::to_string(step_num) + ". 用 shell_exec " + s.desc + "："
           + s.cmd + "\\n";
    } else {
      text += std::to_string(step_num) + ". " + s.desc
           + " with shell_exec: " + s.cmd + "\\n";
    }
    ++step_num;
  }
  // 固定的 retry + commit 步骤
  if (zh) {
    text += std::to_string(step_num) + ". 如构建或测试失败：读错误 → 修复 → 重新构建 → 重新测试（最多 3 次）\\n";
    text += std::to_string(step_num + 1) + ". 提交：git_add + git_commit\\n";
  } else {
    text += std::to_string(step_num) + ". If build or test fails: read errors → fix → rebuild → retest (up to 3 times)\\n";
    text += std::to_string(step_num + 1) + ". Commit with git_add + git_commit\\n";
  }
  return text;
}

/// 占位符替换核心
static std::string replace_placeholder(const std::string& text,
                                        const std::string& replacement) {
  auto pos = text.find("{WORKFLOW_STEPS}");
  if (pos == std::string::npos) return text;
  return text.substr(0, pos) + replacement + text.substr(pos + 17);
}

std::string WorkflowManager::inject_into_system_prompt(
    const std::string& prompt, const WorkflowDef& wf) const {
  // system prompt 是英文的
  return replace_placeholder(prompt,
      wf.post_code_steps.empty() ? "" : steps_to_prompt_text(wf, false));
}

std::string WorkflowManager::inject_into_profile_zh(
    const std::string& body, const WorkflowDef& wf) const {
  return replace_placeholder(body,
      wf.post_code_steps.empty() ? "" : steps_to_prompt_text(wf, true));
}

std::string WorkflowManager::inject_into_profile_en(
    const std::string& body, const WorkflowDef& wf) const {
  return replace_placeholder(body,
      wf.post_code_steps.empty() ? "" : steps_to_prompt_text(wf, false));
}

// ── 工作流执行引擎 ──

/// 执行单个 shell 命令，捕获 stdout + stderr
static std::pair<int, std::string> exec_cmd(const std::string& cmd) {
  std::array<char, 4096> buf{};
  std::string output;
  // v0.53.43: 步骤看门狗——workflow 可存长命令(build/watch),卡死一步
  // =worker 线程永久占用。POSIX 用 timeout 300 前缀(Windows 步骤命令
  /// 通常为短命令,维持原样与 SyntaxChecker 同策略)。
#ifdef _WIN32
  std::string full_cmd = cmd + " 2>&1";
    // TODO(v0.53.x Windows 真机): Windows 分支无超时(GNU timeout 不可用)
    /// ——改走 SandboxExecutor Job Object 或 WaitOrTimer 模式
#else
  std::string full_cmd = "timeout 300 " + cmd + " 2>&1";
#endif
  #pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-attributes"
  std::unique_ptr<FILE, decltype(&pclose)> pipe(
      popen(full_cmd.c_str(), "r"), pclose);
#pragma GCC diagnostic pop  // v0.53.16: 惯用法告警压制
  if (!pipe) return {-1, "popen failed"};
  while (fgets(buf.data(), buf.size(), pipe.get()) != nullptr) {
    output += buf.data();
  }
  int rc = pclose(pipe.release());
  // v0.54.16: `WEXITSTATUS` 是 POSIX wait 宏，**MSVC/MinGW 都没有** ⇒ Windows 侧编不过。
  // Windows 的 `_pclose` 直接返回子进程退出码（MS 文档：returns the exit status of the
  // command），无需解码。
#ifdef _WIN32
  int exit_code = rc;
#else
  int exit_code = WEXITSTATUS(rc);
#endif
  return {exit_code, output};
}

nlohmann::json WorkflowManager::run(const std::string& workflow_name,
                                     int retry,
                                     const std::string& project_dir) {
  WorkflowDef wf = view(workflow_name);
  if (wf.name.empty()) {
    return {
      {"success", false},
      {"status", "not_found"},
      {"error", "Workflow not found: " + workflow_name}
    };
  }

  int max_retries = wf.max_retries;
  if (retry > max_retries) {
    return {
      {"success", false},
      {"status", "max_retries_exceeded"},
      {"error", "Max retries (" + std::to_string(max_retries) +
                ") exceeded for workflow: " + workflow_name},
      {"retry", retry},
      {"max_retries", max_retries}
    };
  }

  nlohmann::json steps_result = nlohmann::json::array();
  int total_steps = static_cast<int>(wf.post_code_steps.size());
  int passed = 0;

  for (int i = 0; i < total_steps; ++i) {
    const auto& step = wf.post_code_steps[i];
    int step_retries = step.max_retries > 0 ? step.max_retries : max_retries;

    // v0.52.7: 步骤命令过 shell_exec 同款分级判官——workflow_create
    // 可存任意 cmd，若不在执行时校验=绕过 shell_exec 分级的免审批
    // 任意命令通道。Dangerous 拒执行（提示创建者改步骤）。
    if (grade_shell_risk(step.cmd) == ToolRisk::Dangerous) {
      nlohmann::json sr;
      sr["step"] = i + 1;
      sr["desc"] = step.desc;
      sr["cmd"] = step.cmd;
      sr["status"] = "rejected";
      sr["reason"] = "dangerous_command_blocked";
      return {
        {"success", false},
        {"status", "step_rejected"},
        {"failed_step", sr},
        {"passed", passed},
        {"total", total_steps},
        {"error", "workflow step blocked by risk grading (v0.52.7)"}
      };
    }

    auto [exit_code, output] = exec_cmd(step.cmd);

    nlohmann::json sr;
    sr["step"] = i + 1;
    sr["total"] = total_steps;
    sr["desc"] = step.desc;
    sr["cmd"] = step.cmd;
    sr["exit_code"] = exit_code;
    sr["output"] = output;
    sr["retry"] = retry;
    sr["max_retries"] = max_retries;

    if (exit_code != 0) {
      sr["status"] = "failed";
      return {
        {"success", false},
        {"status", "step_failed"},
        {"failed_step", sr},
        {"passed", passed},
        {"total", total_steps},
        {"retry", retry},
        {"max_retries", max_retries},
        {"hint", "Fix the issue and call workflow_run with retry=" +
                 std::to_string(retry + 1)}
      };
    }

    sr["status"] = "passed";
    steps_result.push_back(sr);
    ++passed;
  }

  return {
    {"success", true},
    {"status", "all_passed"},
    {"steps", steps_result},
    {"passed", passed},
    {"total", total_steps},
    {"retry", retry},
    {"max_retries", max_retries},
    {"output", "All " + std::to_string(total_steps) +
               " workflow steps passed" +
               (retry > 1 ? " (after " + std::to_string(retry - 1) +
                                " retries)" : "")}
  };
}

}  // namespace thin_agent
