// test_code_exec_pgroup：v0.52.10 code_exec 进程组收割回归
//
// 背景：超时杀树只 kill(pid)（中间子进程），孙进程（python3）未
// setpgid 自立进程组 → 超时后孤儿化残留。真 e2e 实测 22 个
// python3 /tmp/ta_exec_*.py 残留打满 22 核数小时。
//
// 验证（直接调用插件 handler 源码，走完整 fork→exec→超时→收割链）：
// 1. 死循环+派生子进程的超时代码：整树收割，无残留
// 2. 正常代码不受影响：exit 0 + 输出完整
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "thin_agent/plugin/PluginInterface.h"

#include "test_macros.h"

// 单测直链插件实现（.so 是 dlopen 加载无法链接；源码 include 走真实路径）
#include "../../src/plugin/skills/code_exec.cpp"

using thin_agent::code_exec::handle_code_exec;

static int count_ta_exec_procs() {
  FILE* f = ::popen("pgrep -fc 'python3 -u /tmp/ta_exec_' 2>/dev/null || true",
                    "r");
  if (!f) return -1;
  char buf[64] = {0};
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  pclose(f);
  if (n == 0) return 0;
  return std::atoi(buf);
}

int main() {
  // 1) 超时整树收割
  {
    const char* code =
        "import subprocess, sys\n"
        "subprocess.Popen([sys.executable, '-c', 'while True: pass'])\n"
        "while True: pass\n";
    nlohmann::json params;
    params["code"] = code;
    params["timeout"] = 2;

    int before = count_ta_exec_procs();
    auto r = handle_code_exec(params);
    ASSERT_TRUE("超时应报 timeout", !r.value("success", true));

    bool harvested = false;
    for (int i = 0; i < 8; ++i) {  // 宽限 4s（超时+宽限收割完成）
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (count_ta_exec_procs() <= before) { harvested = true; break; }
    }
    ASSERT_TRUE("整树收割: 无新增残留进程", harvested);
  }

  // 2) 正常执行不受影响
  {
    nlohmann::json params;
    params["code"] = "print('ok_pgroup')";
    params["timeout"] = 10;
    auto r = handle_code_exec(params);
    ASSERT_TRUE("正常执行 success", r.value("success", false));
    ASSERT_TRUE("输出完整",
                r.value("output", "").find("ok_pgroup") != std::string::npos);
  }

  return TEST_REPORT();
}
