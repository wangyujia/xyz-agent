/// test_path_validator.cpp — v0.48.0 路径安全校验测试
///
/// 测试覆盖：
/// 1. 路径规范化（./, ../, 多斜杠, ~展开）
/// 2. 核心层黑名单（系统路径拒绝、二进制覆盖拒绝）
/// 3. 项目级白名单（project_root 内放行、外拒绝、ro模式写拒绝）
/// 4. 路径逃逸防护（../../etc/passwd）
/// 5. 会话隔离（不同 session 不同 ProjectContext）

#include "thin_agent/core/PathValidator.h"
#include "thin_agent/core/V4APatcher.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace thin_agent;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
  if (cond) { ++g_pass; } \
  else { ++g_fail; std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl; } \
} while(0)

// ═══════════════════════════════════════════════════
// 1. 路径规范化
// ═══════════════════════════════════════════════════

void test_normalize() {
  std::cerr << "\n=== test_normalize ===" << std::endl;

  // 基本绝对路径不变
  CHECK(PathValidator::normalize("/home/user/project", "/tmp") == "/home/user/project",
        "absolute path should not change");

  // 消除尾斜杠
  CHECK(PathValidator::normalize("/home/user/", "/tmp") == "/home/user",
        "trailing slash removed");

  // 消除 ./
  CHECK(PathValidator::normalize("/home/./user/project", "/tmp") == "/home/user/project",
        "dot segment eliminated");

  // 消除 ../
  CHECK(PathValidator::normalize("/home/user/../other/project", "/tmp") == "/home/other/project",
        "dotdot segment eliminated");

  // 相对路径拼接 base_dir
  CHECK(PathValidator::normalize("src/main.cpp", "/home/user/project") == "/home/user/project/src/main.cpp",
        "relative path joined with base");

  // 多重斜杠
  CHECK(PathValidator::normalize("/home///user//project", "/tmp") == "/home/user/project",
        "multiple slashes collapsed");

  // 深度 ../
  CHECK(PathValidator::normalize("/a/b/c/../../../x", "/tmp") == "/x",
        "deep dotdot");

  // ../ 超出根
  CHECK(PathValidator::normalize("/../../../etc", "/tmp") == "/etc",
        "dotdot beyond root stays at root");
}

// ═══════════════════════════════════════════════════
// 2. 目录包含判断
// ═══════════════════════════════════════════════════

void test_is_within() {
  std::cerr << "\n=== test_is_within ===" << std::endl;

  CHECK(PathValidator::is_within("/home/user", "/home/user/project/main.cpp"),
        "child in parent");

  CHECK(PathValidator::is_within("/home/user", "/home/user"),
        "same path is within");

  CHECK(!PathValidator::is_within("/home/user", "/home/user_evil/secret"),
        "user_evil is NOT within user (prefix trap)");

  CHECK(!PathValidator::is_within("/home/user", "/etc/passwd"),
        "unrelated path");

  CHECK(PathValidator::is_within("/home/user", "/home/user/a/b/c/d"),
        "deep child");
}

// ═══════════════════════════════════════════════════
// 3. 核心层黑名单
// ═══════════════════════════════════════════════════

