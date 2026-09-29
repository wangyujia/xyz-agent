// test_tools_fallback：v0.53.8 工具调用链多源 fallback 回归
//
// 背景：chat_completion_with_fallback（纯文本链）自 v0.47 起支持
// cloud_providers 逐源冗余，但 FC 主力链 chat_completion_with_tools
// 单源——GLM 熔断=复杂任务全歇。v0.53.8 补 tools_fallback。
//
// 测法：mock 注入点在 with_tools 内（THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE
// 首源即短路）。真失败路径用无 key 的 provider 逐源跳过语义验证 +
// 空列表零开销短路等价性。

#include "thin_agent/llm/CloudLlmClient.h"
#include "thin_agent/llm/IHttpClient.h"
#include "thin_agent/llm/DemoConfigCompat.h"

#include <cstdio>
#include <cstdlib>

using thin_agent::ChatMessage;
using thin_agent::CloudLlmClient;
using thin_agent::DemoConfigCompat;

static int g_failures = 0;
#define CHECK(cond, msg) \
  do { if (!(cond)) { ++g_failures; std::printf("FAIL: %s\n", msg); } \
       else { std::printf("PASS: %s\n", msg); } } while (0)

// 最小 http 桩（不应被真调——mock 环境变量先短路；无 key 源也不发请求）
struct NullHttp : thin_agent::IHttpClient {
  thin_agent::HttpResponse post(const std::string&, const std::string&,
                                const std::string&,
                                const std::string& = "") override {
    return {false, 0, "", "null_http_should_not_be_called"};
  }
  thin_agent::HttpResponse get(const std::string&, const std::string&) override {
    return {false, 0, "", "null_http_should_not_be_called"};
  }
};

int main() {
  NullHttp http;
  std::vector<ChatMessage> msgs{{"user", "hi"}};
  nlohmann::json tools = nlohmann::json::array();

  // 1) 空列表：零开销短路（与直调等价——mock 注入生效即证明走了 with_tools）
  {
    DemoConfigCompat cfg;
    cfg.mode = "cloud";
    cfg.provider = "openai-compatible";
    cfg.model_name = "glm-5.2";
    cfg.api_base = "https://example.invalid/v1";
    ::setenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE", "{\"ok_marker\":1}", 1);
    auto r = CloudLlmClient::chat_completion_with_tools_fallback(
        http, cfg, "k", msgs, tools);
    CHECK(r.ok && r.text.find("ok_marker") != std::string::npos,
          "空列表短路: mock 生效（等价直调）");
    ::unsetenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE");
  }

  // 2) 多源+mock：首源命中（mock 短路——逐源循环正常进入）
  {
    DemoConfigCompat cfg;
    cfg.mode = "cloud";
    thin_agent::CloudProviderConfig p1;
    p1.provider = "openai-compatible";
    p1.model_name = "glm-5.2";
    p1.api_base = "https://example.invalid/v1";
    p1.api_key_env = "TA_FB_KEY_A";
    cfg.cloud_providers.push_back(p1);
    ::setenv("TA_FB_KEY_A", "key-a", 1);
    ::setenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE", "{\"chain_hit\":1}", 1);
    auto r = CloudLlmClient::chat_completion_with_tools_fallback(
        http, cfg, "", msgs, tools);
    CHECK(r.ok && r.text.find("chain_hit") != std::string::npos,
          "多源首源: mock 命中");
    ::unsetenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE");
    ::unsetenv("TA_FB_KEY_A");
  }

  // 3) 全源无 key：逐源跳过 + all_providers_failed 汇总
  {
    DemoConfigCompat cfg;
    cfg.mode = "cloud";
    thin_agent::CloudProviderConfig p1, p2;
    p1.provider = "openai-compatible"; p1.model_name = "m1";
    p1.api_base = "https://a.invalid/v1"; p1.api_key_env = "TA_FB_MISSING_1";
    p2.provider = "openai-compatible"; p2.model_name = "m2";
    p2.api_base = "https://b.invalid/v1"; p2.api_key_env = "TA_FB_MISSING_2";
    cfg.cloud_providers.push_back(p1);
    cfg.cloud_providers.push_back(p2);
    ::unsetenv("TA_FB_MISSING_1");
    ::unsetenv("TA_FB_MISSING_2");
    auto r = CloudLlmClient::chat_completion_with_tools_fallback(
        http, cfg, "", msgs, tools);
    CHECK(!r.ok, "全源无 key: 失败");
    CHECK(r.error.find("all_providers_failed") != std::string::npos &&
              r.error.find("m1:no_key") != std::string::npos &&
              r.error.find("m2:no_key") != std::string::npos,
          "全源无 key: 逐源错误汇总（m1/m2:no_key）");
  }

  // 4) key 兜底语义：源自身 env 无 key → 全局 api_key_env 兜底
  {
    DemoConfigCompat cfg;
    cfg.mode = "cloud";
    cfg.api_key_env = "TA_FB_GLOBAL";
    ::setenv("TA_FB_GLOBAL", "gk", 1);
    thin_agent::CloudProviderConfig p1;
    p1.provider = "openai-compatible"; p1.model_name = "gm";
    p1.api_base = "https://c.invalid/v1"; p1.api_key_env = "TA_FB_MISSING_3";
    cfg.cloud_providers.push_back(p1);
    ::setenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE", "{\"key_fallback\":1}", 1);
    auto r = CloudLlmClient::chat_completion_with_tools_fallback(
        http, cfg, "", msgs, tools);
    CHECK(r.ok && r.text.find("key_fallback") != std::string::npos,
          "key 兜底: 全局 api_key_env 生效（未被 no_key 跳过）");
    ::unsetenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE");
    ::unsetenv("TA_FB_GLOBAL");
  }

  if (g_failures == 0) std::printf("ALL PASS\n");
  return g_failures == 0 ? 0 : 1;
}
