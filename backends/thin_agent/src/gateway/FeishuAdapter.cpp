/// FeishuAdapter — 飞书 Long Connection 协议适配器
///
/// 实现 PlatformAdapter 接口，封装飞书特有的：
/// - Protobuf 二进制 Frame 编解码
/// - WS Long Connection 连接管理
/// - REST API（发送消息、reaction、tenant token）
///
/// 配置环境变量：FEISHU_APP_ID, FEISHU_APP_SECRET, FEISHU_CHAT_ID

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <cstdio>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "mongoose.h"

#include "thin_agent/api/PlatformAdapter.h"
#include "thin_agent/gateway/HttpObservability.h"  // v0.53.83: 发送可观测
#include "thin_agent/log/LogEvent.h"

namespace thin_agent {
namespace {

// ══════════════════════════════════════════════════════
// Protobuf 手工编解码（仅 Frame + Header，无外部依赖）
// ══════════════════════════════════════════════════════

enum { WT_VARINT = 0, WT_LENGTH = 2 };

static int pb_write_varint(std::vector<uint8_t>& buf, uint64_t v) {
  while (v >= 0x80) { buf.push_back((v & 0x7F) | 0x80); v >>= 7; }
  buf.push_back(v & 0x7F);
  return 0;
}

static void pb_write_tag_varint(std::vector<uint8_t>& buf, int field, uint64_t v) {
  buf.push_back((field << 3) | WT_VARINT);
  uint64_t x = v;
  while (x >= 0x80) { buf.push_back((x & 0x7F) | 0x80); x >>= 7; }
  buf.push_back(x & 0x7F);
}

static void pb_write_tag_length(std::vector<uint8_t>& buf, int field, const void* data, size_t len) {
  buf.push_back((field << 3) | WT_LENGTH);
  pb_write_varint(buf, len);
  buf.insert(buf.end(), (const uint8_t*)data, (const uint8_t*)data + len);
}

static int pb_read_varint(const uint8_t* data, size_t len, uint64_t& out) {
  out = 0;
  int shift = 0;
  for (size_t i = 0; i < len && i < 10; ++i) {
    out |= (uint64_t)(data[i] & 0x7F) << shift;
    if (!(data[i] & 0x80)) return static_cast<int>(i + 1);
    shift += 7;
  }
  return -1;
}

struct FeishuFrame {
  uint64_t seq_id = 0;
  uint64_t log_id = 0;
  int32_t service = 0;
  int32_t method = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string payload;

