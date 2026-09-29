#pragma once

#include <functional>
#include <string>

#include "thin_agent/llm/IHttpClient.h"

namespace thin_agent {

/// IHttpClient 的生产实现：封装 libcurl。
///
/// 用法：
///   CurlHttpClient http(15000);  // 15 秒超时
///   auto resp = http.post(url, body, token);
///   if (resp.ok) { ... resp.body ... }
class CurlHttpClient : public IHttpClient {
 public:
  /// @param timeout_ms 请求超时毫秒数（默认 15000）
  explicit CurlHttpClient(long timeout_ms = 15000);

  HttpResponse post(const std::string& url,
                    const std::string& body_json,
                    const std::string& auth_bearer,
                    const std::string& api_mode = "") override;

  HttpResponse get(const std::string& url,
                   const std::string& auth_bearer) override;

  /// v0.53.33: 流式 POST——响应体增量到达时喂 on_data（worker 线程内同步调，
  /// 复用方负责线程安全）。body 仍完整累积（兼容既有解析/重试路径）。
  HttpResponse post_streaming(const std::string& url,
                              const std::string& body_json,
                              const std::string& auth_bearer,
                              const std::string& api_mode,
                              std::function<void(const char*, size_t)> on_data);

 private:
  long timeout_ms_;
};

}  // namespace thin_agent
