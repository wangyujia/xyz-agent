// test_web_search_config：v0.53.10 网页搜索多源 + 运行时配置回归
//
// 背景：web_search 数据源此前为 mock（agent 无真实网络信息获取能力）。
// v0.53.10：①Bing html 适配器（免费无 key，b_algo 正则解析）
// ②fetch_search "auto"=读 chat_policy.json web_search.provider
// ③WS web_search_config 运行时切换+落盘。
//
// 本单测：Bing html 解析（真页面 fixture）/auto 配置读取/实体解码。
// 网络依赖部分（真 Bing 请求+WS 消息）由 e2e 覆盖。

#include "thin_agent/core/ExternalInfoClient.h"
#include "thin_agent/core/ChatPolicy.h"
#include "thin_agent/llm/IHttpClient.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace thin_agent;

static int g_failures = 0;
#define CHECK(cond, msg) \
  do { if (!(cond)) { ++g_failures; std::printf("FAIL: %s\n", msg); } \
       else { std::printf("PASS: %s\n", msg); } } while (0)

/// 固定 http：返回预置 Bing 结果页 fixture（含 2 条 b_algo + 噪音块）
class FixtureHttp : public IHttpClient {
 public:
  HttpResponse post(const std::string&, const std::string&,
                    const std::string&, const std::string& = "") override {
    return {0, "", false, "fixture_post_unused"};
  }
  HttpResponse get(const std::string& url, const std::string&) override {
    HttpResponse r;
    r.ok = true;
    r.status_code = 200;
    // 真 Bing 页面结构（抓取样例的精简保真版）：两条真结果+一条非 b_algo 噪音
    r.body =
        "<html><body>"
        "<li class=\"b_algo\"><h2><a href=\"https://example.com/cpp-agent\">"
        "C++ Agent Framework &amp; Tools</a></h2>"
        "<p>A lightweight &lt;agent&gt; framework for embedded devices.</p></li>"
        "<li class=\"b_algo\"><h2><a href=\"https://example.org/llm\">"
        "LLM Integration Guide</a></h2>"
        "<p>How to integrate function&#39;calling in C++.</p></li>"
        "<li class=\"b_ad\"><h2>广告块不应命中</h2></li>"
        "</body></html>";
    last_url_ = url;
    return r;
  }
  std::string last_url_;
};

int main() {
  // ── 1) Bing 适配器：解析+实体解码+噪音过滤 ──
  {
    FixtureHttp http;
    auto r = ExternalInfoClient::fetch_search(http, "cpp agent", "zh", "bing");
    CHECK(r.ok, "bing: ok");
    CHECK(r.source == "bing", "bing: source 标识");
    CHECK(r.http_status == 200, "bing: http_status");
    CHECK(http.last_url_.find("bing.com/search") != std::string::npos &&
          http.last_url_.find("mkt=zh-CN") != std::string::npos,
          "bing: URL 构造（zh→zh-CN）");
    const auto& results = r.data["results"];
    CHECK(results.size() == 2, "bing: 恰好 2 条（广告块过滤）");
    if (results.size() == 2) {
      CHECK(results[0]["title"] == "C++ Agent Framework & Tools",
            "bing: 实体解码 &amp;→&");
      CHECK(results[0]["snippet"].get<std::string>().find("<agent>") != std::string::npos,
            "bing: 实体解码 &lt;agent&gt;");
      CHECK(results[1]["snippet"].get<std::string>().find("function'calling") != std::string::npos,
            "bing: 实体解码 &#39;");
      CHECK(results[0]["url"] == "https://example.com/cpp-agent", "bing: url 提取");
    }
  }
  // ── 2) lang=en 区域参数 ──
  {
    FixtureHttp http;
    ExternalInfoClient::fetch_search(http, "cpp", "en", "bing");
    CHECK(http.last_url_.find("mkt=en-US") != std::string::npos,
          "bing: en→en-US 区域");
  }
  // ── 3) auto 模式：读 chat_policy web_search.provider ──
  {
    // 写临时 policy 文件 → 环境变量指向 → reset 缓存
    const char* tpl =
        "{\"web_search\": {\"provider\": \"mock\"}}";
    std::ofstream("/tmp/ta_ws_policy.json") << tpl;
    setenv("THIN_AGENT_CHAT_POLICY_PATH", "/tmp/ta_ws_policy.json", 1);
    chat_policy_reset();
    FixtureHttp http;
    // provider="auto" → 应读到 mock（mock 分支恒 ok）
    auto r = ExternalInfoClient::fetch_search(http, "q", "zh", "auto");
    CHECK(r.source == "mock", "auto: 读 chat_policy provider=mock");
    // 改盘 → reset → 生效（运行时切换语义）
    std::ofstream("/tmp/ta_ws_policy.json")
        << "{\"web_search\": {\"provider\": \"bing\"}}";
    chat_policy_reset();
    auto r2 = ExternalInfoClient::fetch_search(http, "q", "zh", "auto");
    CHECK(r2.source == "bing", "auto: 盘改+reset 后切到 bing");
  }
  // ── 3.5) v0.53.15: 数字型 provider 不崩（此前 value<string> 抛 type_error
  //        逃逸连接层——无响应无错误帧，真审计实测 provider:123 复现）──
  // （该行为在 WS handler 层；此处验证 ExternalInfoClient 对非常规
  //  provider 串的兜底语义不变：未知串≠崩，走 bing/词表外按空处理）
  {
    FixtureHttp http;
    auto r = ExternalInfoClient::fetch_search(http, "q", "zh", "123");
    // 现有语义：未知 provider 词表值落 mock fallback（保守不炸网不误打网）。
    // 本断言守住的契约=不崩、有确定 source（WS 层的 type_error 已另修）。
    CHECK(r.ok && !r.source.empty(), "数字串 provider: 不崩且有确定 source（mock 兜底）");
  }

  // ── 4) 空 provider 兜底 bing ──
  {
    unsetenv("THIN_AGENT_CHAT_POLICY_PATH");
    chat_policy_reset();
    FixtureHttp http;
    auto r = ExternalInfoClient::fetch_search(http, "q", "zh", "");
    CHECK(r.source == "bing", "空 provider: 兜底 bing");
  }

  ::remove("/tmp/ta_ws_policy.json");
  if (g_failures == 0) std::printf("ALL PASS\n");
  return g_failures == 0 ? 0 : 1;
}
