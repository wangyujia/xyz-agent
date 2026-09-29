#include "thin_agent/llm/CloudLlmClient.h"
#include "thin_agent/llm/LlmCircuitBreaker.h"
#include "thin_agent/llm/LlmRateLimiter.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

#include "thin_agent/core/ChatPolicy.h"
#include "thin_agent/llm/IHttpClient.h"
#include "thin_agent/llm/CurlHttpClient.h"
#include "thin_agent/llm/ProviderFactory.h"
#include "thin_agent/log/LogEvent.h"

// CloudLlmClient：curl 调用 OpenAI 兼容 chat/completions，供 AgentService 云策略顾问使用。

namespace thin_agent {

/// v0.53.33: SSE 增量解析器——curl 写回调喂原始段，完整 data: 行立即
/// 抽 delta.content 推 on_token（真逐 token：网络到达即转发，零攒批延迟）。
/// 工具调用片段不推（攒批解析器仍负责最终 tool_calls 组装）。
class SseIncrementalParser {
 public:
  // v0.53.38: on_reasoning——思考型模型 reasoning_content 增量透传
  // （此前只用来最终判错,过程内容丢弃——用户看不到真思考流）
  explicit SseIncrementalParser(
      std::function<void(const std::string&)> on_token,
      std::function<void(const std::string&)> on_reasoning = nullptr)
      : on_token_(std::move(on_token)), on_reasoning_(std::move(on_reasoning)) {}

  void feed(const char* data, size_t len) {
    buf_.append(data, len);
    // 按行拆（SSE 事件以 \n 分隔;跨网络段的不完整行留缓冲）
    size_t pos = 0;
    while (true) {
      const size_t nl = buf_.find('\n', pos);
      if (nl == std::string::npos) break;
      std::string line = buf_.substr(pos, nl - pos);
      pos = nl + 1;
      process_line(line);
    }
    buf_.erase(0, pos);
  }

 private:
  void process_line(const std::string& raw) {
    if (raw.empty() || raw[0] == '\r') return;
    if (raw.rfind("data: ", 0) != 0) return;
    const std::string data = raw.substr(6);
    if (data == "[DONE]") return;
    try {
      const auto chunk = nlohmann::json::parse(data);
      if (!chunk.contains("choices")) return;
      const auto& choices = chunk["choices"];
      if (!choices.is_array() || choices.empty()) return;
      const auto& delta = choices[0].value("delta", nlohmann::json::object());
      if (delta.contains("content") && delta["content"].is_string()) {
        const auto& tok = delta["content"].get<std::string>();
        if (!tok.empty() && on_token_) on_token_(tok);
      }
      // v0.53.38: reasoning_content / reasoning 双兼容（GLM/DS 与通用命名）
      for (const char* rk : {"reasoning_content", "reasoning"}) {
        if (delta.contains(rk) && delta[rk].is_string()) {
          const auto& rt = delta[rk].get<std::string>();
          if (!rt.empty() && on_reasoning_) on_reasoning_(rt);
          break;
        }
      }
    } catch (...) {
      // 非 JSON 行（心跳注释等）——忽略
    }
  }

