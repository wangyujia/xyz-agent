#include "thin_agent/core/PseudoTerminal.h"
#include "thin_agent/core/BackgroundProcessManager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

static int failures = 0;

void check(const std::string& label, bool ok, const std::string& detail = "") {
  printf("%s %s\n", ok ? "  PASS:" : "  FAIL:", label.c_str());
  if (!ok) {
    ++failures;
    if (!detail.empty()) printf("    detail: %s\n", detail.c_str());
  }
}

int main() {
  printf("=== PTY Tests ===\n");

  // ── PTY: simple echo ──
  {
    auto r = thin_agent::pty_exec("echo hello");
    check("pty echo exit_code=0", r.exit_code == 0,
          "exit_code=" + std::to_string(r.exit_code));
    check("pty echo output", r.output.find("hello") != std::string::npos,
          "output=" + r.output);
  }

  // ── PTY: command with args ──
  {
    auto r = thin_agent::pty_exec("ls /tmp 2>/dev/null || true");
    check("pty ls exit_code=0", r.exit_code == 0,
          "exit_code=" + std::to_string(r.exit_code));
  }

  // ── PTY: failing command ──
  {
    auto r = thin_agent::pty_exec("exit 42");
    check("pty exit 42 exit_code=42", r.exit_code == 42,
          "exit_code=" + std::to_string(r.exit_code));
  }

  // ── PTY: with stdin ──
  {
    auto r = thin_agent::pty_exec("bash -c 'read line; echo \"$line\"'", "hello stdin\n", 5000);
    check("pty stdin exit_code=0", r.exit_code == 0,
          "exit_code=" + std::to_string(r.exit_code));
    check("pty stdin output", r.output.find("hello stdin") != std::string::npos,
          "output=" + r.output);
  }

  // ── PTY: timeout ──
  {
    auto r = thin_agent::pty_exec("sleep 10", "", 500);
    check("pty timeout timed_out", r.timed_out,
          "timed_out=" + std::string(r.timed_out ? "true" : "false"));
  }

  printf("\n=== Background Process Tests ===\n");

  auto& mgr = thin_agent::BackgroundProcessManager::instance();

  // ── BG: start + poll running ──
  {
    auto sid = mgr.start("sleep 10");
    check("bg start returns id", !sid.empty(), "sid=" + sid);

    auto state = mgr.poll(sid);
    check("bg poll running", state.running,
          "running=" + std::string(state.running ? "true" : "false"));
    check("bg poll pid>0", state.pid > 0,
          "pid=" + std::to_string(state.pid));

    // Kill it
    auto killed = mgr.kill(sid);
    check("bg kill success", !killed.running,
          "running=" + std::string(killed.running ? "true" : "false"));
  }

  // ── BG: start + wait (short-lived process) ──
  {
    auto sid = mgr.start("echo background_test");
    check("bg echo start returns id", !sid.empty());

    auto state = mgr.wait(sid, 5000);
    check("bg echo wait exit_code=0", state.exit_code == 0,
          "exit_code=" + std::to_string(state.exit_code));
    check("bg echo output", state.output.find("background_test") != std::string::npos,
          "output=" + state.output);
  }

  // ── BG: exit code ──
  {
    auto sid = mgr.start("exit 7");
    auto state = mgr.wait(sid, 3000);
    check("bg exit 7 exit_code=7", state.exit_code == 7,
          "exit_code=" + std::to_string(state.exit_code));
  }

  // ── BG: poll nonexistent ──
  {
    auto state = mgr.poll("nonexistent_session");
    check("bg poll nonexistent shows error",
          state.output.find("process not found") != std::string::npos ||
          state.output.find("not found") != std::string::npos,
          "output=" + state.output);
  }

  // ── BG: kill nonexistent ──
  {
    auto state = mgr.kill("nonexistent_session_2");
    check("bg kill nonexistent shows error",
          state.output.find("process not found") != std::string::npos ||
          state.output.find("not found") != std::string::npos,
          "output=" + state.output);
  }

  // ── BG: wait 输出完整性（v0.54.8 / R93 回归锚）──
  // 背景：输出由 monitor 线程异步 drain，而 wait() 此前在**子进程退出瞬间**就返回，还把
  // running 置 false ⇒ monitor 从此不再读该管道（只收集 running==true 的进程）⇒ 快命令偶发
  // "exit_code=0 但 output 为空"。实测：负载下 25 次复现 1 次（`bg echo output` FAIL，detail 空）。
  // 修后必须 20/20 都有输出（本锚连跑即可暴露回退）。
  {
    int empty = 0;
    for (int i = 0; i < 20; ++i) {
      auto sid = mgr.start("echo bg_wait_drain_probe");
      auto state = mgr.wait(sid, 5000);
      if (state.output.find("bg_wait_drain_probe") == std::string::npos) ++empty;
    }
    check("bg wait 输出完整性（20 连跑无空输出）", empty == 0,
          "empty=" + std::to_string(empty) + "/20");
  }

  printf("\n=== Results: %d failures ===\n", failures);
  return failures > 0 ? 1 : 0;
}