  std::string header(const std::string& key) const {
    for (auto& h : headers) if (h.first == key) return h.second;
    return "";
  }
};

static bool pb_parse_frame(const uint8_t* data, size_t len, FeishuFrame& f) {
  size_t pos = 0;
  while (pos < len) {
    if (pos >= len) break;
    uint8_t tag = data[pos++];
    int field = tag >> 3;
    int wt = tag & 0x07;

    if (wt == WT_VARINT) {
      uint64_t v;
      int n = pb_read_varint(data + pos, len - pos, v);
      if (n < 0) return false;
      pos += n;
      switch (field) {
        case 1: f.seq_id = v; break;
        case 2: f.log_id = v; break;
        case 3: f.service = static_cast<int32_t>(v); break;
        case 4: f.method = static_cast<int32_t>(v); break;
      }
    } else if (wt == WT_LENGTH) {
      uint64_t slen;
      int n = pb_read_varint(data + pos, len - pos, slen);
      if (n < 0 || pos + n + slen > len) return false;
      pos += n;
      const uint8_t* sdata = data + pos;
      pos += slen;
      switch (field) {
        case 5: {
          size_t hp = 0;
          std::string key, value;
          while (hp < slen) {
            uint8_t htag = sdata[hp++];
            int hfield = htag >> 3;
            int hwt = htag & 0x07;
            if (hwt == WT_LENGTH) {
              uint64_t hv;
              int hn = pb_read_varint(sdata + hp, slen - hp, hv);
              if (hn < 0) break;
              hp += hn;
              std::string s(reinterpret_cast<const char*>(sdata + hp), hv);
              hp += hv;
              if (hfield == 1) key = s;
              else if (hfield == 2) { value = s; f.headers.emplace_back(key, value); }
            } else if (hwt == WT_VARINT) {
              uint64_t hv;
              int hn = pb_read_varint(sdata + hp, slen - hp, hv);
              if (hn < 0) break;
              hp += hn;
            } else break;
          }
          break;
        }
        case 6: f.payload.assign(reinterpret_cast<const char*>(sdata), slen); break;
      }
    }
  }
  return true;
}

static void pb_build_frame(std::vector<uint8_t>& buf, int32_t service, int32_t method,
                           uint64_t seq_id, uint64_t log_id,
                           const std::string& payload) {
  pb_write_tag_varint(buf, 1, seq_id);
  pb_write_tag_varint(buf, 2, log_id);
  pb_write_tag_varint(buf, 3, static_cast<uint64_t>(service));
  pb_write_tag_varint(buf, 4, static_cast<uint64_t>(method));
  if (!payload.empty()) pb_write_tag_length(buf, 6, payload.data(), payload.size());
}

static int parse_url_query_int(const std::string& url, const char* key) {
  const auto q = url.find('?');
  if (q == std::string::npos) return 0;
  const std::string needle = std::string(key) + "=";
  const auto p = url.find(needle, q + 1);
  if (p == std::string::npos) return 0;
  return std::atoi(url.c_str() + p + needle.size());
}

// ══════════════════════════════════════════════════════
// HTTP 工具（适配器内联，避免与 gateway 耦合）
// ══════════════════════════════════════════════════════

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

static std::string http_post(const std::string& url, const std::string& body,
                             const std::vector<std::string>& extra_headers = {},
                             int timeout_ms = 15000) {
  CURL* curl = curl_easy_init();
  if (!curl) return "";
  std::string response;
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  for (const auto& h : extra_headers) hdrs = curl_slist_append(hdrs, h.c_str());
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  static const char* ca_paths[] = {"/etc/ssl/certs/ca-certificates.crt",
                                   "/etc/pki/tls/certs/ca-bundle.crt",
                                   "/etc/ssl/cert.pem", nullptr};
  for (const char** p = ca_paths; *p != nullptr; ++p) {
    std::ifstream ifs(*p);
    if (ifs.good()) { curl_easy_setopt(curl, CURLOPT_CAINFO, *p); break; }
  }
  // v0.53.83: 统一观测（此前裸 perform=发送失败静默；v0.53.79 只补了 delete
  // 路径，发消息的 post 路径漏网=修一漏一）
  thin_agent::gateway::curl_perform_observed(curl, "feishu", url);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  return response;
}

static std::string http_delete(const std::string& url,
                               const std::vector<std::string>& extra_headers = {},
                               int timeout_ms = 5000) {
  CURL* curl = curl_easy_init();
  if (!curl) return "";
  std::string response;
  struct curl_slist* hdrs = nullptr;
  for (const auto& h : extra_headers) hdrs = curl_slist_append(hdrs, h.c_str());
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  // v0.53.83: 并入统一判据（v0.53.79 手写块 → curl_perform_observed）
  thin_agent::gateway::curl_perform_observed(curl, "feishu", url);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  return response;
}

}  // namespace

class FeishuAdapter : public PlatformAdapter {
 public:
  FeishuAdapter() {
    const char* id = std::getenv("FEISHU_APP_ID");
    const char* secret = std::getenv("FEISHU_APP_SECRET");
    const char* chat = std::getenv("FEISHU_CHAT_ID");
    if (id) app_id_ = id;
    if (secret) app_secret_ = secret;
    if (chat) chat_id_ = chat;
  }

  std::string name() const override { return "feishu"; }
  bool is_enabled() const override { return !app_id_.empty(); }

  void connect(struct mg_mgr* mgr) override {
    mgr_ = mgr;
    std::string ws_url = get_ws_url();
    if (ws_url.empty()) return;
    ws_conn_ = mg_ws_connect(mgr, ws_url.c_str(), ws_callback_static, this, nullptr);
    if (ws_conn_) {
      log_event("feishu", LogLevel::Info, "connecting");
      last_ping_ = time(nullptr);
    }
    get_tenant_token();
  }