void test_core_blacklist() {
  std::cerr << "\n=== test_core_blacklist ===" << std::endl;

  // /etc/ 写拒绝
  {
    auto r = PathValidator::validate_core("/etc/passwd", PathValidator::Op::Write);
    CHECK(!r.allowed, "/etc/passwd write should be blocked");
    CHECK(r.reason.find("system_path") != std::string::npos, "reason should mention system_path");
  }

  // /etc/ 读允许（黑名单只拦截写）
  {
    auto r = PathValidator::validate_core("/etc/passwd", PathValidator::Op::Read);
    CHECK(r.allowed, "/etc/passwd read should be allowed by core");
  }

  // /boot/efi 写拒绝
  {
    auto r = PathValidator::validate_core("/boot/efi/boot.img", PathValidator::Op::Write);
    CHECK(!r.allowed, "/boot write blocked");
  }

  // /lib64 写拒绝
  {
    auto r = PathValidator::validate_core("/lib64/libc.so.6", PathValidator::Op::Write);
    CHECK(!r.allowed, "/lib64 write blocked");
  }

  // 正常路径写允许
  {
    auto r = PathValidator::validate_core("/tmp/test_safe.txt", PathValidator::Op::Write);
    CHECK(r.allowed, "/tmp write allowed");
  }

  // 已有 .so 文件覆盖拒绝
  {
    // 先创建一个临时 .so 文件
    std::string tmp_so = "/tmp/test_path_validator_dummy.so";
    {
      std::ofstream f(tmp_so);
      f << "dummy";
    }
    auto r = PathValidator::validate_core(tmp_so, PathValidator::Op::Write);
    CHECK(!r.allowed, "overwriting existing .so should be blocked");
    CHECK(r.reason.find("binary_overwrite") != std::string::npos, "reason should mention binary_overwrite");
    std::remove(tmp_so.c_str());
  }

  // 新建 .so 文件允许（文件不存在）
  {
    auto r = PathValidator::validate_core("/tmp/newlib.so", PathValidator::Op::Write);
    CHECK(r.allowed, "creating new .so should be allowed");
  }

  // /root/.ssh/authorized_keys 写拒绝
  {
    auto r = PathValidator::validate_core("/root/.ssh/authorized_keys", PathValidator::Op::Write);
    CHECK(!r.allowed, "/root/.ssh write blocked");
  }
}

// ═══════════════════════════════════════════════════
// 4. 项目级白名单
// ═══════════════════════════════════════════════════

void test_project_whitelist() {
  std::cerr << "\n=== test_project_whitelist ===" << std::endl;

  PathValidator::ProjectContext ctx;
  ctx.project_root = "/home/user/myproject";
  ctx.read_only = false;  // rw 模式
  ctx.valid = true;

  // 项目内写允许（rw 模式）
  {
    auto r = PathValidator::validate_project(
        "/home/user/myproject/src/main.cpp", PathValidator::Op::Write, ctx);
    CHECK(r.allowed, "write inside project (rw) should be allowed");
  }

  // 项目外写拒绝
  {
    auto r = PathValidator::validate_project(
        "/home/other/file.txt", PathValidator::Op::Write, ctx);
    CHECK(!r.allowed, "write outside project should be blocked");
    CHECK(r.reason.find("outside_project_root") != std::string::npos, "reason should mention outside_project_root");
  }

  // 系统路径仍然拒绝（即使项目内也不行——但 /etc 不在项目内所以不会触发）
  {
    auto r = PathValidator::validate_project(
        "/etc/passwd", PathValidator::Op::Write, ctx);
    CHECK(!r.allowed, "/etc write blocked even with project context");
    CHECK(r.reason.find("system_path") != std::string::npos, "reason should mention system_path");
  }

  // 读操作项目外允许
  {
    auto r = PathValidator::validate_project(
        "/usr/include/stdio.h", PathValidator::Op::Read, ctx);
    CHECK(r.allowed, "read outside project should be allowed");
  }

  // 切换到 ro 模式
  ctx.read_only = true;
  {
    auto r = PathValidator::validate_project(
        "/home/user/myproject/src/main.cpp", PathValidator::Op::Write, ctx);
    CHECK(!r.allowed, "write inside project (ro) should be blocked");
    CHECK(r.reason.find("read_only") != std::string::npos, "reason should mention read_only");
  }

  // ro 模式下项目内读允许
  {
    auto r = PathValidator::validate_project(
        "/home/user/myproject/src/main.cpp", PathValidator::Op::Read, ctx);
    CHECK(r.allowed, "read inside project (ro) should be allowed");
  }
}

// ═══════════════════════════════════════════════════
// 5. 路径逃逸防护
// ═══════════════════════════════════════════════════

void test_path_escape() {
  std::cerr << "\n=== test_path_escape ===" << std::endl;

  PathValidator::ProjectContext ctx;
  ctx.project_root = "/home/user/myproject";
  ctx.read_only = false;
  ctx.valid = true;

  // ../../ 逃逸到项目外
  {
    auto r = PathValidator::validate_project(
        "/home/user/myproject/../../etc/passwd", PathValidator::Op::Write, ctx);
    CHECK(!r.allowed, "path traversal escape should be blocked");
    // 规范化后是 /etc/passwd → system_path 拦截
  }

  // 相对路径逃逸（需要 base_dir）
  {
    // 在 project_root 下的相对路径
    std::string escape = "../../../etc/shadow";
    auto r = PathValidator::validate_project(escape, PathValidator::Op::Write, ctx);
    CHECK(!r.allowed, "relative path escape should be blocked");
  }

  // 深层嵌套逃逸
  {
    auto r = PathValidator::validate_project(
        "/home/user/myproject/src/../../../other/secret.txt", PathValidator::Op::Write, ctx);
    CHECK(!r.allowed, "deep nested escape blocked");
  }
}

