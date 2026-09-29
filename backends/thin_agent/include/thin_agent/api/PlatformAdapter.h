#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct mg_mgr;
struct mg_connection;

namespace thin_agent {

/// PlatformAdapter — IM 平台抽象接口
///
/// 每个平台（飞书/微信/Telegram/Discord 等）实现此接口，由 Gateway 统一管理。
/// Gateway 不再关心平台协议细节，只通过此接口收发消息。
class PlatformAdapter {
 public:
  virtual ~PlatformAdapter() = default;

  // ── 标识 ──

  /// 平台名称（"feishu", "telegram", "wechat", ...），用于日志和 pending 匹配
  virtual std::string name() const = 0;

  /// 是否已配置凭据，未配置时 Gateway 跳过连接
  virtual bool is_enabled() const = 0;

  // ── 生命周期 ──

  /// 建立平台连接（WebSocket / long-poll 等）
  virtual void connect(struct mg_mgr* mgr) = 0;

  /// 断开连接并清理资源
  virtual void disconnect() = 0;

  /// 周期性维护（心跳、重连、long-poll 迭代）
  virtual void on_timer() = 0;

  // ── 消息发送 ──

  /// 发送文本消息到指定 chat/用户
  virtual void send_chat(const std::string& chat_id,
                         const std::string& text,
                         const std::string& thread_id = "") = 0;

  /// 回复特定消息（如飞书的 msg reply API）
  virtual void send_reply(const std::string& message_id,
                          const std::string& text) = 0;

  /// 添加 reaction（返回 reaction_id，失败返回空字符串）
  virtual std::string add_reaction(const std::string& message_id,
                                   const std::string& emoji) = 0;

  /// 移除 reaction（返回是否成功）
  virtual bool remove_reaction(const std::string& message_id,
                               const std::string& reaction_id) = 0;

  // ── 回调 ──

  /// Gateway 注册此回调，平台收到用户消息时调用
  using MessageCallback =
      std::function<void(const std::string& text,       // 用户消息文本
                         const std::string& message_id,  // 平台消息 ID
                         const std::string& chat_id,     // 会话 ID
                         const std::string& thread_id,   // 话题 ID（空为无话题）
                         PlatformAdapter* platform       // 指向自己，供 Gateway 回找
                         )>;

  void set_message_callback(MessageCallback cb) { on_message_ = std::move(cb); }

  // ── 连接状态 ──

  /// 是否有活跃连接
  virtual bool is_connected() const { return false; }

 protected:
  /// 子类通过此回调将收到的消息转发给 Gateway
  void forward_message(const std::string& text,
                       const std::string& message_id,
                       const std::string& chat_id,
                       const std::string& thread_id) {
    if (on_message_) on_message_(text, message_id, chat_id, thread_id, this);
  }

  MessageCallback on_message_;
};

}  // namespace thin_agent