  std::string buf_;
  std::function<void(const std::string&)> on_token_;
  std::function<void(const std::string&)> on_reasoning_;
};


namespace {

/// libcurl 写回调。
size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

/// 去除 URL 末尾斜杠。
std::string trim_trailing_slash(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

/// 按 provider 推断 chat/completions 完整 URL。
std::string infer_chat_endpoint(const DemoConfigCompat& cfg) {
  const auto provider = normalize_provider(cfg.provider);
  if (provider == "deepseek") {
    return "https://api.deepseek.com/chat/completions";
  }
  // v0.53.34: LM Studio 本地默认端点（api_base 可覆盖——远端机器上的实例）
  if (provider == "lmstudio" && cfg.api_base.empty()) {
    return "http://127.0.0.1:1234/v1/chat/completions";
  }
  if (!cfg.api_base.empty()) {
    return trim_trailing_slash(cfg.api_base) + "/chat/completions";
  }
  if (provider == "github-copilot") {
    return "https://api.githubcopilot.com/chat/completions";
  }
  return "";
}

}  // namespace

CloudChatResult CloudLlmClient::chat_completion(IHttpClient& http,
                                                const DemoConfigCompat& cfg,
                                                const std::string& api_key,
                                                const std::vector<ChatMessage>& messages,
                                                const char* mock_env_var,
                                                const nlohmann::json& response_format) {
  CloudChatResult out;

  // 单测/集成测试 mock 注入点（与 IHttpClient mock 二选一）
  const char* env_key = mock_env_var ? mock_env_var : "THIN_AGENT_TEST_CLOUD_RESPONSE";
  if (const char* mock_resp = std::getenv(env_key); mock_resp && *mock_resp) {
    out.ok = true;
    out.http_status = 200;
    out.text = mock_resp;
    return out;
  }

  const std::string endpoint = infer_chat_endpoint_for_mode(cfg);
  if (endpoint.empty()) {
    out.error = "missing_api_base";
    return out;
  }

  std::string api_key_effective = api_key;
  if (api_key_effective.empty()) {
    const char* v = std::getenv("THIN_AGENT_CLOUD_TEST_KEY");
    if (v && *v) api_key_effective = v;
  }

  std::string body;
  if (is_anthropic_mode(cfg)) {
    body = build_anthropic_request_body(messages, cfg.model_name, 4096,
                                        nlohmann::json::array());
  } else {
    body = build_request_body(messages, cfg.model_name,
                              cfg.max_completion_tokens > 0 ? cfg.max_completion_tokens : 2048,
                              response_format,
                              nlohmann::json::object(), false, cfg.vision);
  }

  // v0.52.6: 文本路径此前零重试单发（AgentLoop decompose/摘要等走此
  // 路径，GLM 间歇抖动直接失败）——补与 FC 路径同款重试+熔断+jitter。
  auto& breaker = LlmCircuitBreaker::instance();
  HttpResponse hr;
  static const int kMaxRetries = 3;
  static const int kRetryDelaysMs[] = {1000, 2000, 4000};
  for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
    // v0.52.22: 主动限流——令牌桶预节流（熔断管"坏了别打"，
    // 限流管"别打太快"；打穿 1302 窗口的根治）
    LlmRateLimiter::instance().acquire(endpoint);
    if (!breaker.allow_request(endpoint)) {
      hr.ok = false;
      hr.error = "circuit_open";
      break;
    }
    hr = http.post(endpoint, body, api_key_effective, cfg.api_mode);
    breaker.report(endpoint, hr.ok);
    if (hr.ok) {
      // v0.52.8: 语义空重试（与 FC 路径同款）
      if (is_anthropic_mode(cfg)) {
        out = parse_anthropic_response(hr.body, hr.status_code);
      } else {
        out = parse_response(hr.body, hr.status_code);
      }
      if (out.ok) return out;
      if (out.error == "empty_content_with_reasoning") {
        log_event("llm-retry", LogLevel::Warn, "retry",
                  {{"attempt", attempt + 1},
                   {"error", "empty_content_with_reasoning"},
                   {"status", hr.status_code}});
        continue;
      }
      // v0.52.19: 业务级限流同款（与 FC 路径一致——同族全扫）
      if (out.error.rfind("rate_limited:", 0) == 0) {
        breaker.report(endpoint, false);
        if (attempt < kMaxRetries) {
          static const int kRateLimitDelaysMs[] = {5000, 10000, 20000};
          long delay = jittered_delay_ms(kRateLimitDelaysMs[attempt]);
          log_event("llm-retry", LogLevel::Warn, "rate_limited retry",
                    {{"attempt", attempt + 1}, {"error", out.error},
                     {"retry_in_ms", delay}});
          std::this_thread::sleep_for(std::chrono::milliseconds(delay));
          continue;
        }
        return out;
      }
      return out;
    }
    if (hr.status_code >= 400 && hr.status_code < 500) break;
    if (attempt < kMaxRetries) {
      long delay = jittered_delay_ms(kRetryDelaysMs[attempt]);
      log_event("llm-retry", LogLevel::Warn, "retry",
                {{"attempt", attempt + 1}, {"error", hr.error},
                 {"status", hr.status_code},
                 {"retry_in_ms", delay}});
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
  }
  if (!hr.ok) {
    out.error = hr.error;
    // v0.53.81: 网络层失败也回填状态码——此前 http_status 恒 0，诊断面
    // 无法区分"连不上(0)/超时/被拒"，前端与 decision_trace 只见空 reason
    out.http_status = hr.status_code;
    return out;
  }

  if (is_anthropic_mode(cfg)) {
    return parse_anthropic_response(hr.body, hr.status_code);
  }
  return parse_response(hr.body, hr.status_code);
}

CloudChatResult CloudLlmClient::chat_completion_with_fallback(
    IHttpClient& http,
    const DemoConfigCompat& cfg,
    const std::vector<ChatMessage>& messages,
    const char* mock_env_var,
    const nlohmann::json& response_format) {
  // 单 provider 模式（cloud_providers 为空或未配置）
  if (cfg.cloud_providers.empty()) {
    std::string api_key;
    if (!cfg.api_key_env.empty()) {
      const char* v = std::getenv(cfg.api_key_env.c_str());
      if (v && *v) api_key = v;
    }
    return chat_completion(http, cfg, api_key, messages, mock_env_var, response_format);
  }

  // 多源 fallback：按顺序尝试
  CloudChatResult last_result;
  std::string error_summary;

  for (size_t i = 0; i < cfg.cloud_providers.size(); ++i) {
    const auto& p = cfg.cloud_providers[i];

    // 构造临时单 provider 配置
    DemoConfigCompat tmp_cfg;
    tmp_cfg.mode = "cloud";
    tmp_cfg.provider = p.provider;
    tmp_cfg.model_name = p.model_name;
    tmp_cfg.api_base = p.api_base;
    tmp_cfg.api_key_env = p.api_key_env;
    tmp_cfg.request_timeout_ms = p.request_timeout_ms > 0 ? p.request_timeout_ms : cfg.request_timeout_ms;
    tmp_cfg.fallback = "offline";
    tmp_cfg.api_mode = cfg.api_mode;  // inherit API protocol mode

    // 读取 API key
    std::string api_key;
    if (!p.api_key_env.empty()) {
      const char* v = std::getenv(p.api_key_env.c_str());
      if (v && *v) api_key = v;
    }
    if (api_key.empty()) {
      // 尝试 fallback 到全局 api_key_env
      if (!cfg.api_key_env.empty()) {
        const char* v = std::getenv(cfg.api_key_env.c_str());
        if (v && *v) api_key = v;
      }
    }

    if (api_key.empty() && tmp_cfg.api_base.empty()) {
      // 跳过没有密钥也没有 api_base 的 provider
      error_summary += "[" + p.model_name + ":no_key] ";
      continue;
    }

    auto result = chat_completion(http, tmp_cfg, api_key, messages, mock_env_var, response_format);
    if (result.ok) {
      // 标注胜出的 provider
      result.text = result.text;
      return result;
    }

    // 记录错误，继续下一个
    error_summary += "[" + p.model_name + ":" + result.error.substr(0, 60) + "] ";
    last_result = std::move(result);
  }

  // 全部失败
  last_result.error = "all_providers_failed: " + error_summary;
  return last_result;
}

CloudChatResult CloudLlmClient::chat_completion_with_tools(
    IHttpClient& http,
    const DemoConfigCompat& cfg,
    const std::string& api_key,
    const std::vector<ChatMessage>& messages,
    const nlohmann::json& tools,
    bool stream,
    std::vector<std::string>* out_stream_chunks,
    std::function<void(const std::string&)> on_token,
    std::function<void(const std::string&)> on_reasoning) {
  CloudChatResult out;
  (void)on_reasoning;  // 消费点在 SSE 构造(仅 stream+curl 真链路到达)

  // v0.52.3: 单测/集成测试 mock 注入点（与 with_fallback 同款模式；
  // AgentLoop/decompose 路径此前无 mock 点，分解链路只能真网验证）
  if (const char* mock_resp = std::getenv("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE");
      mock_resp && *mock_resp) {
    out.ok = true;
    out.http_status = 200;
    out.text = mock_resp;
    return out;
  }

  const std::string endpoint = infer_chat_endpoint_for_mode(cfg);
  if (endpoint.empty()) {
    out.error = "missing_api_base";
    return out;
  }

  std::string api_key_effective = api_key;
  if (api_key_effective.empty()) {
    const char* v = std::getenv("THIN_AGENT_CLOUD_TEST_KEY");
    if (v && *v) api_key_effective = v;
  }

  std::string body;
  if (is_anthropic_mode(cfg)) {
    body = build_anthropic_request_body(messages, cfg.model_name, 4096, tools);
  } else {
    body = build_request_body(messages, cfg.model_name,
                              cfg.max_completion_tokens > 0 ? cfg.max_completion_tokens : 2048,
                              nlohmann::json::object(), tools, stream, cfg.vision);
  }

  // v0.51.5: 超时修复——调用方默认构造的 CurlHttpClient 是 15s 硬编码，
  // cfg.request_timeout_ms 从未生效：编程类长请求（写数百行代码，GLM-5.2
  // 实测 >15s）必然 Timeout→重试三连→FC 空转→离线降级。
  // 优先复用注入的 http（mock/自定义实现不受影响）；生产 CurlHttpClient
  // 场景按配置超时重构（与 fast 分类 L247 同模式）。
  const int fc_timeout_ms = cfg.request_timeout_ms > 0 ? cfg.request_timeout_ms : 15000;

  // v0.29.0: 重试+指数退避（最多 3 次，仅 transient errors）
  // v0.52.6: +熔断（连续 3 次 transient 失败→endpoint 开闸冷却 30s
  // 指数至 2min，期间秒拒 circuit_open 不烧超时时长）+jitter（延迟
  // ±30% 打散并发同步撞车）。4xx 语义错误不重试也不计入熔断。
  // v0.52.8: "content 空+reasoning 非空"（思考型模型 reasoning 耗尽
  // 预算 → content 空）视为可重试 transient——见环内判定。
  auto& breaker = LlmCircuitBreaker::instance();
  HttpResponse hr;
  static const int kMaxRetries = 3;
  static const int kRetryDelaysMs[] = {1000, 2000, 4000};
  bool need_semantic_retry = false;  // v0.52.8: 200 但 content 空
  for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
    // v0.52.22: 主动限流——令牌桶预节流（熔断管"坏了别打"，
    // 限流管"别打太快"；打穿 1302 窗口的根治）
    LlmRateLimiter::instance().acquire(endpoint);
    if (!breaker.allow_request(endpoint)) {
      hr.ok = false;
      hr.error = "circuit_open";
      break;  // 熔断开——快速失败（别的 provider 不受影响，per-endpoint）
    }
    // 动态超时探测：注入对象非 curl 实现时退回原引用（依赖反转保持）
    // v0.53.33: on_token 非空 + curl → post_streaming 真逐 token
    //（非 curl=测试 mock：优雅降级零推送，攒批路径兜底）
    if (dynamic_cast<CurlHttpClient*>(&http) != nullptr) {
      CurlHttpClient timed_http(fc_timeout_ms);
      if (on_token && stream) {
        SseIncrementalParser ip(
            [on_token](const std::string& t) { on_token(t); },
            [on_reasoning](const std::string& r) {
              if (on_reasoning) on_reasoning(r);
            });
        hr = timed_http.post_streaming(
            endpoint, body, api_key_effective, cfg.api_mode,
            [&ip](const char* d, size_t n) { ip.feed(d, n); });
      } else {
        hr = timed_http.post(endpoint, body, api_key_effective, cfg.api_mode);
      }
    } else {
      hr = http.post(endpoint, body, api_key_effective, cfg.api_mode);
    }
    breaker.report(endpoint, hr.ok);
    if (hr.ok) {
      // v0.52.8: HTTP 200 但语义空（思考型 reasoning 耗尽预算 →
      // content 空）——预算波动是随机的，重试通常可拿到 content
      if (stream && out_stream_chunks) {
        out = parse_sse_stream(hr.body, out_stream_chunks);
        // v0.53.81: SSE 解析不携带状态码/错误体——非 SSE 错误响应
        // （429 限流 / 401 鉴权 / 5xx 的 JSON 错误体）此前被静默吞成
        // http_status=0 + error="" ；且 parse_sse_stream 末尾无条件
        // ok=true（无 data: 行也判成功）→ 上层只见"空回复"回退，
        // 诊断面完全失明（实测 GLM 5 小时额度耗尽 429：decision_trace
        // 只剩空 reason）。此处回填状态码、非 2xx/非 SSE 体判失败并留原文
        out.http_status = hr.status_code;
        if (hr.status_code >= 400 ||
            (out.text.empty() && !hr.body.empty() && hr.body.rfind("data:", 0) != 0)) {
          out.ok = false;
          if (out.error.empty())
            out.error = "non_sse_error_body: " + hr.body.substr(0, 300);
        }
      } else if (is_anthropic_mode(cfg)) {
        out = parse_anthropic_response(hr.body, hr.status_code);
      } else {
        out = parse_response(hr.body, hr.status_code);
      }
      if (out.ok) return out;
      if (out.error == "empty_content_with_reasoning") {
        need_semantic_retry = true;
        log_event("llm-retry", LogLevel::Warn, "retry",
                  {{"attempt", attempt + 1},
                   {"error", "empty_content_with_reasoning"},
                   {"status", hr.status_code}});
        continue;  // 语义空——不计熔断（endpoint 健康），直接重试
      }
      // v0.52.19: 业务级限流（200+error.code 1302/429）——长退避重试
      // +计熔断（持续限流=endpoint 过载，熔断冷却保护整进程）。
      // 退避 5s/10s/20s（限流窗口通常 1 分钟级，普通 1/2/4s 不够）。
      if (out.error.rfind("rate_limited:", 0) == 0) {
        breaker.report(endpoint, false);  // 计入熔断统计
        if (attempt < kMaxRetries) {
          static const int kRateLimitDelaysMs[] = {5000, 10000, 20000};
          long delay = jittered_delay_ms(kRateLimitDelaysMs[attempt]);
          log_event("llm-retry", LogLevel::Warn, "rate_limited retry",
                    {{"attempt", attempt + 1}, {"error", out.error},
                     {"retry_in_ms", delay}});
          std::this_thread::sleep_for(std::chrono::milliseconds(delay));
          continue;
        }
        return out;  // 限流重试耗尽——带 rate_limited 错误返回
      }
      return out;  // 其他解析错误原样返回
    }
    // 4xx 语义错误：不重试不计熔断（余额/鉴权类，重试与冷却均无意义）
    if (hr.status_code >= 400 && hr.status_code < 500) break;
    if (attempt < kMaxRetries) {
      long delay = jittered_delay_ms(kRetryDelaysMs[attempt]);
      log_event("llm-retry", LogLevel::Warn, "retry",
                {{"attempt", attempt + 1}, {"error", hr.error},
                 {"status", hr.status_code},
                 {"retry_in_ms", delay}});
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
  }
  if (!hr.ok) {
    out.error = hr.error;
    // v0.53.81: 网络层失败也回填状态码——此前 http_status 恒 0，诊断面
    // 无法区分"连不上(0)/超时/被拒"，前端与 decision_trace 只见空 reason
    out.http_status = hr.status_code;
    return out;
  }
  if (need_semantic_retry && !out.ok) {
    return out;  // 语义空重试耗尽——带 error 返回
  }
  if (stream && out_stream_chunks && out.text.empty()) {
    // v0.53.81: 收尾 SSE 回退路径同款——回填状态码+非 2xx/非 SSE 体判
    // 失败并留原文（与 L398 主路径同一缺陷的第二处，修一漏一）
    auto sse_out = parse_sse_stream(hr.body, out_stream_chunks);
    sse_out.http_status = hr.status_code;
    if (hr.status_code >= 400 ||
        (sse_out.text.empty() && !hr.body.empty() && hr.body.rfind("data:", 0) != 0)) {
      sse_out.ok = false;
      if (sse_out.error.empty())
        sse_out.error = "non_sse_error_body: " + hr.body.substr(0, 300);
    }
    return sse_out;
  }
  if (is_anthropic_mode(cfg) && out.text.empty()) {
    return parse_anthropic_response(hr.body, hr.status_code);
  }
  if (out.text.empty() && !out.ok) {
    return parse_response(hr.body, hr.status_code);
  }
  return out;
}

CloudLlmClient::IntentClassifyResult CloudLlmClient::classify_intent(
    IHttpClient& http,
    const DemoConfigCompat& cfg,
    const std::string& text,
    const std::string& lang) {
  IntentClassifyResult result;
  const auto t0 = std::chrono::steady_clock::now();

  // 构建 fast 配置
  auto fast_model = fast_path_config().model;
  DemoConfigCompat fast_cfg;
  fast_cfg.model_name = fast_model.empty() ? "glm-4.5-flash" : fast_model;  // 兜底
  fast_cfg.api_base = cfg.api_base;
  fast_cfg.request_timeout_ms = 5000;  // fast 分类 5 秒超时

  // 构建 messages
  std::string prompt = intent_classifier_prompt(lang);
  if (prompt.empty()) {
    prompt = intent_classifier_prompt("zh");  // fallback 中文
  }
  std::vector<ChatMessage> messages = {
      {"system", prompt},
      {"user", text}
  };

  // 通过 cloud_providers 中的 fast 配置调用
  std::string fast_provider;
  if (cfg.cloud_providers.size() >= 2 && !cfg.cloud_providers[1].model_name.empty()) {
    // cloud_providers[1] 是 fast 模型
    fast_cfg.model_name = cfg.cloud_providers[1].model_name;
    fast_cfg.api_base = cfg.cloud_providers[1].api_base;
    fast_provider = cfg.cloud_providers[1].provider;
  }

  // 强制 JSON 输出
  nlohmann::json resp_fmt = {{"type", "json_object"}};
  auto cloud_resp = chat_completion(http, fast_cfg, "", messages,
                                     "THIN_AGENT_TEST_CLOUD_INTENT_RESPONSE", resp_fmt);

  auto t1 = std::chrono::steady_clock::now();
  result.latency_ms = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
  result.model_used = fast_cfg.model_name;

  if (!cloud_resp.ok) {
    result.error = "classify_intent LLM call failed: " + cloud_resp.error;
    return result;
  }

  // 解析 JSON 响应
  try {
    std::string raw = cloud_resp.text;
    // 去掉可能的 markdown 包裹
    auto json_start = raw.find('{');
    if (json_start != std::string::npos) {
      raw = raw.substr(json_start);
    }
    auto parsed = nlohmann::json::parse(raw);
    result.intent = parsed.value("intent", "unknown");
    result.confidence = parsed.value("confidence", 0.0);
    result.slots = parsed.value("slots", nlohmann::json::object());
    result.ok = true;
  } catch (const std::exception& e) {
    result.error = std::string("classify_intent JSON parse error: ") + e.what();
    result.intent = "unknown";
  }

  return result;
}

std::vector<CloudLlmClient::ParsedToolCall> CloudLlmClient::parse_tool_calls(
    const std::string& response_json) {
  std::vector<ParsedToolCall> out;
  try {
    auto jr = nlohmann::json::parse(response_json);
    // v0.52.13: 断言加固——const operator[] 对缺失 key 直接 assert 崩
    //（真 e2e 实测：GLM 超时重试后残缺响应缺 message → 服务 SIGABRT，
    // 波次 38/39 永久 pending）。全部改守卫式访问。
    if (!jr.contains("choices") || !jr["choices"].is_array() ||
        jr["choices"].empty())
      return out;
    const auto& choices = jr["choices"];

    if (!choices[0].contains("message") ||
        !choices[0]["message"].is_object())
      return out;
    const auto& msg = choices[0]["message"];
    if (!msg.contains("tool_calls")) return out;
    const auto& tcs = msg["tool_calls"];
    if (!tcs.is_array()) return out;

    for (const auto& tc : tcs) {
      if (!tc.contains("function") || !tc["function"].is_object())
        continue;  // v0.52.13: 残缺 tool_call 条目跳过
      ParsedToolCall ptc;
      ptc.id = tc.value("id", "");
      ptc.name = tc["function"].value("name", "");

      // arguments 可能是字符串（需要解析）或已解析的对象
      if (!tc["function"].contains("arguments")) continue;  // v0.52.13
      const auto& args = tc["function"]["arguments"];
      if (args.is_string()) {
        try {
          ptc.arguments = nlohmann::json::parse(args.get<std::string>());
        } catch (...) {
          ptc.arguments = nlohmann::json::object();
        }
      } else if (args.is_object()) {
        ptc.arguments = args;
      }
      out.push_back(ptc);
    }
  } catch (...) {
  }
  return out;
}

// ══════════════════════════════════════════════════════════════════
// 可测试的纯函数（提取自 curl 实现）
// ══════════════════════════════════════════════════════════════════

std::string CloudLlmClient::build_request_body(
    const std::vector<ChatMessage>& messages,
    const std::string& model_name,
    int max_tokens,
    const nlohmann::json& response_format,
    const nlohmann::json& tools,
    bool stream,
    bool vision) {
  nlohmann::json body;
  body["model"] = model_name;
  body["max_tokens"] = max_tokens;
  body["messages"] = nlohmann::json::array();
  for (const auto& m : messages) {
    nlohmann::json msg;
    msg["role"] = m.role;
    if (m.role == "tool") {
      msg["tool_call_id"] = m.tool_call_id;
      if (!m.name.empty()) msg["name"] = m.name;
      msg["content"] = m.content;
    } else {
      // v0.39.0: vision 多模态支持 — content 变数组
      if (vision && !m.image_base64.empty() && (m.role == "user" || m.role == "system")) {
        nlohmann::json text_part;
        text_part["type"] = "text";
        text_part["text"] = m.content;

        nlohmann::json image_part;
        image_part["type"] = "image_url";
        image_part["image_url"]["url"] = "data:image/png;base64," + m.image_base64;

        msg["content"] = nlohmann::json::array({text_part, image_part});
      } else {
        msg["content"] = m.content;
      }
      if (m.role == "assistant" && m.tool_calls.is_array() && !m.tool_calls.empty()) {
        msg["tool_calls"] = m.tool_calls;
      }
    }
    body["messages"].push_back(msg);
  }
  if (!response_format.empty()) body["response_format"] = response_format;
  if (tools.is_array() && !tools.empty()) {
    body["tools"] = tools;
    body["tool_choice"] = "auto";
  }
  if (stream) body["stream"] = true;
  return body.dump();
}

std::string CloudLlmClient::build_anthropic_request_body(
    const std::vector<ChatMessage>& messages,
    const std::string& model_name,
    int max_tokens,
    const nlohmann::json& tools) {
  nlohmann::json body;
  body["model"] = model_name;
  body["max_tokens"] = max_tokens;

  // Anthropic: system is a top-level field
  for (const auto& m : messages) {
    if (m.role == "system") {
      std::string existing = body.value("system", "");
      body["system"] = existing.empty() ? m.content : existing + "\n" + m.content;
    }
  }

  body["messages"] = nlohmann::json::array();
  for (const auto& m : messages) {
    if (m.role == "system") continue;

    nlohmann::json msg;
    msg["role"] = (m.role == "tool") ? "user" : m.role;

    if (m.role == "tool") {
      msg["content"] = nlohmann::json::array({
        {{"type", "tool_result"},
         {"tool_use_id", m.tool_call_id},
         {"content", m.content}}
      });
    } else if (m.role == "assistant" && m.tool_calls.is_array() && !m.tool_calls.empty()) {
      nlohmann::json content_arr = nlohmann::json::array();
      if (!m.content.empty()) {
        content_arr.push_back({{"type", "text"}, {"text", m.content}});
      }
      for (const auto& tc : m.tool_calls) {
        // v0.52.13: 守卫——残缺 tool_call（缺 function/键）跳过，
        // const operator[] 断言崩溃防线
        if (!tc.contains("function") || !tc["function"].is_object() ||
            !tc["function"].contains("name") ||
            !tc["function"].contains("arguments"))
          continue;
        nlohmann::json tu;
        tu["type"] = "tool_use";
        tu["id"] = tc.value("id", "");
        tu["name"] = tc["function"]["name"];
        const auto& args = tc["function"]["arguments"];
        if (args.is_string()) {
          try { tu["input"] = nlohmann::json::parse(args.get<std::string>()); }
          catch (...) { tu["input"] = nlohmann::json::object(); }
        } else {
          tu["input"] = args;
        }
        content_arr.push_back(tu);
      }
      msg["content"] = content_arr;
    } else {
      msg["content"] = m.content;
    }

    body["messages"].push_back(msg);
  }

  // Convert OpenAI tool format → Anthropic: {name, description, input_schema}
  if (tools.is_array() && !tools.empty()) {
    nlohmann::json anthropic_tools = nlohmann::json::array();
    for (const auto& t : tools) {
      nlohmann::json at;
      at["name"] = t["function"]["name"];
      at["description"] = t["function"].value("description", "");
      at["input_schema"] = t["function"].value("parameters",
          nlohmann::json::object({{"type", "object"}}));
      anthropic_tools.push_back(at);
    }
    body["tools"] = anthropic_tools;
  }

  return body.dump();
}

CloudChatResult CloudLlmClient::parse_anthropic_response(const std::string& json_body,
                                          int http_status) {
  CloudChatResult out;
  out.http_status = http_status;

  nlohmann::json jr;
  try {
    jr = nlohmann::json::parse(json_body);
  } catch (const std::exception& e) {
    out.error = std::string("invalid_json_response: ") + e.what();
    return out;
  }

  if (http_status < 200 || http_status >= 300) {
    out.error = jr.contains("error") ? jr["error"].dump() : json_body;
    // v0.53.56: 认证失败显式打点——401/403 此前混在通用 error 里,
    /// 坏 key 排障要翻响应体;多 key pool 场景配合 mark_failed 轮转
    if (http_status == 401 || http_status == 403) {
      log_event("llm-auth", LogLevel::Error, "auth_failed",
                {{"status", http_status},
                 {"hint", "api key invalid/expired - check credential pool or env"}});
    }
    return out;
  }

  if (jr.value("type", "") == "error") {
    out.error = jr["error"].value("message", json_body);
    return out;
  }

  std::string text_content;
  nlohmann::json tool_calls = nlohmann::json::array();

  // v0.52.13: 守卫——Anthropic 错误体无 content（type=error 分支已拦，
  // 这里再防残缺体：content 缺失时 const operator[] 断言崩）
  if (!jr.contains("content")) {
    out.error = "missing_content";
    return out;
  }
  const auto& content = jr["content"];
  if (content.is_array()) {
    for (const auto& block : content) {
      std::string btype = block.value("type", "");
      if (btype == "text") {
        if (!text_content.empty()) text_content += "\n";
        text_content += block.value("text", "");
      } else if (btype == "tool_use") {
        nlohmann::json tc;
        tc["id"] = block.value("id", "");
        tc["type"] = "function";
        tc["function"]["name"] = block.value("name", "");
        tc["function"]["arguments"] = block.value("input", nlohmann::json::object()).dump();
        tool_calls.push_back(tc);
      }
    }
  }

  nlohmann::json combined;
  nlohmann::json msg;
  msg["role"] = "assistant";
  msg["content"] = text_content;
  if (!tool_calls.empty()) msg["tool_calls"] = tool_calls;
  combined["choices"] = nlohmann::json::array({{{"message", msg}}});
  out.text = combined.dump();
  out.ok = true;
  return out;
}

bool CloudLlmClient::is_anthropic_mode(const DemoConfigCompat& cfg) {
  return cfg.api_mode == "anthropic_messages";
}

std::string CloudLlmClient::infer_chat_endpoint_for_mode(const DemoConfigCompat& cfg) {
  if (is_anthropic_mode(cfg)) {
    return "https://api.anthropic.com/v1/messages";
  }
  return infer_chat_endpoint(cfg);
}

CloudChatResult CloudLlmClient::parse_response(const std::string& json_body,
                                                int http_status) {
  CloudChatResult out;
  out.http_status = http_status;

  // 清洗非法 UTF-8 字节（LLM API 偶尔返回含 null 字节或非法序列的响应）
  auto sanitize_utf8 = [](const std::string& in) -> std::string {
    std::string clean_out;
    clean_out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
      unsigned char c = static_cast<unsigned char>(in[i]);
      if (c == 0x00) {        // null byte
        i++;
        continue;
      }
      // ASCII (0x01-0x7F) — filter control chars except \t \n \r (JSON RFC 8259 §7)
      if (c < 0x80) {
        if (c >= 0x20 || c == 0x09 || c == 0x0A || c == 0x0D) {
          clean_out.push_back(static_cast<char>(c));
        }
        i++;
        continue;
      }
      // 2-byte UTF-8 (0xC0-0xDF)
      if (c >= 0xC2 && c <= 0xDF) {
        if (i + 1 < in.size() && (static_cast<unsigned char>(in[i+1]) & 0xC0) == 0x80) {
          clean_out.push_back(in[i]);
          clean_out.push_back(in[i+1]);
          i += 2;
          continue;
        }
        i++;
        continue;
      }
      // 3-byte UTF-8 (0xE0-0xEF)
      if (c >= 0xE0 && c <= 0xEF) {
        if (i + 2 < in.size() &&
            (static_cast<unsigned char>(in[i+1]) & 0xC0) == 0x80 &&
            (static_cast<unsigned char>(in[i+2]) & 0xC0) == 0x80) {
          clean_out.push_back(in[i]);
          clean_out.push_back(in[i+1]);
          clean_out.push_back(in[i+2]);
          i += 3;
          continue;
        }
        i++;
        continue;
      }
      // 4-byte UTF-8 (0xF0-0xF4)
      if (c >= 0xF0 && c <= 0xF4) {
        if (i + 3 < in.size() &&
            (static_cast<unsigned char>(in[i+1]) & 0xC0) == 0x80 &&
            (static_cast<unsigned char>(in[i+2]) & 0xC0) == 0x80 &&
            (static_cast<unsigned char>(in[i+3]) & 0xC0) == 0x80) {
          clean_out.push_back(in[i]);
          clean_out.push_back(in[i+1]);
          clean_out.push_back(in[i+2]);
          clean_out.push_back(in[i+3]);
          i += 4;
          continue;
        }
        i++;
        continue;
      }
      // 无效字节，跳过一次一个
      i++;
    }
    return clean_out;
  };

