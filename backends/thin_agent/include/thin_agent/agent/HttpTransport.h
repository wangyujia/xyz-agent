#pragma once

#include <string>

#include "thin_agent/agent/ITransport.h"

namespace thin_agent {
namespace agent {

/// MCP 传输层：通过 HTTP POST 发送 JSON-RPC 请求。
///
/// 用法：
///   HttpTransport transport;
///   transport.connect("http://localhost:8080/mcp");
///   auto resp = transport.send(json_rpc_request);
///
/// 依赖 libcurl（通过 IHttpClient 接口解耦，或直接内嵌 curl）。
class HttpTransport : public ITransport {
 public:
  HttpTransport() = default;
  ~HttpTransport() override = default;

  // ITransport
  bool connect(const std::string& endpoint) override;
  std::string send(const std::string& request) override;
  void disconnect() override;

 private:
  std::string endpoint_;
  bool connected_ = false;
};

}  // namespace agent
}  // namespace thin_agent