  void disconnect() override {
    ws_conn_ = nullptr;
    mgr_ = nullptr;
  }

  void on_timer() override {
    if (!ws_conn_ && !app_id_.empty() && mgr_) connect(mgr_);
    ping();
  }

  void send_chat(const std::string& chat, const std::string& text,
                 const std::string& thread_id = "") override {
    get_tenant_token();
    if (tenant_token_.empty()) return;
    auto content = nlohmann::json{{"text", text}};
    auto body = nlohmann::json{{"content", content.dump()}, {"msg_type", "text"}};
    std::string url = domain_ + "/open-apis/im/v1/messages?receive_id_type=chat_id";
    body["receive_id"] = chat;
    http_post(url, body.dump(), {"Authorization: Bearer " + tenant_token_});
  }

  void send_reply(const std::string& message_id, const std::string& text) override {
    get_tenant_token();
    if (tenant_token_.empty()) return;
    auto content = nlohmann::json{{"text", text}};
    auto body = nlohmann::json{{"content", content.dump()}, {"msg_type", "text"}};
    http_post(domain_ + "/open-apis/im/v1/messages/" + message_id + "/reply",
              body.dump(), {"Authorization: Bearer " + tenant_token_});
  }

  std::string add_reaction(const std::string& message_id, const std::string& emoji) override {
    get_tenant_token();
    if (tenant_token_.empty()) return "";
    auto body = nlohmann::json{{"reaction_type", {{"emoji_type", emoji}}}};
    std::string resp = http_post(domain_ + "/open-apis/im/v1/messages/" + message_id + "/reactions",
                                 body.dump(), {"Authorization: Bearer " + tenant_token_});
    try {
      auto j = nlohmann::json::parse(resp);
      if (j.value("code", -1) == 0) return j["data"].value("reaction_id", "");
    } catch (...) {}
    return "";
  }

  bool remove_reaction(const std::string& message_id, const std::string& reaction_id) override {
    if (reaction_id.empty()) return false;
    get_tenant_token();
    if (tenant_token_.empty()) return false;
    std::string resp = http_delete(domain_ + "/open-apis/im/v1/messages/" + message_id
                                   + "/reactions/" + reaction_id,
                                   {"Authorization: Bearer " + tenant_token_});
    try {
      auto j = nlohmann::json::parse(resp);
      return j.value("code", -1) == 0;
    } catch (...) {}
    return false;
  }

  bool is_connected() const override { return ws_conn_ != nullptr; }

  /// cron_notify 目标 chat（从环境变量 FEISHU_CHAT_ID 读取）
  const std::string& chat_id() const { return chat_id_; }

 private:
  void get_tenant_token() {
    if (app_id_.empty()) return;
    time_t now = time(nullptr);
    if (!tenant_token_.empty() && now < token_expires_ - 300) return;
    auto body = nlohmann::json{{"app_id", app_id_}, {"app_secret", app_secret_}};
    std::string resp = http_post(domain_ + "/open-apis/auth/v3/tenant_access_token/internal",
                                 body.dump());
    try {
      auto j = nlohmann::json::parse(resp);
      tenant_token_ = j.value("tenant_access_token", "");
      token_expires_ = now + j.value("expire", 7200);
    } catch (...) { log_event("feishu", LogLevel::Error, "tenant_token fail"); }
  }

  std::string get_ws_url() {
    auto body = nlohmann::json{{"AppID", app_id_}, {"AppSecret", app_secret_}};
    std::string resp = http_post(domain_ + "/callback/ws/endpoint", body.dump(),
                                 {"locale: zh"});
    if (resp.empty()) return "";
    try {
      auto j = nlohmann::json::parse(resp);
      if (j.value("code", -1) != 0) return "";
      std::string url = j["data"].value("URL", j["data"].value("url", ""));
      int sid = parse_url_query_int(url, "service_id");
      if (sid > 0) service_id_ = sid;
      return url;
    } catch (...) {}
    return "";
  }