  nlohmann::json jr;
  try {
    jr = nlohmann::json::parse(json_body);
  } catch (const std::exception& e) {
    // 尝试 UTF-8 清洗后重试
    std::string cleaned = sanitize_utf8(json_body);
    bool retried = false;
    if (cleaned.size() != json_body.size()) {
      try {
        jr = nlohmann::json::parse(cleaned);
        retried = true;
      } catch (...) {}
    }
    // UTF-8 清洗不够 — 尝试允许异常字符 (nlohmann::json allow_exceptions=false)
    if (!retried) {
      try {
        jr = nlohmann::json::parse(cleaned, nullptr, false);
        if (jr.is_discarded()) {
          out.error = std::string("invalid_json_response: ") + e.what();
          return out;
        }
        retried = true;
      } catch (...) {
        out.error = std::string("invalid_json_response: ") + e.what();
        return out;
      }
    }
    if (!retried) {
      out.error = std::string("invalid_json_response: ") + e.what();
      return out;
    }
  }

  if (http_status < 200 || http_status >= 300) {
    out.error = jr.contains("error") ? jr["error"].dump() : json_body;
    // v0.53.56: 认证失败显式打点——401/403 此前混在通用 error 里,
    /// 坏 key 排障要翻响应体;多 key pool 场景配合 mark_failed 轮转
    if (http_status == 401 || http_status == 403) {
      log_event("llm-auth", LogLevel::Error, "auth_failed",
                {{"status", http_status},
                 {"hint", "api key invalid/expired - check credential pool or env"}});
    }
    return out;
  }

