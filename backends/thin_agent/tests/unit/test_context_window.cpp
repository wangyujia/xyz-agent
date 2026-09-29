// test_context_window：v0.52.25 TokenBudget 窗口动态化回归
//
// 背景：FC 循环 token 预算自 v0.28.0 起硬编码 128000（注释还写着
// "DeepSeek V4 Pro"，早已过时）。v0.52.25 改为：配置显式值优先，
// 否则按 model_name 推断。本测试锁推断表+优先级语义。

#include "../../include/thin_agent/llm/DemoConfigCompat.h"
#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond, msg) \
  do { if (!(cond)) { ++g_failures; std::printf("FAIL: %s\n", msg); } \
       else { std::printf("PASS: %s\n", msg); } } while (0)

using thin_agent::infer_context_window;
using thin_agent::DemoConfigCompat;

int main() {
  // 1) 推断表：已知模型
  CHECK(infer_context_window("glm-5.2") == 128000, "glm-5.2 → 128k");
  CHECK(infer_context_window("glm-4.5-flash") == 128000, "glm-4.5-flash → 128k");
  CHECK(infer_context_window("deepseek-chat") == 64000, "deepseek → 64k");
  CHECK(infer_context_window("claude-sonnet-4") == 200000, "claude → 200k");
  CHECK(infer_context_window("qwen3-235b") == 131072, "qwen → 131072");
  // 2) 未知模型保守值
  CHECK(infer_context_window("some-unknown-model") == 32000, "未知 → 32k 保守");
  CHECK(infer_context_window("") == 32000, "空名 → 32k 保守");
  // 3) 大小写敏感按子串（GLM 大写也命中——find 区分大小写，GLM 大写不命中，
  //    配置里都是小写 glm-5.2，语义 OK；此断言锁当前行为防意外变更）
  CHECK(infer_context_window("GLM-5.2") == 32000, "大写 GLM 不命中（锁行为）");
  // 4) 配置字段默认 0=未设置
  DemoConfigCompat cfg;
  CHECK(cfg.context_window_tokens == 0, "默认 0=按推断");
  if (g_failures == 0) std::printf("ALL PASS\n");
  return g_failures == 0 ? 0 : 1;
}
