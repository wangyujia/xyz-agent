// unit_llm_circuit_breaker：v0.52.6 熔断器+jitter 回归
//
// 状态机：closed → 3 连 transient 失败 → open（秒拒）→ 冷却后半开
// → 探测成功 closed / 失败 open 冷却翻倍。
// 隔离：endpoint A 熔断不影响 B。
// 4xx：不上报（由调用方判断——本测只测 breaker 本体语义）。
// jitter：±30% 边界 + THIN_AGENT_TEST_NO_JITTER 关闭。
#include <cstdlib>
#include <thread>

#include "thin_agent/llm/LlmCircuitBreaker.h"

#include "test_macros.h"

using namespace thin_agent;

int main() {
  auto& cb = LlmCircuitBreaker::instance();
  cb.reset();

  // 1) 正常态全放行
  ASSERT_TRUE("初始放行", cb.allow_request("https://a"));
  cb.report("https://a", true);
  ASSERT_TRUE("成功后仍放行", cb.allow_request("https://a"));

  // 2) 3 连失败 → 开闸
  cb.report("https://a", false);
  cb.report("https://a", false);
  ASSERT_TRUE("2 次失败未开闸", cb.allow_request("https://a"));
  cb.report("https://a", false);
  ASSERT_TRUE("3 次失败已开闸（is_open）", cb.is_open("https://a"));
  ASSERT_TRUE("开闸后秒拒", !cb.allow_request("https://a"));

  // 3) per-endpoint 隔离
  ASSERT_TRUE("B 端点不受 A 影响", cb.allow_request("https://b"));

  // 4) 半开探测：直接构造冷却期满场景——报告失败使 A 再次进入 open，
  // 但无法等 30s——用 reset 重建+直接验证状态机转换逻辑：
  // （冷却时长无法在单测中等待，状态机的时间分支由 allow_request
  //  内部 steady_clock 驱动；这里验证成功上报会复位全部状态）
  cb.report("https://b", false);
  cb.report("https://b", false);
  cb.report("https://b", false);
  ASSERT_TRUE("B 开闸", cb.is_open("https://b"));
  cb.report("https://b", true);  // （开闸态上报成功=管理性复位）
  ASSERT_TRUE("成功上报复位", !cb.is_open("https://b"));

  // 5) jitter 边界
  setenv("THIN_AGENT_TEST_NO_JITTER", "1", 1);
  ASSERT_EQ("NO_JITTER 关闭时精确返回", jittered_delay_ms(1000), 1000L);
  unsetenv("THIN_AGENT_TEST_NO_JITTER");
  bool seen_spread = false;
  long lo = 1000 * 70 / 100, hi = 1000 * 130 / 100;
  for (int i = 0; i < 200; ++i) {
    long d = jittered_delay_ms(1000);
    ASSERT_TRUE("jitter 范围内", d >= lo && d <= hi);
    if (d != 1000) seen_spread = true;
  }
  ASSERT_TRUE("200 次出现离散（非恒等）", seen_spread);

  cb.reset();
  return TEST_REPORT();
}
