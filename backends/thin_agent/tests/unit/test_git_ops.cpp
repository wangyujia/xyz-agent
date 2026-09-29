/// unit_git_ops：Git 操作插件测试
///
/// 测试 thin_agent::git_ops 的 8 个 handler

#include <cstdlib>
#include <unistd.h>
#include <iostream>
#include <string>

#include "thin_agent/plugin/PluginInterface.h"
#include "../../src/plugin/skills/git_ops.cpp"

#define ASSERT(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << std::endl; return 1; } \
} while (0)
#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { std::cerr << "FAIL: " << msg << " (" << (a) << " != " << (b) << ")" << std::endl; return 1; } \
} while (0)

#define TEST(name) int test_##name()

using namespace nlohmann;

static bool g_git_ok = false;

static void check_git() {
  // Check git is available AND we're in a git repo
  g_git_ok = (system("git --version > /dev/null 2>&1") == 0);
  if (g_git_ok) {
    g_git_ok = (system("git rev-parse --git-dir > /dev/null 2>&1") == 0);
  }
}

// ── git_status ──

TEST(status_clean) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  auto r = thin_agent::git_ops::handle_git_status(json::object());
  ASSERT(r["success"].get<bool>(), "should succeed");
  std::cout << "pass: status_clean" << std::endl;
  return 0;
}

// ── git_diff ──

TEST(diff_no_changes) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  auto r = thin_agent::git_ops::handle_git_diff(json::object());
  ASSERT(r["success"].get<bool>(), "should succeed");
  std::cout << "pass: diff_no_changes" << std::endl;
  return 0;
}

TEST(diff_staged) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  json params = {{"staged", true}};
  auto r = thin_agent::git_ops::handle_git_diff(params);
  ASSERT(r["success"].get<bool>(), "should succeed");
  std::cout << "pass: diff_staged" << std::endl;
  return 0;
}

// ── git_log ──

TEST(log_default) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  auto r = thin_agent::git_ops::handle_git_log(json::object());
  ASSERT(r["success"].get<bool>(), "should succeed");
  ASSERT(r["commits"].is_array(), "commits is array");
  ASSERT(r["commits"].size() >= 1, "at least 1 commit");
  std::cout << "pass: log_default" << std::endl;
  return 0;
}

TEST(log_custom_n) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  json params = {{"n", 1}};
  auto r = thin_agent::git_ops::handle_git_log(params);
  ASSERT(r["success"].get<bool>(), "should succeed");
  ASSERT_EQ(r["count"].get<int>(), 1, "1 commit");
  std::cout << "pass: log_custom_n" << std::endl;
  return 0;
}

TEST(log_n_clamped) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  json params = {{"n", 999}};
  auto r = thin_agent::git_ops::handle_git_log(params);
  ASSERT(r["success"].get<bool>(), "should succeed");
  ASSERT(r["count"].get<int>() <= 100, "clamped to 100");
  std::cout << "pass: log_n_clamped" << std::endl;
  return 0;
}

// ── git_show ──

TEST(show_head) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  json params = {{"commit", "HEAD"}};
  auto r = thin_agent::git_ops::handle_git_show(params);
  ASSERT(r["success"].get<bool>(), "should succeed");
  std::cout << "pass: show_head" << std::endl;
  return 0;
}

TEST(show_bad_ref) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  json params = {{"commit", "nonexistent1234567"}};
  auto r = thin_agent::git_ops::handle_git_show(params);
  ASSERT(!r["success"].get<bool>(), "should fail on bad ref");
  std::cout << "pass: show_bad_ref" << std::endl;
  return 0;
}

TEST(show_injection_attempt) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  json params = {{"commit", "HEAD; rm -rf /"}};
  auto r = thin_agent::git_ops::handle_git_show(params);
  ASSERT(!r["success"].get<bool>(), "should reject injection");
  std::cout << "pass: show_injection_attempt" << std::endl;
  return 0;
}

// ── git_branch ──

TEST(branch_local) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  auto r = thin_agent::git_ops::handle_git_branch(json::object());
  ASSERT(r["success"].get<bool>(), "should succeed");
  ASSERT(r["branches"].is_array(), "branches is array");
  ASSERT(r["branches"].size() >= 1, "at least 1 branch");
  std::cout << "pass: branch_local" << std::endl;
  return 0;
}

// ── git_add ──

TEST(add_empty_files) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  json params = {{"files", json::array()}};
  auto r = thin_agent::git_ops::handle_git_add(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty files");
  std::cout << "pass: add_empty_files" << std::endl;
  return 0;
}

TEST(add_no_files_param) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  json params;
  auto r = thin_agent::git_ops::handle_git_add(params);
  ASSERT(!r["success"].get<bool>(), "should fail without files");
  std::cout << "pass: add_no_files_param" << std::endl;
  return 0;
}

