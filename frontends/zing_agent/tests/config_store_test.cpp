// zing_agent: ConfigStore 单测（KV 持久化——v7）
// 覆盖：路径解析（env 覆盖）/load-set-load 跨实例持久/损坏 JSON 自愈/
// del/has/all_json 契约/写穿真落盘（文件内容断言）。
#include "ConfigStore.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <string>

static int g_failed = 0;
static void check(bool ok, const std::string& name) {
  std::cout << (ok ? "PASS: " : "FAIL: ") << name << "\n";
  if (!ok) g_failed++;
}

int main() {
  const std::string dir = "/tmp/zing_cfg_test_" + std::to_string(::getpid());
  ::system(("mkdir -p " + dir).c_str());
  const std::string path = dir + "/state.json";

  // ── 1) 默认路径 env 覆盖 ──
  {
    ::setenv("ZING_STATE_PATH", "/tmp/zing_env_probe.json", 1);
    check(zing::ConfigStore::default_path() == "/tmp/zing_env_probe.json",
          "env ZING_STATE_PATH 覆盖路径");
    ::unsetenv("ZING_STATE_PATH");
    const auto p = zing::ConfigStore::default_path();
    check(p.find("zing_agent") != std::string::npos && p.find("state.json") != std::string::npos,
          "默认路径含 zing_agent/state.json（实际: " + p + "）");
  }

  // ── 2) set 写穿+跨实例 load 持久 ──
  {
    zing::ConfigStore s1(path);
    s1.set("zing_theme", "dark");
    s1.set("zing_sessions", "[{\"id\":\"a\"}]");
    // 文件真落盘（内容断言）
    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    check(content.find("\"zing_theme\":\"dark\"") != std::string::npos ||
          content.find("\"zing_theme\": \"dark\"") != std::string::npos,
          "写穿落盘（文件含 zing_theme=dark）");
    // 第二实例（模拟进程重启）
    zing::ConfigStore s2(path);
    check(s2.load(), "跨实例 load 成功");
    check(s2.get("zing_theme") == "dark", "重启后主题恢复=dark");
    check(s2.get("zing_sessions") == "[{\"id\":\"a\"}]", "重启后会话列表恢复");
    check(!s2.has("nope"), "has 不存在键=false");
  }

  // ── 3) del ──
  {
    zing::ConfigStore s(path);
    s.load();
    s.del("zing_theme");
    zing::ConfigStore s2(path);
    s2.load();
    check(!s2.has("zing_theme") && s2.get("zing_theme", "def") == "def",
          "del 持久化（重启后仍无）");
  }

  // ── 4) 损坏 JSON 自愈 ──
  {
    { std::ofstream f(path, std::ios::trunc); f << "{broken!!!"; }
    zing::ConfigStore s(path);
    check(!s.load(), "损坏 JSON load 返回 false");
    check(s.get("x", "ok") == "ok", "损坏后不炸（空表兜底）");
    s.set("recovered", "1");  // 写穿应成功（重建文件）
    zing::ConfigStore s2(path);
    s2.load();
    check(s2.get("recovered") == "1", "损坏后重写可恢复");
  }

  // ── 5) all_json 契约（cfgGetAll 桥数据源）──
  {
    zing::ConfigStore s(path);
    s.load();
    const std::string j = s.all_json();
    check(!j.empty() && j.front() == '{', "all_json 对象形态");
    check(j.find("recovered") != std::string::npos, "all_json 含键");
  }

  // ── 6) 原子写崩溃模拟：残留 .tmp 不影响主文件 ──
  {
    zing::ConfigStore s(path);
    s.load();
    s.set("atomic", "1");
    { std::ofstream f(path + ".tmp", std::ios::trunc); f << "{half-written"; }  // 模拟崩溃残留
    zing::ConfigStore s2(path);
    check(s2.load() && s2.get("atomic") == "1", "残留 .tmp 不破坏主文件（原子替换）");
  }

  ::system(("rm -rf " + dir).c_str());
  std::cout << (g_failed ? "FAILED\n" : "ALL PASS\n");
  return g_failed ? 1 : 0;
}
