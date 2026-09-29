/// TelegramAdapter — Telegram Bot API (HTTP long-poll)
///
/// 通过 getUpdates 轮询消息 → sendMessage 回复。
/// 配置环境变量：TELEGRAM_BOT_TOKEN

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <atomic>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "thin_agent/api/PlatformAdapter.h"
#include "thin_agent/gateway/HttpObservability.h"  // v0.53.83: 发送可观测
#include "thin_agent/log/LogEvent.h"

namespace thin_agent {
namespace {

// curl 写回调
static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

// HTTP GET
static std::string http_get(const std::string& url, int timeout_ms = 10000) {
  CURL* curl = curl_easy_init();
  if (!curl) return "";
  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  // v0.53.83: 统一观测（发送失败/轮询失败不再静默）
  thin_agent::gateway::curl_perform_observed(curl, "telegram", url);
  curl_easy_cleanup(curl);
  return response;
}

// HTTP POST (JSON body)
static std::string http_post_json(const std::string& url,
                                  const nlohmann::json& body,
                                  int timeout_ms = 10000) {
  CURL* curl = curl_easy_init();
  if (!curl) return "";
  std::string response;
  std::string body_str = body.dump();
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  // v0.53.83: 统一观测（发送失败/轮询失败不再静默）
  thin_agent::gateway::curl_perform_observed(curl, "telegram", url);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  return response;
}

}  // namespace

class TelegramAdapter : public PlatformAdapter {
 public:
  TelegramAdapter() {
    const char* token = std::getenv("TELEGRAM_BOT_TOKEN");
    if (token) bot_token_ = token;
    poll_thread_running_ = false;
    stop_polling_ = false;
  }

  std::string name() const override { return "telegram"; }
  bool is_enabled() const override { return !bot_token_.empty(); }

  void connect(struct mg_mgr* /*mgr*/) override {
    if (bot_token_.empty()) return;
    if (poll_thread_running_) return;

    // 启动轮询线程
    stop_polling_ = false;
    poll_thread_ = std::thread(&TelegramAdapter::poll_loop, this);
    poll_thread_running_ = true;
    log_event("telegram", LogLevel::Info, "poll started");
  }

  void disconnect() override {
    stop_polling_ = true;
    if (poll_thread_.joinable()) poll_thread_.join();
    poll_thread_running_ = false;
  }

  void on_timer() override {
    // Telegram long-poll 在独立线程中运行，无需 timer 维护
  }

  void send_chat(const std::string& chat_id,
                 const std::string& text,
                 const std::string& /*thread_id*/) override {
    send_message(chat_id, text);
  }

  void send_reply(const std::string& message_id,
                  const std::string& text) override {
    // Telegram Bot API 不支持按 message_id 回复（不同于飞书）
    // 使用 reply_to_message_id 参数实现引用回复
    nlohmann::json body;
    body["chat_id"] = last_chat_id_;
    body["text"] = text;
    if (!message_id.empty()) {
      try {
        body["reply_to_message_id"] = std::stoll(message_id);
      } catch (...) {}
    }
    std::string url = api_base() + "/sendMessage";
    http_post_json(url, body);
  }

  std::string add_reaction(const std::string& /*message_id*/,
                           const std::string& /*emoji*/) override {
    // Telegram Bot API 不直接支持 reactions（需要 premium 或特定方法）
    return "";
  }

  bool remove_reaction(const std::string& /*message_id*/,
                       const std::string& /*reaction_id*/) override {
    return false;
  }

  bool is_connected() const override { return poll_thread_running_; }

 private:
  std::string api_base() const {
    return "https://api.telegram.org/bot" + bot_token_;
  }

  void send_message(const std::string& chat_id, const std::string& text) {
    nlohmann::json body;
    body["chat_id"] = chat_id;
    // v0.53.52: 移除 parse_mode=HTML——文本未经 HTML 转义,用户消息含
    /// <tag> 时 Telegram API 返回 400(回复静默丢)。纯文本发送最稳
    body["text"] = text;
    std::string url = api_base() + "/sendMessage";
    std::string resp = http_post_json(url, body);
    log_event("telegram", LogLevel::Info, "sendMessage",
              {{"resp", resp.substr(0, 80)}});
  }

  void poll_loop() {
    int64_t last_update_id = 0;

    while (!stop_polling_) {
      std::string url = api_base() + "/getUpdates"
                        "?timeout=10"
                        "&offset=" + std::to_string(last_update_id + 1) +
                        "&allowed_updates=[\"message\"]";

      std::string resp = http_get(url, 15000);
      if (resp.empty()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }

      try {
        auto j = nlohmann::json::parse(resp);
        if (!j.value("ok", false)) {
          log_event("telegram", LogLevel::Error, "getUpdates error",
                    {{"resp", resp.substr(0, 120)}});
          std::this_thread::sleep_for(std::chrono::seconds(2));
          continue;
        }

        for (const auto& upd : j["result"]) {
          last_update_id = std::max(last_update_id,
                                    static_cast<int64_t>(upd.value("update_id", 0LL)));

          if (!upd.contains("message")) continue;
          const auto& msg = upd["message"];
          if (!msg.contains("text")) continue;

          std::string text = msg["text"].get<std::string>();
          std::string message_id = std::to_string(msg["message_id"].get<int64_t>());
          std::string chat_id = std::to_string(msg["chat"]["id"].get<int64_t>());

          // 保存 chat_id 供 send_reply 使用
          last_chat_id_ = chat_id;

          if (!text.empty()) {
            log_event("telegram", LogLevel::Info, "msg",
                      {{"text", text.substr(0, 80)}});
            forward_message(text, message_id, chat_id, "");
          }
        }
      } catch (const std::exception& e) {
        log_event("telegram", LogLevel::Error, "parse error",
                  {{"what", std::string(e.what())}});
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }

  std::string bot_token_;
  std::string last_chat_id_;  // 记录最近 chat_id 用于 reply(定性:仅
                              /// reply 路径使用,gw 主路径走 send_chat 显式
                              /// chat_id;多会话 reply 竞态面=低危,Telegram
                              /// 侧 reply 未接线)
  std::thread poll_thread_;
  std::atomic<bool> poll_thread_running_{false};  // v0.53.52: 跨线程标志
  std::atomic<bool> stop_polling_{false};         /// atomic 化(裸 bool=UB 面)
};

}  // namespace thin_agent

namespace thin_agent {
std::unique_ptr<PlatformAdapter> create_telegram_adapter() {
  return std::make_unique<TelegramAdapter>();
}
}  // namespace thin_agent