// ═══════════════════════════════════════════════════
// 6. TLS 会话上下文
// ═══════════════════════════════════════════════════

void test_tls_context() {
  std::cerr << "\n=== test_tls_context ===" << std::endl;

  // 初始状态：无上下文
  ProjectContextTLS::clear();
  CHECK(!ProjectContextTLS::current().ctx.valid, "initial TLS should be invalid");

  // 设置上下文
  PathValidator::ProjectContext ctx;
  ctx.project_root = "/tmp/test_project";
  ctx.read_only = true;
  ctx.valid = true;
  ProjectContextTLS::set(ctx);
  CHECK(ProjectContextTLS::current().ctx.valid, "TLS should be valid after set");
  CHECK(ProjectContextTLS::current().ctx.project_root == "/tmp/test_project",
        "TLS project_root matches");

  // 清除
  ProjectContextTLS::clear();
  CHECK(!ProjectContextTLS::current().ctx.valid, "TLS should be invalid after clear");
}

// ═══════════════════════════════════════════════════
// 7. 无项目上下文退化到核心策略
// ═══════════════════════════════════════════════════

void test_fallback_no_context() {
  std::cerr << "\n=== test_fallback_no_context ===" << std::endl;

  PathValidator::ProjectContext empty_ctx;  // valid = false

  // 无上下文 → 退化到核心黑名单
  {
    auto r = PathValidator::validate_project("/etc/passwd", PathValidator::Op::Write, empty_ctx);
    CHECK(!r.allowed, "no context: /etc write blocked by core blacklist");
  }

  {
    auto r = PathValidator::validate_project("/tmp/safe.txt", PathValidator::Op::Write, empty_ctx);
    CHECK(r.allowed, "no context: /tmp write allowed by core fallback");
  }
}

// ═══════════════════════════════════════════════════
// 8. safe_write_file
// ═══════════════════════════════════════════════════

void test_safe_write_file() {
  std::cerr << "\n=== test_safe_write_file ===" << std::endl;

  // 正常写入（无 TLS 上下文 → 核心黑名单）
  {
    std::string tmp = "/tmp/test_safe_write_basic.txt";
    auto r = safe_write_file(tmp, "hello world\n", {.verify = true, .syntax_check = false});
    CHECK(r.success, "basic write should succeed");
    CHECK(r.bytes_written == 12, "bytes_written should be 12");
    std::remove(tmp.c_str());
  }

  // /etc 写入拒绝
  {
    auto r = safe_write_file("/etc/test_blocked.txt", "x");
    CHECK(!r.success, "/etc write should be blocked");
    CHECK(r.error.find("system_path") != std::string::npos, "reason should mention system_path");
  }

  // 自动建父目录
  {
    std::string tmp = "/tmp/test_safe_write_dir/sub/deep.txt";
    auto r = safe_write_file(tmp, "nested", {.verify = false, .syntax_check = false});
    CHECK(r.success, "write with auto-mkdir should succeed");
    std::remove(tmp.c_str());
    std::filesystem::remove_all("/tmp/test_safe_write_dir");
  }

  // 带项目上下文：项目内写允许
  {
    std::string tmp_dir = "/tmp/test_safe_write_project";
    std::filesystem::create_directories(tmp_dir);
    PathValidator::ProjectContext ctx;
    ctx.project_root = tmp_dir;
    ctx.read_only = false;
    ctx.valid = true;
    ProjectContextTLS::set(ctx);

    auto r = safe_write_file(tmp_dir + "/test.txt", "project file");
    CHECK(r.success, "write inside project should succeed");

    // 项目外写拒绝
    auto r2 = safe_write_file("/tmp/outside_project.txt", "x");
    CHECK(!r2.success, "write outside project should be blocked");

    ProjectContextTLS::clear();
    std::filesystem::remove_all(tmp_dir);
  }
}

