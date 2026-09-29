// unit_http_observability：网关发送"失败可观测"统一判据（v0.53.83）
//
// 背景：R67/HHH2 给 FeishuAdapter 补可观测时**只改了 http_delete**，真正发消息的
// http_post 仍裸 curl_easy_perform（返回值全吞、无状态码、无日志）；同族还有
// DiscordAdapter::http_request、TelegramAdapter::http_get/post_json、
// WechatAdapter::http_post、im_gateway_v2::gw_http_post/delete（v1 早有 rc 捕获）。
// 故提为 include/thin_agent/gateway/HttpObservability.h 一处判据，四适配器+gw2 共用。
// 本测试锁住判据与文案契约（纯函数，不发真网）。
#include <curl/curl.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "thin_agent/gateway/HttpObservability.h"
#include "test_macros.h"

using thin_agent::gateway::http_send_failed;
using thin_agent::gateway::http_send_failure_message;

int main() {
  // ── 成功面：不得误报 ──
  ASSERT_TRUE("200 + OK 不算失败", !http_send_failed(CURLE_OK, 200));
  ASSERT_TRUE("204 + OK 不算失败", !http_send_failed(CURLE_OK, 204));
  ASSERT_TRUE("301 重定向不算失败（curl 默认不跟随，但非 4xx/5xx）",
              !http_send_failed(CURLE_OK, 301));
  ASSERT_EQ("成功时文案为空", http_send_failure_message("feishu", CURLE_OK, 200, "u"),
            std::string(""));

  // ── 失败面：传输错误与 4xx/5xx 都要被抓到 ──
  ASSERT_TRUE("传输错误算失败", http_send_failed(CURLE_COULDNT_CONNECT, 0));
  ASSERT_TRUE("超时算失败", http_send_failed(CURLE_OPERATION_TIMEDOUT, 0));
  ASSERT_TRUE("400 算失败", http_send_failed(CURLE_OK, 400));
  ASSERT_TRUE("404 算失败", http_send_failed(CURLE_OK, 404));
  ASSERT_TRUE("429 算失败（限流=消息被丢，必须可见）", http_send_failed(CURLE_OK, 429));
  ASSERT_TRUE("500 算失败", http_send_failed(CURLE_OK, 500));

  // ── 文案契约：含平台、判据值、URL；transport 与 http 两种形态可区分 ──
  const std::string t = http_send_failure_message(
      "discord", CURLE_COULDNT_CONNECT, 0, "https://discord/api/x");
  ASSERT_TRUE("transport 形态标注 platform", t.find("[discord]") != std::string::npos);
  ASSERT_TRUE("transport 形态标 transport=", t.find("transport=") != std::string::npos);
  ASSERT_TRUE("transport 形态带 url", t.find("https://discord/api/x") != std::string::npos);
  ASSERT_TRUE("transport 形态不含 http=", t.find("http=") == std::string::npos);

  const std::string h =
      http_send_failure_message("telegram", CURLE_OK, 429, "https://tg/api/y");
  ASSERT_TRUE("http 形态标注 platform", h.find("[telegram]") != std::string::npos);
  ASSERT_TRUE("http 形态带状态码", h.find("http=429") != std::string::npos);
  ASSERT_TRUE("http 形态带 url", h.find("https://tg/api/y") != std::string::npos);
  ASSERT_TRUE("http 形态不含 transport=", h.find("transport=") == std::string::npos);

  // ── 平台名缺省：nullptr/空串回落到 gateway（日志里至少能归属到网关）──
  const std::string n1 = http_send_failure_message(nullptr, CURLE_OK, 500, "u");
  const std::string n2 = http_send_failure_message("", CURLE_OK, 500, "u");
  ASSERT_TRUE("nullptr platform 回落 [gateway]",
              n1.find("[gateway]") != std::string::npos);
  ASSERT_TRUE("空 platform 回落 [gateway]", n2.find("[gateway]") != std::string::npos);

  // ── 源码面：不得再有裸 curl_easy_perform（网关发送单点收口）──
  {
    const char* kFiles[] = {"../src/gateway/FeishuAdapter.cpp",
                            "../src/gateway/DiscordAdapter.cpp",
                            "../src/gateway/TelegramAdapter.cpp",
                            "../src/gateway/WechatAdapter.cpp",
                            "../src/demo/im_gateway_v2.cpp"};
    int checked = 0;
    for (const char* f : kFiles) {
      std::ifstream in(f);
      if (!in.is_open()) continue;
      ++checked;
      std::string body((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
      const bool ok = body.find("curl_easy_perform(curl);") == std::string::npos;
      ASSERT_TRUE((std::string("无裸 perform: ") + f).c_str(), ok);
    }
    ASSERT_TRUE("源码面扫描至少命中一个文件（非 ctest 目录时不误报）", checked > 0);
  }

  return TEST_REPORT();
}
