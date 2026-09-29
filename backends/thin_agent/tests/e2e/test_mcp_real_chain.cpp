// test_mcp_real_chain（**e2e 层**）：真 MCP server 链路回归
//   —— npx @modelcontextprotocol/server-filesystem 握手 + list_tools + call_tool
//
// 为什么在 e2e 层、且**默认 SKIP**（v0.54.9 / R93「测试预算」未闭环项）：
//   本用例是**真链路**——要拉起真实 MCP server 进程（`npx -y` 首次/未缓存需联网下载包）。
//   它此前住在 tests/unit/ 里（`test_stdio_transport_args.cpp` 第 4 组），实测**同一目标**
//   全量耗时 76.97s / 358.19s / 472.96s / 474.37s（4 次），占全量 637s 的 **74%**
//   （比其余 134 项加起来还慢），且 TIMEOUT 560 已贴近实测 474s ⇒ npm 再慢一点就"触顶假红"。
//   故：①移出 unit 层（unit 层只留 python3 -c 的确定性组，~1s）
//       ②默认不跑，**显式 TA_MCP_REAL=1 才跑** ⇒ 全量预算回到 ~200s
//       ③真链路覆盖一点不缩水，需要时按下面口令跑。
//
// 跑法：TA_MCP_REAL=1 ctest -R e2e_mcp_real_chain --output-on-failure
//      （或直接 TA_MCP_REAL=1 ./build/e2e_mcp_real_chain）
//
// 覆盖（与原 unit 版第 4 组逐条一致）：握手 connect → list_tools 非空 → 含 read 类工具 →
// call_tool(list_directory, /tmp) 有返回 → ok=true（**v0.53.7 回归锚：大响应不再截断**）。

#include "thin_agent/agent/StdioTransport.h"
#include "thin_agent/agent/McpClient.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using thin_agent::agent::McpClient;
using thin_agent::agent::StdioTransport;

static int g_failures = 0;
#define CHECK(cond, msg) \
  do { if (!(cond)) { ++g_failures; std::printf("FAIL: %s\n", msg); } \
       else { std::printf("PASS: %s\n", msg); } } while (0)

int main() {
  // ── 门控一：显式开关（默认不跑，保护全量预算）──
  if (const char* on = ::getenv("TA_MCP_REAL"); !on || std::string(on) != "1") {
    std::printf("SKIP: 真链路默认不跑（TA_MCP_REAL=1 才跑；npx 真链路单次可达 470s+）\n");
    return 0;
  }
  // ── 门控二：npx 不可用则跳过（环境缺 node/npm）──
  if (std::system("npx --version > /dev/null 2>&1") != 0) {
    std::printf("SKIP: 真链路（npx 不可用）\n");
    return 0;
  }

  StdioTransport t;
  t.set_args({"-y", "@modelcontextprotocol/server-filesystem", "/tmp"});
  // 注意：McpClient::connect 内部自调 transport.connect——此处不预连
  McpClient client(t, "npx");
  bool hs = client.connect("thin_agent_test", "0.54.9");
  if (!hs) {
    std::printf("SKIP: npx server 启动失败（网络/包不可达）\n");
    return 0;
  }

  auto tools = client.list_tools();
  CHECK(!tools.empty(), "真链路: list_tools 非空");
  bool has_read = false;
  for (auto& tl : tools)
    if (tl.name.find("read") != std::string::npos) has_read = true;
  CHECK(has_read, "真链路: 含 read_file 类工具");

  // call_tool 真调用：列 /tmp（list_directory）
  std::string tool_name;
  for (auto& tl : tools)
    if (tl.name.find("list") != std::string::npos) { tool_name = tl.name; break; }
  if (!tool_name.empty()) {
    auto r = client.call_tool(tool_name, {{"path", "/tmp"}});
    CHECK(r.dump().size() > 10, "真链路: call_tool 有返回");
    std::printf("  (call %s -> %.80s...)\n", tool_name.c_str(), r.dump().c_str());
    // v0.53.7 回归锚：大响应（/tmp 全列表 >64KB）不再截断
    CHECK(r.value("ok", false), "真链路: call_tool ok=true（大响应不截断）");
  }

  if (g_failures == 0) std::printf("ALL PASS\n");
  return g_failures == 0 ? 0 : 1;
}
