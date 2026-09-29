#pragma once

#include <string>

namespace thin_agent {
namespace agent {

/// MCP 传输层抽象接口（依赖反转，解耦 fork/exec 和 curl）。
///
/// 生产实现：
///   StdioTransport — fork 子进程，通过 stdin/stdout 通信
///   HttpTransport   — HTTP POST JSON-RPC
///
/// 测试实现：MockTransport — 注入预设响应
class ITransport {
 public:
  virtual ~ITransport() = default;

  /// 建立传输连接。
  /// @param endpoint  stdio: 命令路径；http: 完整 URL
  virtual bool connect(const std::string& endpoint) = 0;

  /// 发送一条 JSON-RPC 请求并阻塞等待完整响应。
  /// @param request  完整的 JSON-RPC 请求字符串（含换行）
  /// @return  JSON-RPC 响应字符串（含换行）；失败时以 {"error":"..."} 返回
  virtual std::string send(const std::string& request) = 0;

  /// 断开传输连接。
  virtual void disconnect() = 0;
};

}  // namespace agent
}  // namespace thin_agent