  // v0.52.19: HTTP 200 但 body 携带业务级限流码（GLM 1302"速率限制"
  // /OpenAI 429 语义同族）——真 e2e 实测密集并发时整轮波次打到限流，
  // 原先走 missing_choices 静默失败。标记 rate_limited 供调用层
  // 冷却重试（含熔断上报——持续限流=endpoint 过载）。
  if (jr.contains("error") && jr["error"].is_object() &&
      jr["error"].contains("code")) {
    std::string code_str;
    if (jr["error"]["code"].is_string())
      code_str = jr["error"]["code"].get<std::string>();
    else if (jr["error"]["code"].is_number_integer())
      code_str = std::to_string(jr["error"]["code"].get<int>());
    if (code_str == "1302" || code_str == "429" || code_str == "rate_limit" ||
        code_str == "rate_limit_exceeded") {
      out.error = "rate_limited:" + code_str;
      out.ok = false;
      return out;
    }
  }

  // v0.53.36: 用 contains() 防 const operator[] 抛异常（200 但无 choices
  // 的畸形响应——限流/错误已被上游拦截,此处为防御性编程）
  if (!jr.contains("choices") || !jr["choices"].is_array() ||
      jr["choices"].empty()) {
    out.error = "missing_choices";
    return out;
  }
  const auto& choices = jr["choices"];
  const auto& choice0 = choices[0];
  if (!choice0.contains("message") || !choice0["message"].is_object()) {
    out.error = "missing_choices_message";
    return out;
  }
  const auto& message = choice0["message"];

