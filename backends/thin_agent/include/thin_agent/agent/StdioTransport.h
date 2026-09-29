#pragma once

#include <map>
#include <string>
#include <vector>

#include "thin_agent/agent/ITransport.h"

namespace thin_agent {
namespace agent {

/// MCP 传输层：fork 子进程，通过 stdin/stdout 进行 JSON-RPC 通信。
///
/// 用法：
///   StdioTransport transport;
///   transport.connect("/usr/bin/some-mcp-server");
///   auto resp = transport.send(json_rpc_request);
///   transport.disconnect();
///
/// 子进程生命周期：connect() fork，disconnect() kill + waitpid。
class StdioTransport : public ITransport {
 public:
  StdioTransport() = default;
  ~StdioTransport() override;

  // v0.53.7: 市面 mcpServers 标准格式支持——connect 前注入。
  //   set_args():  command 的显式参数列表（市面标准 command+args 分离写法，
  //               如 command="npx", args=["-y","@modelcontextprotocol/
  //               server-filesystem","/tmp"]）。设置后 endpoint 空格切分
  //               不再生效——argv 完全由 command+args 构成，无 shell 引号歧义。
  //   set_env():  追加/覆盖子进程环境变量（如 GITHUB_TOKEN），
  //               未设置的变量继承父进程环境。
  void set_args(const std::vector<std::string>& args) { extra_args_ = args; }
  void set_env(const std::map<std::string, std::string>& env) { extra_env_ = env; }

  // ITransport
  bool connect(const std::string& endpoint) override;
  std::string send(const std::string& request) override;
  void disconnect() override;

 private:
#ifdef _WIN32
  void* child_handle_ = nullptr;  // v0.54.18: 子进程句柄（WaitForSingleObject/TerminateProcess）
#endif
  int child_pid_ = -1;
  int stdin_fd_ = -1;   // 写入子进程 stdin
  int stdout_fd_ = -1;  // 从子进程 stdout 读取
  std::vector<std::string> extra_args_;      ///< v0.53.7: 显式参数
  std::map<std::string, std::string> extra_env_;  ///< v0.53.7: 追加环境变量
};

}  // namespace agent
}  // namespace thin_agent
