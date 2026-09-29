#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace thin_agent {
class IHttpClient;
namespace agent {

/// 远程 Agent 描述。
struct AgentInfo {
  std::string id;          ///< 唯一标识
  std::string name;        ///< 显示名称
  std::string description; ///< 能力描述
  std::string endpoint;    ///< 通信地址（HTTP URL 或 "feishu:chat_id"）
  std::vector<std::string> capabilities;  ///< 能力标签
};

/// Agent 目录：管理已知的远程 Agent。
class AgentDirectory {
 public:
  static AgentDirectory& instance();

  void register_agent(const AgentInfo& info);
  void unregister_agent(const std::string& id);
  const AgentInfo* find(const std::string& id) const;
  std::vector<AgentInfo> list() const;
  std::vector<AgentInfo> find_by_capability(const std::string& capability) const;

 private:
  AgentDirectory() = default;
  std::unordered_map<std::string, AgentInfo> agents_;
};

/// 注册 delegate_to_agent 工具到 ToolRegistry。
/// 使用 HTTP POST 向远程 Agent 发送请求，或通过平台消息转发。
/// @param http  HTTP 客户端（注入，便于测试 mock）
void register_delegate_tool(
    IHttpClient& http,
    const std::string& platform = "http");  // "http" 或 "feishu"

}  // namespace agent
}  // namespace thin_agent