  void ping() {
    if (!ws_conn_) return;
    time_t now = time(nullptr);
    if (now - last_ping_ < 60) return;  // 每 60s ping 一次
    last_ping_ = now;
    // 发送 PING Frame (method=0)
    std::vector<uint8_t> buf;
    pb_build_frame(buf, service_id_, 0, 0, 0, "");
    mg_ws_send(ws_conn_, buf.data(), buf.size(), WEBSOCKET_OP_BINARY);
  }

  void ws_ack(const FeishuFrame& req) {
    if (!ws_conn_) return;
    std::string ack_payload = R"({"code":200})";
    std::vector<uint8_t> buf;
    pb_build_frame(buf, service_id_, 1, req.seq_id, req.log_id, ack_payload);
    mg_ws_send(ws_conn_, buf.data(), buf.size(), WEBSOCKET_OP_BINARY);
  }

  void handle_binary(const uint8_t* data, size_t len) {
    FeishuFrame f;
    if (!pb_parse_frame(data, len, f)) return;
    if (f.service > 0) service_id_ = f.service;

    if (f.method == 0) return;  // CONTROL frame (pong)
    if (f.method != 1 || f.header("type") != "event") return;

    if (f.payload.empty()) return;
    try {
      auto j = nlohmann::json::parse(f.payload);
      auto header = j.value("header", nlohmann::json{});
      std::string event_type = header.value("event_type", "");

      if (event_type == "im.message.receive_v1") {
        auto event = j.value("event", nlohmann::json{});
        auto msg = event.value("message", nlohmann::json{});
        if (msg.value("message_type", "") != "text") { ws_ack(f); return; }

        auto content_j = nlohmann::json::parse(msg.value("content", "{}"));
        std::string text = content_j.value("text", "");
        std::string message_id = msg.value("message_id", "");
        std::string chat = msg.value("chat_id", "");
        std::string root_id = msg.value("root_id", "");

        if (!text.empty()) {
          log_event("feishu", LogLevel::Info, "msg",
                    {{"text", text.substr(0, 80)}});
          forward_message(text, message_id, chat, root_id);
        }
      }
      ws_ack(f);
    } catch (const std::exception& e) {
      log_event("feishu", LogLevel::Error, "event parse error",
                {{"what", std::string(e.what())}});
    }
  }

  static void ws_callback_static(struct mg_connection* c, int ev, void* ev_data) {
    auto* self = static_cast<FeishuAdapter*>(c->fn_data);
    if (!self) return;
    // TLS init
    if (ev == MG_EV_OPEN && ev_data) {
      const char* url = static_cast<const char*>(ev_data);
      if (mg_url_is_ssl(url)) {
        struct mg_tls_opts opts = {};
        opts.name = mg_url_host(url);
        opts.ca = mg_str("");  // 不验证（简化版）
        mg_tls_init(c, &opts);
      }
    }
    switch (ev) {
      case MG_EV_ERROR:
        log_event("feishu", LogLevel::Error, "ws error");
        break;
      case MG_EV_WS_OPEN:
        self->ws_conn_ = c;
        self->last_ping_ = time(nullptr);
        log_event("feishu", LogLevel::Info, "ws connected");
        break;
      case MG_EV_WS_MSG: {
        auto* wm = static_cast<mg_ws_message*>(ev_data);
        self->handle_binary(reinterpret_cast<const uint8_t*>(wm->data.buf), wm->data.len);
        break;
      }
      case MG_EV_CLOSE:
        log_event("feishu", LogLevel::Warn, "ws closed");
        self->ws_conn_ = nullptr;
        break;
    }
  }

  std::string app_id_;
  std::string app_secret_;
  std::string domain_ = "https://open.feishu.cn";
  std::string chat_id_;
  std::string tenant_token_;
  time_t token_expires_ = 0;
  struct mg_mgr* mgr_ = nullptr;
  struct mg_connection* ws_conn_ = nullptr;
  int service_id_ = 0;
  time_t last_ping_ = 0;
};

}  // namespace thin_agent

// 工厂函数（供 Gateway 调用）
namespace thin_agent {
std::unique_ptr<PlatformAdapter> create_feishu_adapter() {
  return std::make_unique<FeishuAdapter>();
}
}  // namespace thin_agent
