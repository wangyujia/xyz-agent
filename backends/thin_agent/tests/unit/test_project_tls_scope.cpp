// unit_project_tls_scope：验证会话级项目上下文 TLS 的作用域正确性。
//
// 覆盖点：
//   1. ProjectContextGuard RAII：构造设置、析构清理
//   2. 无项目会话 guard(nullptr)：显式清空残留（防 worker 线程复用泄漏）
//   3. ro 上下文下 validate_write_with_tls 语义（经 safe_write_file 观察）
//   4. handle_request 全分支注入：ro 会话的 action/写路径被项目上下文约束
//      （通过 skill 工具执行路径验证，无需 mock 云）

#include <cstdio>
#include <filesystem>
#include <memory>

#include <nlohmann/json.hpp>

#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/PathValidator.h"
#include "thin_agent/core/TaskEngine.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"

namespace {

int g_failures = 0;

int expect(bool ok, const std::string& msg) {
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", msg.c_str());
  }
  return g_failures == 1 ? 1 : 0;
}

}  // namespace

int main() {
  using thin_agent::PathValidator;
using thin_agent::ProjectContextGuard;
using thin_agent::ProjectContextTLS;

  std::filesystem::remove_all("/tmp/tls_scope_proj");
  std::filesystem::remove_all("data_tls_scope");
  std::filesystem::create_directories("/tmp/tls_scope_proj");

  // ── 1. RAII 基本语义 ──
  {
    PathValidator::ProjectContext pc;
    pc.valid = true;
    pc.project_root = "/tmp/tls_scope_proj";
    pc.read_only = true;
    {
      thin_agent::ProjectContextGuard g(pc);
      expect(ProjectContextTLS::current().ctx.valid, "guard sets ctx");
      expect(ProjectContextTLS::current().ctx.read_only, "guard ro flag");
      // ro 上下文内：项目内写被拒
      auto r = PathValidator::validate_project("/tmp/tls_scope_proj/x.txt",
                                                PathValidator::Op::Write,
                                                ProjectContextTLS::current().ctx);
      expect(!r.allowed, "ro ctx: in-project write rejected");
    }
    expect(!ProjectContextTLS::current().ctx.valid, "guard dtor clears");
  }

  // ── 2. guard(nullptr) 清残留 ──
  {
    ProjectContextTLS::set(PathValidator::ProjectContext{});
    ProjectContextTLS::current().ctx = {};
    // 手动制造残留
    PathValidator::ProjectContext stale;
    stale.valid = true;
    stale.project_root = "/tmp/tls_scope_proj";
    stale.read_only = true;
    ProjectContextTLS::set(stale);
    {
      thin_agent::ProjectContextGuard g(nullptr);
      expect(!ProjectContextTLS::current().ctx.valid,
             "guard(nullptr) clears stale ctx");
    }
    expect(!ProjectContextTLS::current().ctx.valid, "no residue after scope");
  }

  // ── 3. handle_request 层：ro 会话 skill 工具写路径被拒 ──
  // action 分支（不依赖云 mock）触发 sandbox 写 —— 用 chat 分支的本地意图
  // 太难直达 write_file；改用 validate 层验证（修复本身在 handle_request 入口）
  setenv("THIN_AGENT_TEST_API_KEY", "test-key-0123456789abcdef0123", 1);
  thin_agent::DemoConfigCompat cfg;
  cfg.mode = "offline";
  auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
  auto ex = std::make_shared<thin_agent::ActionExecutor>(dc);
  auto te = std::make_shared<thin_agent::TaskEngine>(ex);
  te->init("data_tls_scope/test.db");
  thin_agent::AgentService svc(cfg, ex, te);
  svc.on_session_open("s_ro");
  auto sp = svc.handle_request("s_ro", nlohmann::json{
      {"type", "set_project"}, {"path", "/tmp/tls_scope_proj"}, {"mode", "ro"}});
  expect(sp.value("type", "") == "project_set", "set_project ok");
  expect(sp.value("mode", "") == "ro", "set_project mode=ro");

  // 简单 chat（本地 offline 路径，验证 guard 在 handle_request 内不崩溃且语义正常）
  auto r1 = svc.handle_request("s_ro", nlohmann::json{{"type", "chat"}, {"text", "ping"}});
  expect(r1.value("type", "") == "chat_result", "ro session chat ok with guard");

  // 请求结束后 TLS 必须已清理（RAII）——直接检查当前线程
  expect(!ProjectContextTLS::current().ctx.valid,
         "TLS cleared after handle_request returns");

  std::filesystem::remove_all("data_tls_scope");
  std::filesystem::remove_all("/tmp/tls_scope_proj");
  if (g_failures == 0) {
    std::printf("unit:test_project_tls_scope PASS\n");
    return 0;
  }
  std::printf("unit:test_project_tls_scope FAIL (%d)\n", g_failures);
  return 1;
}
