/// DiscordAdapter — Discord Bot API (Gateway WebSocket + REST)
///
/// 通过 Discord Gateway WebSocket 接收事件，REST API 发送消息。
/// 配置环境变量：DISCORD_BOT_TOKEN
///
/// 协议参考：https://discord.com/developers/docs

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "mongoose.h"

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

static size_t curl_header_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

// HTTP GET/POST/PUT/DELETE helpers
static std::string http_request(const std::string& method,
                                const std::string& url,
                                const std::string& body = "",
                                const std::string& auth_token = "",
                                int timeout_ms = 10000) {
  CURL* curl = curl_easy_init();
  if (!curl) return "";
  std::string response;
  struct curl_slist* hdrs = nullptr;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  if (!auth_token.empty()) {
    hdrs = curl_slist_append(hdrs, ("Authorization: Bot " + auth_token).c_str());
  }

  if (method == "POST") {
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  } else if (method == "PUT") {
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  } else if (method == "DELETE") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  }

  if (hdrs) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  }

  // v0.53.83: 统一观测（R67 已鉴定 Discord send 为同款待修——返回值全吞，
  // 消息丢失零感知）
  thin_agent::gateway::curl_perform_observed(curl, "discord", url);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  return response;
}

// ══════════════════════════════════════════════════════
// DiscordAdapter
// ══════════════════════════════════════════════════════

class DiscordAdapter : public PlatformAdapter {
 public:
  std::string name() const override { return "discord"; }

  bool is_enabled() const override {
    const char* token = std::getenv("DISCORD_BOT_TOKEN");
    return token && token[0];
  }

  bool is_connected() const override { return ws_connected_; }

  // ── 生命周期 ──

  void connect(struct mg_mgr* mgr) override {
    const char* token = std::getenv("DISCORD_BOT_TOKEN");
    if (!token || !token[0]) {
      log_event("discord", LogLevel::Error, "DISCORD_BOT_TOKEN not set");
      return;
    }
    if (!mgr) {
      log_event("discord", LogLevel::Error, "mg_mgr is null");
      return;
    }
    bot_token_ = token;

    // 获取 Gateway URL
    std::string gw_resp = http_request("GET",
        "https://discord.com/api/v10/gateway", "", bot_token_);
    try {
      auto gw_json = nlohmann::json::parse(gw_resp);
      gateway_url_ = gw_json.value("url", "wss://gateway.discord.gg");
    } catch (...) {
      gateway_url_ = "wss://gateway.discord.gg";
    }

    // 连接 Gateway WebSocket
    std::string ws_url = gateway_url_ + "/?v=10&encoding=json";
    mgr_ = mgr;
    ws_connect(ws_url);
    log_event("discord", LogLevel::Info, "connecting", {{"url", ws_url}});
  }

  void disconnect() override {
    ws_connected_ = false;
    heartbeat_running_ = false;
    // v0.53.52: join 心跳线程——析构/disconnect 后线程还跑=UAF 面
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    if (ws_conn_) {
      ws_conn_->is_closing = 1;
      ws_conn_ = nullptr;
    }
  }

  void on_timer() override {
    // v0.53.52: 去 mg_mgr_poll(0)——gw 主循环已驱动同一 mgr,嵌套 poll
    /// =mongoose 未定义行为;改为断线重连检查(接管原 CLOSE 内 sleep 逻辑)
    if (!mgr_ || bot_token_.empty()) return;
    static time_t last_try = 0;
    time_t now = time(nullptr);
    if (!ws_connected_ && now - last_try >= 5) {
      last_try = now;
      log_event("discord", LogLevel::Info, "reconnect attempt");
      ws_connect(gateway_url_ + "/?v=10&encoding=json");
    }
  }

  // ── 消息发送 ──

