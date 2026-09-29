// v0.53.20: 沙箱路径视图重写纯函数单测（include 源文件直测 static 函数）
#include <cassert>
#include <cstdio>
#include <string>

#include "thin_agent/core/sandbox_paths.h"

using thin_agent::rewrite_sandbox_tmp_paths;
using thin_agent::unrewrite_sandbox_tmp_paths;

int fails = 0;
void check(bool ok, const char* name) {
  if (!ok) { std::printf("FAIL: %s\n", name); ++fails; }
  else std::printf("PASS: %s\n", name);
}

int main() {
  // 入参重写
  check(rewrite_sandbox_tmp_paths("ls /tmp") == "ls /host_tmp", "裸 /tmp 重写");
  check(rewrite_sandbox_tmp_paths("ls /tmp/") == "ls /host_tmp/", "尾斜杠重写");
  check(rewrite_sandbox_tmp_paths("cat /tmp/a.txt") == "cat /host_tmp/a.txt", "子路径重写");
  check(rewrite_sandbox_tmp_paths("ls /tmpxyz") == "ls /tmpxyz", "前缀撞名不误伤");
  check(rewrite_sandbox_tmp_paths("ls /host_tmp") == "ls /host_tmp", "已正确形式不二次重写");
  check(rewrite_sandbox_tmp_paths("echo /tmpfs") == "echo /tmpfs", "/tmpfs 不误伤");
  check(rewrite_sandbox_tmp_paths("a /tmp/b /tmp/c") == "a /host_tmp/b /host_tmp/c", "多处重写");
  // 反重写（输出）
  check(unrewrite_sandbox_tmp_paths("/host_tmp/calc.cpp") == "/tmp/calc.cpp", "输出路径还原");
  check(unrewrite_sandbox_tmp_paths("x /host_tmp/a y /host_tmp/b") == "x /tmp/a y /tmp/b", "多输出还原");
  // 对称性（往返）
  check(unrewrite_sandbox_tmp_paths(rewrite_sandbox_tmp_paths("cat /tmp/a")) == "cat /tmp/a",
        "入出对称（往返恒等）");
  std::printf(fails ? "%d FAILED\n" : "ALL PASS\n", fails);
  return fails ? 1 : 0;
}