// ═══════════════════════════════════════════════════
// 9. safe_read_file_paged
// ═══════════════════════════════════════════════════

void test_safe_read_file_paged() {
  std::cerr << "\n=== test_safe_read_file_paged ===" << std::endl;

  std::string tmp = "/tmp/test_safe_read_paged.txt";
  {
    std::ofstream f(tmp);
    f << "line1\nline2\nline3\nline4\nline5\n";
  }

  // 全量读
  {
    auto r = safe_read_file_paged(tmp);
    CHECK(r.success, "read should succeed");
    CHECK(r.total_lines == 5, "should have 5 lines");
    CHECK(r.output.find("1|line1") != std::string::npos, "should contain line 1");
  }

  // 分页读
  {
    auto r = safe_read_file_paged(tmp, 2, 2);
    CHECK(r.success, "paged read should succeed");
    CHECK(r.start_line == 2, "start_line should be 2");
    CHECK(r.end_line == 3, "end_line should be 3");
  }

  std::remove(tmp.c_str());
}

// ═══════════════════════════════════════════════════
// 10. safe_patch_file
// ═══════════════════════════════════════════════════

void test_safe_patch_file() {
  std::cerr << "\n=== test_safe_patch_file ===" << std::endl;

  std::string tmp = "/tmp/test_safe_patch.txt";
  {
    std::ofstream f(tmp);
    f << "hello old world\nfoo bar\n";
  }

  // 正常 patch
  {
    auto r = safe_patch_file(tmp, "old", "new");
    CHECK(r.success, "patch should succeed");
    CHECK(r.replacements >= 1, "should have >= 1 replacement");
  }

  // 验证写入成功
  {
    std::ifstream in(tmp);
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    CHECK(content.find("hello new world") != std::string::npos, "content should be patched");
  }

  // /etc patch 拒绝
  {
    auto r = safe_patch_file("/etc/passwd", "x", "y");
    CHECK(!r.success, "/etc patch should be blocked");
  }

  std::remove(tmp.c_str());
}

// ═══════════════════════════════════════════════════
// 11. V4APatcher 旁路封堵（v0.49.1）
// ═══════════════════════════════════════════════════

void test_v4a_patcher_blocked() {
  std::cerr << "\n=== test_v4a_patcher_blocked ===" << std::endl;

  // 构造一个 V4A patch 试图写 /etc/passwd
  std::string malicious_patch =
      "*** Begin Patch\n"
      "*** Update File: /etc/passwd\n"
      "@@ context @@\n"
      "-old\n"
      "+evil\n"
      "*** End Patch\n";

  thin_agent::V4APatcher patcher;
  auto result = patcher.apply_to_disk(malicious_patch);
  CHECK(!result.ok, "V4A patch to /etc must be blocked");
  bool has_blocked = false;
  for (const auto& e : result.errors) {
    if (e.find("path_blocked") != std::string::npos) has_blocked = true;
  }
  CHECK(has_blocked, "error should mention path_blocked");

  // 正常路径仍可用：/tmp 下的 patch 应通过校验（即使文件不存在会报 cannot_open）
  std::string tmp = "/tmp/test_v4a_patcher_ok.txt";
  { std::ofstream f(tmp); f << "hello world\n"; }
  std::string ok_patch =
      "*** Begin Patch\n"
      "*** Update File: " + tmp + "\n"
      "@@ context @@\n"
      "-hello world\n"
      "+patched world\n"
      "*** End Patch\n";
  auto r2 = patcher.apply_to_disk(ok_patch);
  CHECK(r2.ok, "V4A patch to /tmp should succeed");
  std::remove(tmp.c_str());
}

// ═══════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════

int main() {
  test_normalize();
  test_is_within();
  test_core_blacklist();
  test_project_whitelist();
  test_path_escape();
  test_tls_context();
  test_fallback_no_context();
  test_safe_write_file();
  test_safe_read_file_paged();
  test_safe_patch_file();
  test_v4a_patcher_blocked();

  std::cerr << "\n══════════════════════════════════════" << std::endl;
  std::cerr << "PathValidator: " << g_pass << " passed, " << g_fail << " failed" << std::endl;
  std::cerr << "══════════════════════════════════════" << std::endl;

  return g_fail > 0 ? 1 : 0;
}
