#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {

/// v0.44.0: Webhook 客户端 — HMAC 签名 + 多平台投递。
///
/// 加载配置 ~/.thin_agent/config/webhooks.json：
/// ```json
/// [
///   {"name":"monitor","url":"https://hooks.example.com/hook","secret":"hmac-key"},
///   {"name":"slack","url":"https://hooks.slack.com/services/xxx","secret":""}
/// ]
/// ```
///
/// deliver_to 格式：
///   "webhook:monitor" → 发往配置中名为 monitor 的 webhook
///   "webhook:https://url?secret=xxx" → 直接发往 URL（自动 HMAC 签名）
class WebhookClient {
 public:
  /// 从 JSON 文件加载配置。文件不存在静默跳过。
  bool load(const std::string& path);
  bool load_json(const nlohmann::json& j);

  /// 发送到注册的 webhook。body 会被 HMAC 签名后 POST。
  /// 返回 {success, status_code, response_body, signature}.
  nlohmann::json send(const std::string& name, const std::string& body);

  /// 直接发送到指定 URL。secret 为空时不签名。
  nlohmann::json send_direct(const std::string& url,
                             const std::string& secret,
                             const std::string& body);

  /// 列出所有已配置的 webhook（隐藏 secret）。
  nlohmann::json list() const;

  /// webhook 测试工具（供 LLM 调用）。
  nlohmann::json test(const std::string& name, const std::string& body);

 private:
  struct Target {
    std::string name;
    std::string url;
    std::string secret;
    bool enabled{true};
  };
  mutable std::mutex mu_;
  std::vector<Target> targets_;

  /// HTTP POST + 签名头部。
  nlohmann::json post_signed(const std::string& url,
                             const std::string& signature,
                             const std::string& body);
};

}  // namespace thin_agent
