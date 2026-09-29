// test_reconnect_backoff：重连退避策略（v0.53.99, R86）
//
// 修复前的行为：im_gateway_v2 的 on_timer(3s) 里 `if (!g_agent_conn) agent_connect();`
// **无退避** → agent 长期不可用时每 3s 一次尝试、永不收敛（连接 churn + 日志刷屏）。
//
// 断言：档位序列 3/6/12/24/48/60 秒、封顶不再增长、reset() 复位、非法参数被纠正。
#include <cstdint>
#include <iostream>

#include "thin_agent/gateway/ReconnectBackoff.h"

static int g_fail = 0;
#define CHECK(cond, msg)                                            \
  do {                                                              \
    if (!(cond)) { std::cout << "FAIL: " << msg << std::endl; ++g_fail; } \
    else { std::cout << "PASS: " << msg << std::endl; }             \
  } while (0)

int main() {
  using thin_agent::ReconnectBackoff;
  ReconnectBackoff b;
  const int64_t expect[] = {3000, 6000, 12000, 24000, 48000, 60000, 60000, 60000};
  for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); ++i) {
    const int64_t d = b.next_delay_ms();
    CHECK(d == expect[i], std::string("第 ") + std::to_string(i + 1) + " 档 = " +
                              std::to_string(expect[i] / 1000) + "s（实测 " +
                              std::to_string(d / 1000) + "s）");
  }
  CHECK(b.at_cap(), "封顶后可观测 at_cap()==true");

  b.reset();
  CHECK(b.pending_delay_ms() == 3000, "reset() 后回到基准档 3s");
  CHECK(b.next_delay_ms() == 3000, "reset() 后下一次仍是 3s（不是 6s）");

  ReconnectBackoff tiny(100, 400);
  CHECK(tiny.next_delay_ms() == 100 && tiny.next_delay_ms() == 200 &&
            tiny.next_delay_ms() == 400 && tiny.next_delay_ms() == 400,
        "自定义 base/cap 生效（100→200→400 封顶）");

  ReconnectBackoff bad(0, 0);  // 非法参数：base<=0 / cap<base
  CHECK(bad.next_delay_ms() == ReconnectBackoff::kBaseMs,
        "非法参数纠正为基准 3s（不返回 0 = 不忙等）");

  std::cout << (g_fail == 0 ? "ALL PASS" : "FAILED") << std::endl;
  return g_fail == 0 ? 0 : 1;
}
