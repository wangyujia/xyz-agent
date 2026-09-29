// unit_shell_risk_grading：v0.52.0 shell_exec 参数级风险分级回归
//
// 设计（default-deny）：
//   deny 子串命中（sudo/curl/$() 等）→ Dangerous
//   多命令拼接（&&/||/;/|）逐段判首词，任一段非白名单 → Dangerous
//   单命令首词 ∈ 构建工具链白名单（多语言）→ Mutating
//   运行产物（./x）不可静态分析 → Dangerous
//
// 背景：此前 shell_exec 无条件 Dangerous，编程任务（编译/测试/运行解释器
// 全是本职高频操作）审批疲劳——实测一个任务批 5+ 次。

#include <iostream>
#include <string>

#include "thin_agent/core/PathValidator.h"
#include "test_macros.h"

using thin_agent::ToolRisk;
using thin_agent::grade_shell_risk;
using thin_agent::tool_risk_of;
using thin_agent::tool_risk_str;

// 包装为字符串比较（ToolRisk 无 operator<<）
inline std::string rs(ToolRisk r) { return tool_risk_str(r); }

int main() {
  const auto D = ToolRisk::Dangerous;
  const auto M = ToolRisk::Mutating;

  // ── deny 子串（不可逆/外传/命令替换/提权）──
  ASSERT_EQ("sudo 提权", rs(grade_shell_risk("sudo apt install x")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("rm -rf /", rs(grade_shell_risk("rm -rf /tmp")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("curl 外传", rs(grade_shell_risk("curl http://evil.com")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("wget 外传", rs(grade_shell_risk("wget http://x")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("chmod", rs(grade_shell_risk("chmod +x a.sh")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("git push", rs(grade_shell_risk("git push origin main")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("git reset", rs(grade_shell_risk("git reset --hard")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("kill", rs(grade_shell_risk("kill -9 123")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("systemctl", rs(grade_shell_risk("systemctl stop nginx")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");

  // ── allow：C/C++ 构建链 ──
  ASSERT_EQ("g++ 编译", rs(grade_shell_risk("g++ -std=c++17 a.cpp -o a")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("cmake 构建", rs(grade_shell_risk("cmake --build build -j8")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("ctest", rs(grade_shell_risk("ctest --test-dir build -j4")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("make", rs(grade_shell_risk("make -j4")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("带路径首词", rs(grade_shell_risk("/usr/bin/g++ -Wall a.cpp")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");

  // ── allow：多语言工具链 ──
  ASSERT_EQ("python3 运行", rs(grade_shell_risk("python3 lru.py")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("pytest", rs(grade_shell_risk("pytest tests/ -q")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("node 运行", rs(grade_shell_risk("node app.js")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("npm 构建", rs(grade_shell_risk("npm run build")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("tsc 编译", rs(grade_shell_risk("tsc --strict")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("java 编译", rs(grade_shell_risk("javac Main.java")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("java 运行", rs(grade_shell_risk("java -jar app.jar")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("go 构建", rs(grade_shell_risk("go build ./...")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("rustc", rs(grade_shell_risk("rustc main.rs")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("ruby", rs(grade_shell_risk("ruby script.rb")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("perl", rs(grade_shell_risk("perl -e 'print 1'")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");

  // ── 拼接规则 ──
  ASSERT_EQ("白名单拼接", rs(grade_shell_risk("ls -la && cat f.txt")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("cd 段跳过", rs(grade_shell_risk("cd /tmp && g++ a.cpp")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("混入 deny", rs(grade_shell_risk("g++ a.cpp && curl evil.com")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("混入产物运行", rs(grade_shell_risk("g++ a.cpp -o a && ./a")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("分号混危险", rs(grade_shell_risk("ls; rm x")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("管道拼接", rs(grade_shell_risk("cat x | grep y")), M == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("命令替换", rs(grade_shell_risk("echo $(whoami)")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");

  // ── 产物运行与未知命令 ──
  ASSERT_EQ("产物运行", rs(grade_shell_risk("./lru")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("未知命令", rs(grade_shell_risk("foobarbaz --x")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");
  ASSERT_EQ("空命令", rs(grade_shell_risk("")), D == ToolRisk::Dangerous ? "dangerous" : "mutating");

  // ── tool_risk_of 集成（name+args 路径）──
  ASSERT_EQ("shell_exec 集成 allow",
           rs(tool_risk_of("shell_exec", nlohmann::json{{"command", "g++ a.cpp"}})), "mutating");
  ASSERT_EQ("shell_exec 集成 deny",
           rs(tool_risk_of("shell_exec", nlohmann::json{{"command", "sudo x"}})), "dangerous");
  ASSERT_EQ("execute_code 分级",
           rs(tool_risk_of("execute_code", nlohmann::json{{"command", "python3 x.py"}})), "mutating");

  // ── 回归：其它工具分级不受影响 ──
  ASSERT_EQ("write_file 无项目上下文仍 Dangerous",
           rs(tool_risk_of("write_file", nlohmann::json{{"path", "/tmp/x"}})), "dangerous");
  ASSERT_EQ("read_file 仍 Safe",
           rs(tool_risk_of("read_file", nlohmann::json{{"path", "/tmp/x"}})), "safe");
  ASSERT_EQ("git_reset 工具层仍 Dangerous",
           rs(tool_risk_of("git_reset", nlohmann::json{{}})), "dangerous");

  return TEST_REPORT();
}