  void send_chat(const std::string& chat_id,
                 const std::string& text,
                 const std::string& thread_id = "") override {
    nlohmann::json body;
    body["content"] = text;
    if (!thread_id.empty()) {
      // Discord 不支持 thread_id 单独发送，使用 message_reference
      // 此处先忽略
    }

    std::string url = "https://discord.com/api/v10/channels/" + chat_id + "/messages";
    std::string resp = http_request("POST", url, body.dump(), bot_token_);

    // 检查速率限制
    try {
      auto j = nlohmann::json::parse(resp);
      if (j.contains("retry_after")) {
        int wait_ms = static_cast<int>(j["retry_after"].get<double>() * 1000);
        log_event("discord", LogLevel::Warn, "rate limited",
                  {{"wait_ms", wait_ms}});
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        // 重试一次
        http_request("POST", url, body.dump(), bot_token_);
      }
    } catch (...) {}
  }

  void send_reply(const std::string& message_id,
                  const std::string& text) override {
    // Discord reply = message with message_reference
    nlohmann::json body;
    body["content"] = text;
    body["message_reference"] = {{"message_id", message_id}};

    // 需要知道 channel_id — 从 message_id 无法直接获取
    // 此处用 last_channel_id_ 兜底
    if (!last_channel_id_.empty()) {
      std::string url = "https://discord.com/api/v10/channels/" +
                        last_channel_id_ + "/messages";
      http_request("POST", url, body.dump(), bot_token_);
    }
  }

  std::string add_reaction(const std::string& message_id,
                           const std::string& emoji) override {
    if (last_channel_id_.empty()) return "";

    // 对 emoji 进行 URL 编码
    std::string encoded_emoji;
    for (char c : emoji) {
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
        encoded_emoji += c;
      } else {
        char hex[4];
        snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
        encoded_emoji += hex;
      }
    }

