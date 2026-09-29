/// WechatAdapter — 微信 iLink Bot (HTTP JSON long-poll)
///
/// 配置环境变量：WECHAT_BOT_ID, WECHAT_SECRET

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "thin_agent/api/PlatformAdapter.h"
#include "thin_agent/gateway/HttpObservability.h"  // v0.53.83: 发送可观测
#include "thin_agent/log/LogEvent.h"

namespace thin_agent {
namespace {

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

static std::string http_post(const std::string& url, const std::string& body,
                             int timeout_ms = 10000) {
  CURL* curl = curl_easy_init();
  if (!curl) return "";
  std::string response;
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  // v0.53.83: 统一观测（发送失败/轮询失败不再静默）
  thin_agent::gateway::curl_perform_observed(curl, "wechat", url);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  return response;
}

}  // namespace

class WechatAdapter : public PlatformAdapter {
 public:
  WechatAdapter() {
    const char* bot = std::getenv("WECHAT_BOT_ID");
    const char* sec = std::getenv("WECHAT_SECRET");
    if (bot) bot_id_ = bot;
    if (sec) secret_ = sec;
  }

  std::string name() const override { return "wechat"; }
  bool is_enabled() const override { return !bot_id_.empty(); }

  void connect(struct mg_mgr* /*mgr*/) override {
    if (bot_id_.empty() || poll_running_) return;
    stop_poll_ = false;
    poll_thread_ = std::thread(&WechatAdapter::poll_loop, this);
    poll_running_ = true;
    log_event("wechat", LogLevel::Info, "poll started");
  }

  void disconnect() override {
    stop_poll_ = true;
    if (poll_thread_.joinable()) poll_thread_.join();
    poll_running_ = false;
  }

  void on_timer() override {}

  void send_chat(const std::string& chat_id, const std::string& text,
                 const std::string& /*thread_id*/) override {
    send_message(chat_id, text);
  }

  void send_reply(const std::string& /*message_id*/,
                  const std::string& text) override {
    // 微信 iLink 不支持按 message_id 回复，使用 last_chat_id_
    send_message(last_chat_id_, text);
  }

  std::string add_reaction(const std::string&, const std::string&) override { return ""; }
  bool remove_reaction(const std::string&, const std::string&) override { return false; }
  bool is_connected() const override { return poll_running_; }

 private:
  void send_message(const std::string& chat_id, const std::string& text) {
    auto body = nlohmann::json{{"to", chat_id}, {"msgType", "text"}, {"content", text}};
    http_post("https://ilinkai.weixin.qq.com/ilink/bot/sendmessage", body.dump());
  }

  void poll_loop() {
    int64_t last_seq = 0;
    while (!stop_poll_) {
      if (bot_id_.empty()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        continue;
      }
      auto body = nlohmann::json{{"botId", bot_id_}, {"secret", secret_},
                                  {"seq", last_seq}, {"count", 10}};
      std::string resp = http_post("https://ilinkai.weixin.qq.com/ilink/bot/getupdates",
                                   body.dump());
      try {
        auto j = nlohmann::json::parse(resp);
        if (j.value("code", -1) != 0) {
          std::this_thread::sleep_for(std::chrono::seconds(5));
          continue;
        }
        for (auto& u : j.value("updates", nlohmann::json::array())) {
          last_seq = std::max(last_seq, static_cast<int64_t>(u.value("seq", 0)));
          if (u.value("msgType", "") != "text") continue;
          std::string text = u.value("content", "");
          if (!text.empty()) {
            last_chat_id_ = u.value("from", "");
            log_event("wechat", LogLevel::Info, "msg",
                      {{"text", text.substr(0, 80)}});
            forward_message(text, u.value("msgId", ""), u.value("from", ""), "");
          }
        }
      } catch (...) {}
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }

  std::string bot_id_;
  std::string secret_;
  std::string last_chat_id_;
  std::thread poll_thread_;
  bool poll_running_ = false;
  bool stop_poll_ = false;
};

}  // namespace thin_agent

namespace thin_agent {
std::unique_ptr<PlatformAdapter> create_wechat_adapter() {
  return std::make_unique<WechatAdapter>();
}
}  // namespace thin_agent
