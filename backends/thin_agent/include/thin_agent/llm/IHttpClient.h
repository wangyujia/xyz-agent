#pragma once

#include <string>

namespace thin_agent {

/// HTTP 请求的原始响应（不解析，不含业务逻辑）。
struct HttpResponse {
  int status_code = 0;       ///< HTTP 状态码（200/401/500...）
  std::string body;          ///< 响应正文
  bool ok = false;           ///< 网络层面是否成功（curl 返回 CURLE_OK）
  std::string error;         ///< curl 错误信息（ok=false 时填充）
};

/// HTTP 客户端抽象接口（依赖反转，解耦 curl）。
///
/// 生产实现：CurlHttpClient（封装 libcurl）。
/// 测试实现：MockHttpClient（注入预设响应）。
class IHttpClient {
 public:
  virtual ~IHttpClient() = default;

  /// 发送 HTTP POST 请求。
  /// @param url             完整的请求 URL
  /// @param body_json       请求体（JSON 字符串）
  /// @param auth_bearer     API key（默认 Authorization: Bearer；api_mode="anthropic_messages" 时用 x-api-key）
  /// @param api_mode        认证模式：""=Bearer，"anthropic_messages"=x-api-key（默认空）
  virtual HttpResponse post(const std::string& url,
                            const std::string& body_json,
                            const std::string& auth_bearer,
                            const std::string& api_mode = "") = 0;

  /// 发送 HTTP GET 请求。
  /// @param url             完整的请求 URL（含 query string）
  /// @param auth_bearer     Authorization: Bearer *** token（空表示不设）
  virtual HttpResponse get(const std::string& url,
                           const std::string& auth_bearer) = 0;
};

}  // namespace thin_agent