  // 优先检查 tool_calls（native function calling）
  if (message.contains("tool_calls") && message["tool_calls"].is_array() &&
      !message["tool_calls"].empty()) {
    nlohmann::json combined;
    combined["choices"] = nlohmann::json::array({nlohmann::json{
        {"message",
         {{"role", "assistant"},
          {"content", message.value("content", nlohmann::json())},
          {"tool_calls", message["tool_calls"]}}}}});
    out.text = combined.dump();
    out.ok = true;
    // v0.47.1: tool_calls 分支也解析 usage
    if (jr.contains("usage") && jr["usage"].is_object()) {
      const auto& u = jr["usage"];
      out.usage.prompt_tokens = u.value("prompt_tokens", 0);
      out.usage.completion_tokens = u.value("completion_tokens", 0);
      out.usage.total_tokens = u.value("total_tokens", 0);
    }
    return out;
  }

  const auto& content = message["content"];
  if (!content.is_string()) {
    out.error = "missing_choices_message_content";
    return out;
  }
  out.text = content.get<std::string>();

  // v0.47.1: 解析 usage（非流式响应的 usage 在 JSON 顶层）
  if (jr.contains("usage") && jr["usage"].is_object()) {
    const auto& u = jr["usage"];
    out.usage.prompt_tokens = u.value("prompt_tokens", 0);
    out.usage.completion_tokens = u.value("completion_tokens", 0);
    out.usage.total_tokens = u.value("total_tokens", 0);
  }

