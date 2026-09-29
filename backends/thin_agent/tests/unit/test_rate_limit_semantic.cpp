// test_rate_limit_semantic：v0.52.19 业务级限流识别回归
//
// 背景：GLM 1302"速率限制"以 HTTP 200+body.error.code 形式返回，
// 原先 parse 走 missing_choices 静默失败——密集并发（3+ 并发波次）
// 打穿限流时整轮任务降级离线兜底。现识别为 rate_limited:* 并
// 走 5/10/20s 长退避+熔断上报。
//
// 验证（parse 层语义）：
// 1. 200+1302（字符串码）→ rate_limited:1302
// 2. 200+429（数字码）→ rate_limited:429
// 3. 200+其他业务错误（如 1113 余额）→ 不误判限流（missing_choices
//    或原错误语义——余额类不该长退避）
// 4. 200+正常 choices → 不受影响
#include "test_macros.h"

#include "thin_agent/llm/CloudLlmClient.h"

using namespace thin_agent;

// 复刻 parse_response 的限流识别段（与生产同式）
static std::string classify(const std::string& body) {
  auto out = CloudLlmClient::parse_response(body, 200);
  return out.ok ? "ok" : out.error;
}

int main() {
  // 1) GLM 1302 字符串码
  {
    auto e = classify(R"({"error":{"code":"1302","message":"速率限制"}})");
    ASSERT_TRUE("1302 识别为限流", e == "rate_limited:1302");
  }
  // 2) 数字码 429
  {
    auto e = classify(R"({"error":{"code":429,"message":"Too Many Requests"}})");
    ASSERT_TRUE("数字 429 识别为限流", e == "rate_limited:429");
  }
  // 3) 其他业务错误不误判（1113 余额类）
  {
    auto e = classify(R"({"error":{"code":"1113","message":"余额不足"}})");
    ASSERT_TRUE("余额错误不误判限流", e != "rate_limited:1113");
  }
  // 4) 正常响应不受影响
  {
    auto e = classify(
        R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})");
    ASSERT_TRUE("正常响应 ok", e == "ok");
  }

  return TEST_REPORT();
}
