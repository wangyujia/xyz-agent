// test_gateway_session_map：v0.52.28 网关会话映射持久化回归
//
// 覆盖：建表/open 加载/lookup miss/assign 落盘/重启后（重新 open 同库）
// lookup 命中/覆盖写/多映射隔离。

#include "../../include/thin_agent/core/GatewaySessionMap.h"
#include <cstdio>
#include <unistd.h>

using thin_agent::GatewaySessionMap;

static int g_failures = 0;
#define CHECK(cond, msg) \
  do { if (!(cond)) { ++g_failures; std::printf("FAIL: %s\n", msg); } \
       else { std::printf("PASS: %s\n", msg); } } while (0)

int main() {
  const char* tmp = "/tmp/test_gw_session_map.db";
  ::unlink(tmp);

  // 第一"代"：写两条映射
  {
    GatewaySessionMap m;
    CHECK(m.open(tmp), "open+建表");
    CHECK(m.lookup("feishu:oc_123").empty(), "初始 lookup miss");
    m.assign("feishu:oc_123", "chat-1");
    m.assign("cli", "chat-2");
    CHECK(m.lookup("feishu:oc_123") == "chat-1", "内存 lookup 命中");
  }
  // 第二"代"（模拟重启）：重新 open 同库 → 恢复
  {
    GatewaySessionMap m;
    CHECK(m.open(tmp), "重启 open");
    CHECK(m.size() == 2, "恢复 2 条映射");
    CHECK(m.lookup("feishu:oc_123") == "chat-1", "重启后 lookup 命中(1)");
    CHECK(m.lookup("cli") == "chat-2", "重启后 lookup 命中(2)");
    CHECK(m.lookup("不存在").empty(), "未知 key 返回空");
    // 覆盖写
    m.assign("cli", "chat-9");
    CHECK(m.lookup("cli") == "chat-9", "覆盖写即时生效");
  }
  // 第三"代"：覆盖已持久化
  {
    GatewaySessionMap m;
    CHECK(m.open(tmp), "三启 open");
    CHECK(m.lookup("cli") == "chat-9", "覆盖写持久化");
    CHECK(m.lookup("feishu:oc_123") == "chat-1", "另一条不受影响");
  }
  // open 失败退化：坏路径 → false，不崩
  {
    GatewaySessionMap m;
    CHECK(!m.open("/nonexistent_dir/x/y/z.db"), "坏路径 open 失败不崩");
    m.assign("k", "v");  // 无 db 也应内存可用
    CHECK(m.lookup("k") == "v", "退化模式内存可用");
  }

  ::unlink(tmp);
  if (g_failures == 0) std::printf("ALL PASS\n");
  return g_failures == 0 ? 0 : 1;
}
