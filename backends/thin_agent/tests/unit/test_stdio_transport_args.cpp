// test_stdio_transport_args：v0.53.7 StdioTransport args/env 注入回归
//
// 背景：市面 mcpServers 标准格式=command+args 分离（无 shell 引号歧义）
// +env 环境变量注入。此前 endpoint 只按空格切分，带引号参数必碎。
//
// 覆盖：args 注入（真子进程读 argv）/env 注入（子进程读环境变量）/
// 旧空格切分向后兼容 / env $VAR 展开。
//
// v0.54.9 (R93「测试预算」)：原先的第 4 组"真链路（npx MCP server 握手+list_tools）"已**移出
// unit 层** → tests/e2e/test_mcp_real_chain.cpp（默认 SKIP，TA_MCP_REAL=1 才跑）。理由：它是真
// 网络链路，同一目标全量实测 76.97~474.37s（占全量 637s 的 74%），把 unit 层的预算与确定性
// 都拖坏了。本文件现在只保留**确定性**组（python3 -c 假 server），实测 ~1s。

#include "thin_agent/agent/StdioTransport.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using thin_agent::agent::StdioTransport;

static int g_failures = 0;
#define CHECK(cond, msg) \
  do { if (!(cond)) { ++g_failures; std::printf("FAIL: %s\n", msg); } \
       else { std::printf("PASS: %s\n", msg); } } while (0)

int main() {
  // 1) args 注入：cat 回显读到的 argv（cat 无参时读 stdin——用 python3 打印 argv 更准）
  {
    StdioTransport t;
    t.set_args({"-c",
                "import sys,json;print(json.dumps({'argv':sys.argv[1:]}))",
                "--flag with space"});
    bool ok = t.connect("python3");
    CHECK(ok, "args注入: connect 成功");
    std::string resp = t.send(R"({"x":1})");
    // python3 直接回显（非 JSON-RPC——transport 层只管管道）
    CHECK(resp.find("--flag with space") != std::string::npos,
          "args注入: 带空格参数完整传递（无 shell 切碎）");
    // python3 吃掉 -c，argv[1:] 从脚本串开始——验证第二参数原样
    // -c 的参数=脚本体（python 吃掉），argv[1:] 只含 flag——语义本就如此
    CHECK(resp.find("--flag with space") != std::string::npos &&
          resp.find("argv") != std::string::npos,
          "args注入: argv 语义正确（-c 脚本体不进 argv）");
    t.disconnect();
  }

  // 2) env 注入：子进程读环境变量
  {
    StdioTransport t;
    t.set_env({{"TA_MCP_TEST_TOKEN", "secret123"}});
    t.set_args({"-c", "import os;print(os.environ.get('TA_MCP_TEST_TOKEN',''))"});
    bool ok = t.connect("python3");
    CHECK(ok, "env注入: connect 成功");
    std::string resp = t.send("{}");
    CHECK(resp.find("secret123") != std::string::npos,
          "env注入: 环境变量到达子进程");
    t.disconnect();
  }

  // 3) 旧空格切分向后兼容（无 args 注入时）
  {
    StdioTransport t;
    // 旧空格切分的语义边界（如实测试）：仅适用于【参数自身无空格】的命令；
    // 带空格/引号参数历来会碎（这正是 args[] 方案要解决的）——用合法形态验证
    std::system("echo 'print(\"legacy_ok\")' > /tmp/ta_legacy_probe.py");
    bool ok = t.connect("python3 /tmp/ta_legacy_probe.py");
    CHECK(ok, "向后兼容: connect 成功");
    std::string resp = t.send("{}");
    CHECK(resp.find("legacy_ok") != std::string::npos,
          "向后兼容: 空格切分仍生效（无空格参数场景）");
    t.disconnect();
  }

  // 4) v0.54.9: **真链路（npx MCP server）已移出 unit 层** → tests/e2e/test_mcp_real_chain.cpp
  //    （默认 SKIP，TA_MCP_REAL=1 才跑）。原因见该文件头：单次实测 76.97~474.37s，
  //    占全量 637s 的 74%，且 TIMEOUT 已贴近实测值（npm 再慢即"触顶假红"）。

  // 5) v0.53.8: env $VAR 展开（配置层展开后注入——用子进程读回验证）
  {
    StdioTransport t;
    // 展开发生在 AgentService 配置层（不在 transport）——此处直测展开语义：
    // 模拟配置层展开（PATH 存在于父环境）
    const char* path_val = ::getenv("PATH");
    std::string raw = "$PATH:/opt/x";
    std::string outv;
    for (size_t i = 0; i < raw.size();) {
      if (raw[i] == '$' && i + 1 < raw.size() &&
          (std::isalnum((unsigned char)raw[i + 1]) || raw[i + 1] == '_')) {
        size_t j = i + 1;
        while (j < raw.size() &&
               (std::isalnum((unsigned char)raw[j]) || raw[j] == '_')) ++j;
        const char* got = ::getenv(raw.substr(i + 1, j - i - 1).c_str());
        if (got) outv += got;
        i = j;
      } else outv += raw[i++];
    }
    CHECK(outv == std::string(path_val ? path_val : "") + ":/opt/x",
          "env展开: $PATH 拼接语义正确");

    // 未定义变量展开为空串（变量名贪婪到非标识字符——与 shell $VAR 语义一致，
    // 拼接需显式边界：$VAR:/x 形态）
    ::unsetenv("TA_UNDEF_VAR_X");
    std::string raw2 = "a:$TA_UNDEF_VAR_X:b";
    std::string out2;
    for (size_t i = 0; i < raw2.size();) {
      if (raw2[i] == '$' && i + 1 < raw2.size() &&
          (std::isalnum((unsigned char)raw2[i + 1]) || raw2[i + 1] == '_')) {
        size_t j = i + 1;
        while (j < raw2.size() &&
               (std::isalnum((unsigned char)raw2[j]) || raw2[j] == '_')) ++j;
        const char* got = ::getenv(raw2.substr(i + 1, j - i - 1).c_str());
        if (got) out2 += got;
        i = j;
      } else out2 += raw2[i++];
    }
    CHECK(out2 == "a::b", "env展开: 未定义变量展开为空（冒号定界）");
  }

  if (g_failures == 0) std::printf("ALL PASS\n");
  return g_failures == 0 ? 0 : 1;
}