    std::string url = "https://discord.com/api/v10/channels/" +
                      last_channel_id_ + "/messages/" +
                      message_id + "/reactions/" + encoded_emoji + "/@me";
    http_request("PUT", url, "", bot_token_);
    return message_id + ":" + emoji;  // 简化 reaction_id
  }

  bool remove_reaction(const std::string& message_id,
                       const std::string& reaction_id) override {
    if (last_channel_id_.empty()) return false;

    auto colon = reaction_id.find(':');
    std::string emoji = (colon != std::string::npos)
                        ? reaction_id.substr(colon + 1)
                        : reaction_id;

    std::string encoded_emoji;
    for (char c : emoji) {
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
        encoded_emoji += c;
      } else {
        char hex[4];
        snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
        encoded_emoji += hex;
      }
    }

    std::string url = "https://discord.com/api/v10/channels/" +
                      last_channel_id_ + "/messages/" +
                      message_id + "/reactions/" + encoded_emoji + "/@me";
    http_request("DELETE", url, "", bot_token_);
    return true;
  }

 private:
  // ── WebSocket 连接 ──

  void ws_connect(const std::string& url) {
    ws_conn_ = mg_ws_connect(mgr_, url.c_str(), ws_event_handler, this, nullptr);
    if (!ws_conn_) {
      log_event("discord", LogLevel::Error, "mg_ws_connect failed");
      return;
    }
  }

  static void ws_event_handler(struct mg_connection* c, int ev,
                               void* ev_data) {
    auto* self = static_cast<DiscordAdapter*>(c->fn_data);
    if (!self) return;
    self->handle_ws_event(c, ev, ev_data);
  }

  void handle_ws_event(struct mg_connection* c, int ev, void* ev_data) {
    switch (ev) {
      case MG_EV_WS_OPEN: {
        log_event("discord", LogLevel::Info, "WebSocket connected");
        ws_connected_ = true;
        break;
      }

      case MG_EV_WS_MSG: {
        auto* wm = static_cast<struct mg_ws_message*>(ev_data);
        std::string payload(wm->data.buf, wm->data.len);
        handle_gateway_payload(payload);
        break;
      }

      case MG_EV_CLOSE: {
        ws_connected_ = false;
        // v0.53.52: 去 sleep 5s——mongoose 事件回调线程内睡=冻结 gw
        /// 全部连接(飞书/agent/微信)5s;重连移交 on_timer 轮询
        log_event("discord", LogLevel::Warn, "WebSocket closed; will reconnect via timer");
        break;
      }

      case MG_EV_ERROR: {
        log_event("discord", LogLevel::Error, "WebSocket error");
        break;
      }
    }
  }

  // ── Gateway 协议处理 ──

  void handle_gateway_payload(const std::string& payload) {
    try {
      auto msg = nlohmann::json::parse(payload);
      int op = msg.value("op", -1);
      auto seq = msg.value("s", 0);

      if (seq > 0) last_seq_ = seq;

      switch (op) {
        case 10: {  // HELLO
          int interval = msg["d"]["heartbeat_interval"].get<int>();
          log_event("discord", LogLevel::Info, "HELLO",
                    {{"heartbeat_interval_ms", interval}});

          // 发送 IDENTIFY
          nlohmann::json identify;
          identify["op"] = 2;
          identify["d"] = {
            {"token", bot_token_},
            {"intents", 1 << 9},  // GUILD_MESSAGES
            {"properties", {
              {"os", "linux"},
              {"browser", "thin_agent"},
              {"device", "thin_agent"}
            }}
          };
          ws_send(identify.dump());

          // 启动心跳
          start_heartbeat(interval);
          break;
        }

        case 0: {  // DISPATCH
          std::string event_type = msg.value("t", "");
          if (event_type == "READY") {
            log_event("discord", LogLevel::Info, "READY",
                      {{"user", msg["d"]["user"]["username"]}});
          } else if (event_type == "MESSAGE_CREATE") {
            handle_message_create(msg["d"]);
          }
          break;
        }

        case 11:  // HEARTBEAT_ACK
          break;

        case 9: {  // INVALID_SESSION
          log_event("discord", LogLevel::Warn, "INVALID_SESSION, re-identifying");
          // Resumable?
          break;
        }
      }
    } catch (const std::exception& e) {
      log_event("discord", LogLevel::Error, "gateway parse error",
                {{"what", std::string(e.what())}});
    }
  }

  void handle_message_create(const nlohmann::json& data) {
    // 忽略机器人自己的消息
    if (data.contains("author") && data["author"].contains("bot") &&
        data["author"]["bot"].get<bool>()) {
      return;
    }

    std::string content = data.value("content", "");
    if (content.empty()) return;

    std::string message_id = data.value("id", "");
    std::string channel_id = data.value("channel_id", "");
    std::string guild_id = data.value("guild_id", "");

    last_channel_id_ = channel_id;

    // 转发到 Gateway
    forward_message(content, message_id, channel_id, guild_id);
  }

  // ── 心跳 ──

  void start_heartbeat(int interval_ms) {
    heartbeat_running_ = true;
    heartbeat_thread_ = std::thread([this, interval_ms]() {
      while (heartbeat_running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        if (!heartbeat_running_) break;

        nlohmann::json hb;
        hb["op"] = 1;
        hb["d"] = last_seq_.load();
        ws_send(hb.dump());
      }
    });
  }

  void ws_send(const std::string& data) {
    std::lock_guard<std::mutex> lock(ws_mutex_);
    if (ws_conn_ && !ws_conn_->is_closing) {
      mg_ws_send(ws_conn_, data.c_str(), data.size(), WEBSOCKET_OP_TEXT);
    }
  }

  // ── 成员变数 ──

  std::string bot_token_;
  std::string gateway_url_;
  struct mg_mgr* mgr_ = nullptr;
  struct mg_connection* ws_conn_ = nullptr;
  std::atomic<bool> ws_connected_{false};
  std::atomic<bool> heartbeat_running_{false};
  std::atomic<int> last_seq_{0};
  std::string last_channel_id_;
  std::thread heartbeat_thread_;
  std::mutex ws_mutex_;
};

}  // namespace

// ── 工厂函数 ──

extern "C" PlatformAdapter* create_discord_adapter() {
  return new DiscordAdapter();
}

}  // namespace thin_agent