  // 推理模型可能把所有 token 消耗在 reasoning_content → 视为失败
  if (out.text.empty() && message.contains("reasoning_content") &&
      message["reasoning_content"].is_string() &&
      !message["reasoning_content"].get<std::string>().empty()) {
    out.ok = false;
    out.error = "empty_content_with_reasoning";
    return out;
  }

  out.ok = true;
  return out;
}

CloudChatResult CloudLlmClient::parse_sse_stream(
    const std::string& sse_text,
    std::vector<std::string>* out_chunks) {
  CloudChatResult out;

  std::istringstream iss(sse_text);
  std::string line;
  std::string accumulated_content;
  struct ToolCallState {
    std::string id;
    std::string name;
    std::string arguments;
  };
  std::map<int, ToolCallState> tc_states;
  bool has_tool_calls = false;

  while (std::getline(iss, line)) {
    if (line.empty() || line[0] == '\r') continue;
    if (line.rfind("data: ", 0) != 0) continue;
    std::string data = line.substr(6);
    if (data == "[DONE]") break;

    try {
      auto chunk = nlohmann::json::parse(data);
      if (out_chunks) out_chunks->push_back(data);

      // v0.47.1: 流式 usage 通常在最后一个 chunk（OpenAI 兼容规范）
      if (chunk.contains("usage") && chunk["usage"].is_object()) {
        const auto& u = chunk["usage"];
        out.usage.prompt_tokens = u.value("prompt_tokens", 0);
        out.usage.completion_tokens = u.value("completion_tokens", 0);
        out.usage.total_tokens = u.value("total_tokens", 0);
      }

      // v0.53.76: contains 前置——const operator[] 对缺失键是【断言崩】
      /// (实测 usage 尾帧 {"usage":{...}} 无 choices 字段=Abort;
      /// stream_options.include_usage 的标准尾帧必中;增量版 L58 与
      /// 非流式版 L541 均有守卫,唯此函数裸奔——修一漏一第 6 例)
      if (!chunk.contains("choices")) continue;
      const auto& choices = chunk["choices"];
      if (!choices.is_array() || choices.empty()) continue;

      const auto& delta = choices[0].value("delta", nlohmann::json::object());

      if (delta.contains("content") && delta["content"].is_string()) {
        accumulated_content += delta["content"].get<std::string>();
      }

      if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        has_tool_calls = true;
        for (const auto& tc : delta["tool_calls"]) {
          int idx = tc.value("index", 0);
          auto& st = tc_states[idx];

          if (tc.contains("id") && tc["id"].is_string())
            st.id = tc["id"].get<std::string>();
          if (tc.contains("function")) {
            const auto& fn = tc["function"];
            if (fn.contains("name") && fn["name"].is_string())
              st.name = fn["name"].get<std::string>();
            if (fn.contains("arguments") && fn["arguments"].is_string())
              st.arguments += fn["arguments"].get<std::string>();
          }
        }
      }
    } catch (...) {
      // 跳过不可解析的 chunk
    }
  }