// ── git_commit ──

TEST(commit_empty_message) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  json params = {{"message", ""}};
  auto r = thin_agent::git_ops::handle_git_commit(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty message");
  std::cout << "pass: commit_empty_message" << std::endl;
  return 0;
}

// ── git_checkout ──

TEST(checkout_empty_target) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  json params = {{"target", ""}};
  auto r = thin_agent::git_ops::handle_git_checkout(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty target");
  std::cout << "pass: checkout_empty_target" << std::endl;
  return 0;
}

// ── v0.54.26 回归：**判定成败必须看退出码，不能扫输出里的 "fatal:"/"error:"** ──
// 背景（本仓实测踩到）：`git show HEAD` 的输出包含 **diff 正文**；若提交的 diff / 提交信息 / 被查看文件
// 内容里出现字面量 "fatal:"，旧实现 `out.find("fatal:")` 即误判失败 ⇒ 正常提交被报 commit_not_found。
// 本仓 v0.54.24 的提交里恰有一行 `TA_LOG_LINE("fatal: " << e.what());` ⇒ unit_git_ops.show_head 确定性变红。
TEST(show_diff_containing_fatal_string) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  char tmpl[] = "/tmp/git_ops_fatal_XXXXXX";
  char* mk = ::mkdtemp(tmpl);
  if (mk == nullptr) { std::cout << "SKIP: mkdtemp failed" << std::endl; return 0; }
  const std::string dir = mk;

  char cwd[4096];
  if (::getcwd(cwd, sizeof(cwd)) == nullptr) { std::cout << "SKIP: getcwd" << std::endl; return 0; }
  const std::string saved = cwd;

  const std::string setup =
      "cd " + dir + " && git init -q . && printf 'fatal: 这是内容里的字面量\n' > f.txt && "
      "git add f.txt && git -c user.email=t@t -c user.name=t commit -q -m 'fatal: 提交信息里也有'";
  const int rc = ::system(setup.c_str());

  json params = {{"commit", "HEAD"}};
  json r;
  bool ok_chdir = (::chdir(dir.c_str()) == 0);
  if (ok_chdir) r = thin_agent::git_ops::handle_git_show(params);
  if (ok_chdir) (void)::chdir(saved.c_str());   // 先还原 cwd，避免影响后续用例

  std::string cmd = "rm -rf " + dir;
  (void)::system(cmd.c_str());

  if (rc != 0 || !ok_chdir) { std::cout << "SKIP: 临时仓库准备失败" << std::endl; return 0; }
  ASSERT(r["success"].get<bool>(),
         "diff/提交信息含 'fatal:' 时仍应成功（判定须看退出码，不得扫描输出文本）");
  std::cout << "pass: show_diff_containing_fatal_string" << std::endl;
  return 0;
}

// ── v0.54.26 回归（**反向**缺陷：漏判失败）──
// 旧实现：`git add` 用 `out.find("error") == npos` 判成功；但 git 失败时输出的是 **`fatal: ...`**
// （**不含 "error"**）⇒ **真失败被报成 success**。用不存在的路径复现：`git add <不存在>` 必然失败。
TEST(add_nonexistent_path_must_fail) {
  if (!g_git_ok) { std::cout << "SKIP: git not available" << std::endl; return 0; }
  json params = {{"files", json::array({"/nonexistent_path_for_git_ops_test_xyz"})}};
  auto r = thin_agent::git_ops::handle_git_add(params);
  ASSERT(!r["success"].get<bool>(),
         "git add 不存在的路径必须报告失败（不得因输出里没有 'error' 而漏判为成功）");
  std::cout << "pass: add_nonexistent_path_must_fail" << std::endl;
  return 0;
}

int main() {
  check_git();

  int fails = 0;
  #define RUN(name) do { \
    std::cout << "test: " #name "..." << std::endl; \
    if (test_##name() != 0) ++fails; \
  } while (0)

  RUN(status_clean);
  RUN(diff_no_changes);
  RUN(diff_staged);
  RUN(log_default);
  RUN(log_custom_n);
  RUN(log_n_clamped);
  RUN(show_head);
  RUN(show_diff_containing_fatal_string);
  RUN(add_nonexistent_path_must_fail);
  RUN(show_bad_ref);
  RUN(show_injection_attempt);
  RUN(branch_local);
  RUN(add_empty_files);
  RUN(add_no_files_param);
  RUN(commit_empty_message);
  RUN(checkout_empty_target);

  std::cout << (fails == 0 ? "ALL PASS" : "SOME FAILED") << " (" << fails << " failures)" << std::endl;
  return fails;
}