  if (has_tool_calls && !tc_states.empty()) {
    nlohmann::json tool_calls_array = nlohmann::json::array();
    for (const auto& [idx, st] : tc_states) {
      tool_calls_array.push_back({
          {"id", st.id.empty() ? "call_" + std::to_string(idx) : st.id},
          {"type", "function"},
          {"function", {{"name", st.name}, {"arguments", st.arguments}}}});
    }
    nlohmann::json fake_resp;
    fake_resp["choices"] = nlohmann::json::array({nlohmann::json{
        {"message", {{"role", "assistant"},
                     {"content", accumulated_content},
                     {"tool_calls", tool_calls_array}}}}});
    out.text = fake_resp.dump();
  } else {
    out.text = accumulated_content;
  }
  out.ok = true;
  return out;
}


// v0.53.8: 工具调用链多源 fallback——与 with_fallback 同款逐源循环。
// mock 注入点在 with_tools 内（THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE），
// 测试注入时首源即返回，循环天然短路。
CloudChatResult CloudLlmClient::chat_completion_with_tools_fallback(
    IHttpClient& http,
    const DemoConfigCompat& cfg,
    const std::string& api_key,
    const std::vector<ChatMessage>& messages,
    const nlohmann::json& tools,
    bool stream,
    std::vector<std::string>* out_stream_chunks,
    std::function<void(const std::string&)> on_token,
    std::function<void(const std::string&)> on_reasoning) {
  // 单 provider 模式：零开销短路（与直调完全一致）
  if (cfg.cloud_providers.empty())
    return chat_completion_with_tools(http, cfg, api_key, messages, tools,
                                      stream, out_stream_chunks, on_token);

  CloudChatResult last_result;
  std::string error_summary;

  for (size_t i = 0; i < cfg.cloud_providers.size(); ++i) {
    const auto& p = cfg.cloud_providers[i];

    DemoConfigCompat tmp_cfg;
    tmp_cfg.mode = "cloud";
    tmp_cfg.provider = p.provider;
    tmp_cfg.model_name = p.model_name;
    tmp_cfg.api_base = p.api_base;
    tmp_cfg.api_key_env = p.api_key_env;
    tmp_cfg.request_timeout_ms =
        p.request_timeout_ms > 0 ? p.request_timeout_ms : cfg.request_timeout_ms;
    tmp_cfg.max_completion_tokens = cfg.max_completion_tokens;  // 逐源继承全局（CloudProviderConfig 无此字段）
    tmp_cfg.fallback = "offline";
    tmp_cfg.api_mode = cfg.api_mode;

    std::string key = api_key;
    if (key.empty() && !p.api_key_env.empty()) {
      if (const char* v = std::getenv(p.api_key_env.c_str()); v && *v) key = v;
    }
    if (key.empty() && !cfg.api_key_env.empty()) {
      if (const char* v = std::getenv(cfg.api_key_env.c_str()); v && *v) key = v;
    }
    if (key.empty()) {
      error_summary += "[" + p.model_name + ":no_key] ";
      continue;
    }

    std::vector<std::string> local_chunks;
    auto* chunks = out_stream_chunks ? out_stream_chunks : &local_chunks;
    auto result = chat_completion_with_tools(http, tmp_cfg, key, messages,
                                             tools, stream, chunks);
    if (result.ok) return result;

    error_summary += "[" + p.model_name + ":" + result.error.substr(0, 60) + "] ";
    last_result = std::move(result);
  }

  last_result.error = "all_providers_failed: " + error_summary;
  return last_result;
}

}  // namespace thin_agent
