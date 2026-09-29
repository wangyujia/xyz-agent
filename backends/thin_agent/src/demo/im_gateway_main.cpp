#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <chrono>
#include <csignal>
#include "thin_agent/gateway/ReconnectBackoff.h"  // v0.54.1: 重连退避(与 v2 同款收口)
#include "thin_agent/gateway/SendQueuePolicy.h"  // v0.53.87: 发送队列有界
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "mongoose.h"

#include "thin_agent/RuntimePaths.h"
#include "thin_agent/log/RotatingLogger.h"

// thin_agent Gateway — IM 平台 Long Connection 网关
//
// 飞书 Long Connection 协议（Protobuf 二进制帧）:
//   1. POST /callback/ws/endpoint  →  获取 WSS URL（AppID+Secret 直接认证）
//   2. mg_ws_connect(url)          →  WebSocket 出站连接
//   3. 接收 Frame(method=1, type=event)  →  payload 是 JSON 事件
//   4. 回复 Frame(method=1)              →  payload={"code":200}（原始 JSON，非 base64）
//   5. 120s PING Frame(method=0)         →  心跳保活
//
// 微信 iLink 协议（HTTP JSON long-poll）:
//   POST getupdates → updates[] → sendmessage
//
// 编译模式:
//   独立可执行文件:  g++ -o thin_agent_gw im_gateway_main.cpp ...
//   作为共享库编译:  定义 THIN_GATEWAY_AS_LIB（跳过 main()）

namespace {

// v0.53.87: 信号 handler 只许置 sig_atomic 标志（YY4 家族全仓收口；此前普通 bool）
volatile std::sig_atomic_t g_stop = 0;
struct mg_mgr* g_mgr = nullptr;
std::string g_dns4_url_storage;   // mgr.dns4.url 生命周期
bool g_dns_configured = false;
std::string g_tls_ca_bundle;      // 系统 CA PEM，供 mg_tls_init 校验 wss

// ── thin_agent core 连接 ──
struct mg_connection* g_agent_conn = nullptr;
std::string g_agent_url;
// v0.54.1 (R88 孪生漏网复查): v1 与 v2 同款"无退避重连"——on_timer(3s) 里两处
// `if (!g_agent_conn) agent_connect();`，agent 长期不可用时每 3s 死缠。
thin_agent::ReconnectBackoff g_agent_backoff{
    getenv("THIN_AGENT_GW_RECONNECT_BASE_MS") ? atoll(getenv("THIN_AGENT_GW_RECONNECT_BASE_MS")) : 3000,
    getenv("THIN_AGENT_GW_RECONNECT_CAP_MS") ? atoll(getenv("THIN_AGENT_GW_RECONNECT_CAP_MS")) : 60000};
int64_t g_agent_retry_at_ms = 0;
bool    g_agent_open = false;

static int64_t gw_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ── 飞书 ──
std::string g_feishu_app_id;
std::string g_feishu_app_secret;
std::string g_feishu_domain = "https://open.feishu.cn";
std::string g_feishu_tenant_token;    // REST API 用（发消息、获取用户信息等）
time_t g_feishu_token_expires = 0;
struct mg_connection* g_feishu_ws = nullptr;
int g_feishu_service_id = 0;          // WS 握手返回的 service_id
std::string g_feishu_conn_id;
time_t g_feishu_last_ping = 0;
std::string g_feishu_chat_id;          // v0.25.9: cron_notify target chat

// ── 微信 ──
std::string g_wechat_bot_id;
std::string g_wechat_secret;

// ── 待回复 ──
struct PendingReply {
  std::string message_id;
  std::string chat_id;
  std::string thread_id;         // v0.26.3: 话题 ID（飞书 root_id），空为无话题
  std::string platform;
  time_t created_at;
  std::string reaction_id;      // "Typing" reaction id for success cleanup
  std::string lang{"zh-CN"};   // detected user language for thinking msg translation
  std::string last_thinking_msg_id;  // 上一条思考进度消息 ID，发新进度前先撤旧
};
std::vector<PendingReply> g_pending_replies;

// ══════════════════════════════════════════════════════
// Protobuf 手工编解码（仅 Frame + Header，无外部依赖）
// ══════════════════════════════════════════════════════

// Wire types
enum { WT_VARINT = 0, WT_LENGTH = 2 };

// 写 varint 到 buf，返回写入字节数
static int pb_write_varint(std::vector<uint8_t>& buf, uint64_t v) {
  while (v >= 0x80) { buf.push_back((v & 0x7F) | 0x80); v >>= 7; }
  buf.push_back(v & 0x7F);
  return 0;  // buf.size() 跟踪
}

// 写 tag + varint
static void pb_write_tag_varint(std::vector<uint8_t>& buf, int field, uint64_t v) {
  buf.push_back((field << 3) | WT_VARINT);
  uint64_t x = v;
  while (x >= 0x80) { buf.push_back((x & 0x7F) | 0x80); x >>= 7; }
  buf.push_back(x & 0x7F);
}

// 写 tag + length-delimited (string/bytes/message)
static void pb_write_tag_length(std::vector<uint8_t>& buf, int field, const void* data, size_t len) {
  buf.push_back((field << 3) | WT_LENGTH);
  pb_write_varint(buf, len);
  buf.insert(buf.end(), (const uint8_t*)data, (const uint8_t*)data + len);
}

// 读 varint，返回读取字节数（<0 表示不足）
static int pb_read_varint(const uint8_t* data, size_t len, uint64_t& out) {
  out = 0;
  int shift = 0;
  for (size_t i = 0; i < len && i < 10; ++i) {
    out |= (uint64_t)(data[i] & 0x7F) << shift;
    if (!(data[i] & 0x80)) return i + 1;
    shift += 7;
  }
  return -1;
}

// Frame 字段值（从二进制解出）
struct FeishuFrame {
  uint64_t seq_id = 0;
  uint64_t log_id = 0;
  int32_t service = 0;
  int32_t method = 0;          // 0=CONTROL, 1=DATA
  std::vector<std::pair<std::string,std::string>> headers;
  std::string payload;         // 原始 bytes → 转到 JSON 用 string
  std::string log_id_new;

  // 读取 header 值
  std::string header(const std::string& key) const {
    for (auto& h : headers) if (h.first == key) return h.second;
    return "";
  }
  int header_int(const std::string& key) const {
    for (auto& h : headers) if (h.first == key) return std::stoi(h.second);
    return 0;
  }
};

// 解析 Frame 二进制
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
        case 3: f.service = (int32_t)v; break;
        case 4: f.method = (int32_t)v; break;
      }
    } else if (wt == WT_LENGTH) {
      uint64_t slen;
      int n = pb_read_varint(data + pos, len - pos, slen);
      if (n < 0 || pos + n + slen > len) return false;
      pos += n;
      const uint8_t* sdata = data + pos;
      pos += slen;

      switch (field) {
        case 5: {  // Header sub-message
          // Header: field 1=key(string), field 2=value(string)
          size_t hp = 0;
          std::string key, value;
          while (hp < slen) {
            uint8_t ht = sdata[hp++];
            int hf = ht >> 3;
            int hwt = ht & 0x07;
            if (hwt != WT_LENGTH) return false;
            uint64_t hl;
            int hn = pb_read_varint(sdata + hp, slen - hp, hl);
            if (hn < 0 || hp + hn + hl > slen) return false;
            hp += hn;
            std::string hv((const char*)(sdata + hp), hl);
            hp += hl;
            if (hf == 1) key = hv;
            else if (hf == 2) { value = hv; f.headers.emplace_back(key, value); }
          }
          break;
        }
        case 6: /* payload_encoding — ignored */ break;
        case 7: /* payload_type — ignored */ break;
        case 8: f.payload.assign((const char*)sdata, slen); break; // payload (actual)
        case 9: f.log_id_new.assign((const char*)sdata, slen); break;
      }
    } else {
      return false;  // unknown wire type
    }
  }
  return true;
}

// 构造 Frame 二进制
static std::vector<uint8_t> pb_build_frame(int method,
    const std::vector<std::pair<std::string,std::string>>& headers,
    const std::string& payload,
    int service = 0) {
  std::vector<uint8_t> buf;

  // Header sub-message
  std::vector<uint8_t> hdr_block;
  for (auto& h : headers) {
    std::vector<uint8_t> hdr;
    pb_write_tag_length(hdr, 1, h.first.data(), h.first.size());
    pb_write_tag_length(hdr, 2, h.second.data(), h.second.size());
    // 前置 length 作为 sub-message
    std::vector<uint8_t> tmp;
    pb_write_tag_length(tmp, 5, hdr.data(), hdr.size());
    hdr_block.insert(hdr_block.end(), tmp.begin(), tmp.end());
  }

  pb_write_tag_varint(buf, 1, 0);                       // SeqID
  pb_write_tag_varint(buf, 2, 0);                       // LogID
  pb_write_tag_varint(buf, 3, service);                 // service
  pb_write_tag_varint(buf, 4, method);                  // method
  buf.insert(buf.end(), hdr_block.begin(), hdr_block.end());  // headers
  if (!payload.empty())
    pb_write_tag_length(buf, 8, payload.data(), payload.size());
  return buf;
}

// Base64 编码（URL 安全，无外部依赖）
static std::string base64_encode(const std::string& s) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0, valb = -6;
  for (unsigned char c : s) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) { out += tbl[(val >> valb) & 0x3F]; valb -= 6; }
  }
  if (valb > -6) out += tbl[((val << 8) >> (valb + 8)) & 0x3F];
  while (out.size() % 4) out += '=';
  return out;
}

/// 从 wss URL query 解析 service_id（飞书 endpoint 返回的 URL 常带 ?service_id=...）
static int parse_url_query_int(const std::string& url, const char* key) {
  const auto q = url.find('?');
  if (q == std::string::npos) return 0;
  const std::string needle = std::string(key) + "=";
  const auto p = url.find(needle, q + 1);
  if (p == std::string::npos) return 0;
  return std::atoi(url.c_str() + p + needle.size());
}

/// wss 客户端在 MG_EV_OPEN 时初始化 TLS（mongoose 需编译 MG_TLS_OPENSSL）
static bool gw_load_system_ca_bundle() {
  if (!g_tls_ca_bundle.empty()) return true;
  static const char* paths[] = {
      "/etc/ssl/certs/ca-certificates.crt",
      "/etc/pki/tls/certs/ca-bundle.crt",
      "/etc/ssl/cert.pem",
      nullptr,
  };
  for (const char** p = paths; *p != nullptr; ++p) {
    std::ifstream ifs(*p, std::ios::binary);
    if (!ifs.is_open()) continue;
    g_tls_ca_bundle.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    if (g_tls_ca_bundle.empty()) continue;
    std::cerr << "[gw] tls ca bundle: " << *p << " (" << g_tls_ca_bundle.size() << " bytes)\n";
    return true;
  }
  std::cerr << "[gw] tls ca bundle: not found, wss verify may fail\n";
  return false;
}

static void gw_ws_tls_maybe_init(struct mg_connection* c, int ev, void* ev_data) {
  if (ev != MG_EV_OPEN || ev_data == nullptr) return;
  const char* url = static_cast<const char*>(ev_data);
  if (!mg_url_is_ssl(url)) return;
  gw_load_system_ca_bundle();
  const struct mg_str host = mg_url_host(url);
  struct mg_tls_opts opts = {};
  opts.name = host;
  if (!g_tls_ca_bundle.empty()) {
    opts.ca = mg_str_n(g_tls_ca_bundle.data(), g_tls_ca_bundle.size());
  } else {
    opts.ca = mg_str("");  // OpenSSL：空 ca 不启用 SSL_VERIFY_PEER
  }
  mg_tls_init(c, &opts);
  std::cerr << "[gw] tls init host=" << std::string(host.buf, host.len) << "\n";
}

// ══════════════════════════════════════════════════════
// 工具函数
// ══════════════════════════════════════════════════════

/// 从 /etc/resolv.conf 读取第一个 nameserver，配置 Mongoose DNS（替代默认 8.8.8.8）
static bool gw_apply_system_dns(struct mg_mgr* mgr) {
  if (mgr == nullptr) return false;
  std::ifstream ifs("/etc/resolv.conf");
  if (!ifs.is_open()) return false;

  const std::string prefix = "nameserver ";
  std::string line;
  while (std::getline(ifs, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (line.compare(0, prefix.size(), prefix) != 0) continue;

    std::string ip = line.substr(prefix.size());
    while (!ip.empty() && std::isspace(static_cast<unsigned char>(ip.front()))) ip.erase(ip.begin());
    while (!ip.empty() && std::isspace(static_cast<unsigned char>(ip.back()))) ip.pop_back();
    if (ip.empty()) continue;

    if (ip.find(':') != std::string::npos) {
      const auto pct = ip.find('%');
      if (pct != std::string::npos) ip.erase(pct);
      g_dns4_url_storage = "udp://[" + ip + "]:53";
    } else {
      g_dns4_url_storage = "udp://" + ip + ":53";
    }
    mgr->dns4.url = g_dns4_url_storage.c_str();
    g_dns_configured = true;
    std::cerr << "[gw] dns4=" << g_dns4_url_storage << " (from /etc/resolv.conf)\n";
    return true;
  }
  return false;
}

static size_t gw_curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

/// libcurl 同步 POST（替代 system curl，供嵌入式设备使用）
static std::string gw_http_post(const std::string& url,
                                const std::string& body,
                                const std::vector<std::string>& extra_headers = {},
                                int timeout_ms = 15000) {
  CURL* curl = curl_easy_init();
  if (!curl) return "";

  std::string response;
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  for (const auto& h : extra_headers) {
    hdrs = curl_slist_append(hdrs, h.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, gw_curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  static const char* ca_paths[] = {
      "/etc/ssl/certs/ca-certificates.crt",
      "/etc/pki/tls/certs/ca-bundle.crt",
      "/etc/ssl/cert.pem",
      nullptr,
  };
  for (const char** p = ca_paths; *p != nullptr; ++p) {
    std::ifstream ifs(*p);
    if (ifs.good()) {
      curl_easy_setopt(curl, CURLOPT_CAINFO, *p);
      break;
    }
  }

  const CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    std::cerr << "[gw] http post fail: " << curl_easy_strerror(rc) << " url=" << url << "\n";
    return "";
  }
  if (code < 200 || code >= 300) {
    std::cerr << "[gw] http post status=" << code << " url=" << url << " body=" << response << "\n";
    return "";
  }
  return response;
}

// ── 单线程发送队列（保序 + 不阻塞主事件循环）──

struct QueuedSend {
  std::string url;
  std::string body;
  std::vector<std::string> headers;
  std::string method;  // "POST" or "DELETE"
  int seq = 0;
};

static std::queue<QueuedSend> g_send_queue;
static std::mutex g_send_mutex;
static std::condition_variable g_send_cv;
static std::thread g_send_worker;
static bool g_send_stop = false;

static std::string gw_http_delete(const std::string& url,
                                   const std::vector<std::string>& extra_headers,
                                   int timeout_ms);

static int g_enqueue_seq = 0;

static void enqueue_send(std::string url, std::string body,
                         std::vector<std::string> headers,
                         std::string method = "POST") {
  int seq = ++g_enqueue_seq;
  // extract first 60 chars of body for logging
  std::string preview = body.substr(0, 60);
  // replace newlines
  for (auto& c : preview) if (c == '\n' || c == '\r') c = ' ';
  std::cerr << "[gw] ENQUEUE #" << seq << " " << method << " " << preview << std::endl;
  std::lock_guard<std::mutex> lock(g_send_mutex);
  // v0.53.87: 有界（v0.53.86 只修了 v2 的同一结构 = 孪生漏网）。复用 SendQueuePolicy：
  // 满载丢最旧 + 可观测，语义与 v2/agent send_queue 一致
  if (thin_agent::gateway::send_queue_needs_drop(g_send_queue.size())) {
    g_send_queue.pop();
    static int dropped_total = 0;
    std::cerr << "[gw] send queue full (cap=" << thin_agent::gateway::kSendQueueCap
              << "), dropped oldest total=" << ++dropped_total << "\n";
  }
  g_send_queue.push({std::move(url), std::move(body), std::move(headers), std::move(method), seq});
  g_send_cv.notify_one();
}

static void send_worker() {
  while (true) {
    std::unique_lock<std::mutex> lock(g_send_mutex);
    g_send_cv.wait(lock, []{ return !g_send_queue.empty() || g_send_stop; });
    if (g_send_stop && g_send_queue.empty()) break;
    if (g_send_queue.empty()) continue;
    auto q = std::move(g_send_queue.front());
    g_send_queue.pop();
    lock.unlock();

    if (q.method == "DELETE") {
      std::cerr << "[gw] SEND    #" << q.seq << " DELETE" << std::endl;
      gw_http_delete(q.url, q.headers, 5000);
    } else {
      std::cerr << "[gw] SEND    #" << q.seq << " POST" << std::endl;
      (void)gw_http_post(q.url, q.body, q.headers);
    }
  }
}

/// 异步 POST（兼容旧接口，走队列）
static void gw_http_post_async(const std::string& url,
                               const std::string& body,
                               const std::vector<std::string>& extra_headers = {}) {
  enqueue_send(url, body, extra_headers);
}

// ══════════════════════════════════════════════════════
// 飞书模块
// ══════════════════════════════════════════════════════

/// 获取 tenant_access_token（REST API 用）
static void feishu_get_tenant_token() {
  if (g_feishu_app_id.empty()) return;
  time_t now = time(nullptr);
  if (!g_feishu_tenant_token.empty() && now < g_feishu_token_expires - 300) return;

  auto body = nlohmann::json{{"app_id", g_feishu_app_id}, {"app_secret", g_feishu_app_secret}};
  std::string resp = gw_http_post(g_feishu_domain + "/open-apis/auth/v3/tenant_access_token/internal",
                                  body.dump());
  try {
    auto j = nlohmann::json::parse(resp);
    g_feishu_tenant_token = j.value("tenant_access_token", "");
    g_feishu_token_expires = now + j.value("expire", 7200);
    std::cerr << "[feishu] tenant_token ok, expire=" << j.value("expire", 0) << "s\n";
  } catch (...) { std::cerr << "[feishu] tenant_token fail: " << resp << "\n"; }
}

/// 获取 Long Connection WS URL
static std::string feishu_get_ws_url() {
  auto body = nlohmann::json{{"AppID", g_feishu_app_id}, {"AppSecret", g_feishu_app_secret}};
  std::string resp = gw_http_post(g_feishu_domain + "/callback/ws/endpoint",
                                  body.dump(),
                                  {"locale: zh"});
  if (resp.empty()) {
    std::cerr << "[feishu] ws url fetch failed (empty response)\n";
    return "";
  }
  try {
    auto j = nlohmann::json::parse(resp);
    if (j.value("code", -1) != 0) { std::cerr << "[feishu] ws url err: " << resp << "\n"; return ""; }
    const auto& data = j.at("data");
    std::string url = data.value("URL", data.value("url", ""));
    if (url.empty()) { std::cerr << "[feishu] ws url missing in: " << resp << "\n"; return ""; }
    const int sid = parse_url_query_int(url, "service_id");
    if (sid > 0) {
      g_feishu_service_id = sid;
      std::cerr << "[feishu] service_id=" << sid << "\n";
    }
    std::cerr << "[feishu] ws url=" << url << "\n";
    return url;
  } catch (...) { std::cerr << "[feishu] ws url parse fail: " << resp << "\n"; }
  return "";
}

/// 发 REST API 回复消息
static void feishu_rest_reply(const std::string& message_id, const std::string& text) {
  feishu_get_tenant_token();
  if (g_feishu_tenant_token.empty()) return;
  auto content = nlohmann::json{{"text", text}};
  auto body = nlohmann::json{{"content", content.dump()}, {"msg_type", "text"}};
  gw_http_post_async(g_feishu_domain + "/open-apis/im/v1/messages/" + message_id + "/reply",
                     body.dump(),
                     {"Authorization: Bearer " + g_feishu_tenant_token});
}

static void feishu_rest_send_chat(const std::string& chat_id, const std::string& text,
                                  const std::string& root_id = "") {
  feishu_get_tenant_token();
  if (g_feishu_tenant_token.empty()) return;
  auto content = nlohmann::json{{"text", text}};
  auto body = nlohmann::json{{"receive_id", chat_id}, {"content", content.dump()}, {"msg_type", "text"}};
  if (!root_id.empty()) body["root_id"] = root_id;  // v0.26.3: 话题消息路由
  gw_http_post_async(g_feishu_domain + "/open-apis/im/v1/messages?receive_id_type=chat_id",
                     body.dump(),
                     {"Authorization: Bearer " + g_feishu_tenant_token});
}

/// 同步 send_chat 并返回 message_id（用于 thinking 占位）
static std::string feishu_rest_send_chat_sync(const std::string& chat_id, const std::string& text,
                                         const std::string& root_id = "") {
  feishu_get_tenant_token();
  if (g_feishu_tenant_token.empty()) return "";
  auto content = nlohmann::json{{"text", text}};
  auto body = nlohmann::json{{"receive_id", chat_id}, {"content", content.dump()}, {"msg_type", "text"}};
  if (!root_id.empty()) body["root_id"] = root_id;  // v0.26.3
  std::string resp = gw_http_post(g_feishu_domain + "/open-apis/im/v1/messages?receive_id_type=chat_id",
                                  body.dump(),
                                  {"Authorization: Bearer " + g_feishu_tenant_token});
  try {
    auto j = nlohmann::json::parse(resp);
    if (j.value("code", -1) == 0) return j["data"].value("message_id", "");
  } catch (...) {}
  return "";
}

/// 同步删除飞书消息（用于撤旧进度消息）— 失败静默，不阻塞主流程
static void feishu_delete_msg_sync(const std::string& message_id) {
  if (message_id.empty()) return;
  feishu_get_tenant_token();
  if (g_feishu_tenant_token.empty()) return;
  std::string url = g_feishu_domain + "/open-apis/im/v1/messages/" + message_id;
  std::vector<std::string> headers = {"Authorization: Bearer " + g_feishu_tenant_token};
  std::string resp = gw_http_delete(url, headers, 5000);
  try {
    auto j = nlohmann::json::parse(resp);
    if (j.value("code", -1) == 0) {
      std::cerr << "[feishu] deleted msg " << message_id << "\n";
    } else {
      std::cerr << "[feishu] delete msg rejected: code=" << j.value("code", -1)
                << " msg=" << j.value("msg", "") << "\n";
    }
  } catch (...) {}
}

/// 更新已发送消息（PATCH），用于 thinking 卡片同一条刷新
static void feishu_rest_update_msg(const std::string& message_id, const std::string& text) {
  feishu_get_tenant_token();
  if (g_feishu_tenant_token.empty()) return;
  auto content = nlohmann::json{{"text", text}};
  auto body = nlohmann::json{{"content", content.dump()}};
  gw_http_post_async("PATCH " + g_feishu_domain + "/open-apis/im/v1/messages/" + message_id,
                     body.dump(),
                     {"Authorization: Bearer " + g_feishu_tenant_token});
}
/// 同步 PATCH 更新消息（原地刷新进度，不产生撤回提示）
static bool feishu_rest_update_msg_sync(const std::string& message_id, const std::string& text) {
  if (message_id.empty()) return false;
  feishu_get_tenant_token();
  if (g_feishu_tenant_token.empty()) return false;
  auto content = nlohmann::json{{"text", text}};
  auto body = nlohmann::json{{"content", content.dump()}};
  CURL* curl = curl_easy_init();
  if (!curl) return false;
  std::string response;
  struct curl_slist* hdrs = nullptr;
  std::string auth = "Authorization: Bearer " + g_feishu_tenant_token;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  hdrs = curl_slist_append(hdrs, auth.c_str());
  std::string url = g_feishu_domain + "/open-apis/im/v1/messages/" + message_id;
  std::string req_body = body.dump();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req_body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, gw_curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  CURLcode rc = curl_easy_perform(curl);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  std::cerr << "[feishu] PATCH msg " << message_id << " rc=" << rc << " resp=" << response << "\n";
  if (rc != CURLE_OK) return false;
  try {
    auto j = nlohmann::json::parse(response);
    return j.value("code", -1) == 0;
  } catch (...) {}
  return false;
}


/// 同步回复并返回 message_id（用于 thinking 占位，后续 PATCH 更新同一消息）
static std::string feishu_rest_reply_sync(const std::string& message_id, const std::string& text) {
  feishu_get_tenant_token();
  if (g_feishu_tenant_token.empty()) return "";
  auto content = nlohmann::json{{"text", text}};
  auto body = nlohmann::json{{"content", content.dump()}, {"msg_type", "text"}};
  std::string resp = gw_http_post(g_feishu_domain + "/open-apis/im/v1/messages/" + message_id + "/reply",
                                  body.dump(),
                                  {"Authorization: Bearer " + g_feishu_tenant_token});
  try {
    auto j = nlohmann::json::parse(resp);
    if (j.value("code", -1) == 0) return j["data"].value("message_id", "");
  } catch (...) {}
 return "";
 }

 /// 同步 DELETE 请求（用于删除 reaction）
 static std::string gw_http_delete(const std::string& url,
                                  const std::vector<std::string>& extra_headers = {},
                                  int timeout_ms = 5000) {
 CURL* curl = curl_easy_init();
 if (!curl) return "";
 std::string response;
 struct curl_slist* hdrs = nullptr;
 for (const auto& h : extra_headers) {
   hdrs = curl_slist_append(hdrs, h.c_str());
 }
 curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
 curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
 curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
 curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, gw_curl_write_cb);
 curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
 curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
 curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

 const CURLcode rc = curl_easy_perform(curl);
 curl_slist_free_all(hdrs);
 curl_easy_cleanup(curl);
 if (rc != CURLE_OK) {
   std::cerr << "[gw] http delete fail: " << curl_easy_strerror(rc) << "\n";
   return "";
 }
 return response;
 }

 /// 添加 emoji reaction 到消息，同步返回 reaction_id（失败返回空）
 static std::string feishu_add_reaction_sync(const std::string& message_id,
                                            const std::string& emoji) {
 feishu_get_tenant_token();
 if (g_feishu_tenant_token.empty()) return "";
 auto body = nlohmann::json{{"reaction_type", {{"emoji_type", emoji}}}};
 std::string resp = gw_http_post(
     g_feishu_domain + "/open-apis/im/v1/messages/" + message_id + "/reactions",
     body.dump(),
     {"Authorization: Bearer " + g_feishu_tenant_token});
 try {
   auto j = nlohmann::json::parse(resp);
   if (j.value("code", -1) == 0) return j["data"].value("reaction_id", "");
   std::cerr << "[feishu] add reaction rejected: code=" << j.value("code", -1)
             << " msg=" << j.value("msg", "") << "\n";
 } catch (...) {}
 return "";
 }

 /// 移除 reaction
 static bool feishu_remove_reaction_sync(const std::string& message_id,
                                        const std::string& reaction_id) {
 if (reaction_id.empty()) return false;
 feishu_get_tenant_token();
 if (g_feishu_tenant_token.empty()) return false;
 std::string url = g_feishu_domain + "/open-apis/im/v1/messages/" + message_id
                   + "/reactions/" + reaction_id;
 std::vector<std::string> headers = {"Authorization: Bearer " + g_feishu_tenant_token};
 std::string resp = gw_http_delete(url, headers);
 try {
   auto j = nlohmann::json::parse(resp);
   return j.value("code", -1) == 0;
 } catch (...) {}
 return false;
 }

 /// 通过 WS 回复 protobuf 确认帧（payload 为原始 JSON {"code":200}）
static void feishu_ws_ack(const FeishuFrame& req) {
  if (!g_feishu_ws) return;
  const int service = req.service > 0 ? req.service : g_feishu_service_id;
  const std::string ack_body = R"({"code":200})";
  auto frame = pb_build_frame(1, req.headers, ack_body, service);
  mg_ws_send(g_feishu_ws, frame.data(), frame.size(), WEBSOCKET_OP_BINARY);
}

/// 发送 PING
static void feishu_ping() {
  if (!g_feishu_ws) return;
  time_t now = time(nullptr);
  if (now - g_feishu_last_ping < 110) return;
  g_feishu_last_ping = now;
  auto frame = pb_build_frame(0, {{"type", "ping"}}, "", g_feishu_service_id);
  mg_ws_send(g_feishu_ws, frame.data(), frame.size(), WEBSOCKET_OP_BINARY);
}

/// 剥离 IM 平台 @ 提及（飞书 @_user_N、@机器人名 等），只保留用户正文。
static std::string trim_im_text(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  return s;
}

static std::string strip_im_mentions(std::string text) {
  text = trim_im_text(std::move(text));
  while (!text.empty() && text[0] == '@') {
    const auto sp = text.find(' ');
    if (sp == std::string::npos) {
      return "";
    }
    text = trim_im_text(text.substr(sp + 1));
  }
  return text;
}

// ── 语言检测 ──

/// Unicode 范围检测用户消息语言
static std::string detect_lang(const std::string& text) {
  bool has_thai = false, has_kana = false, has_hangul = false;
  bool has_cyrillic = false, has_cjk = false, has_traditional = false;

  // simple UTF-8 iteration
  for (size_t i = 0; i < text.size(); ) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    char32_t cp;
    int len;
    if (c < 0x80)       { cp = c; len = 1; }
    else if (c < 0xE0)  { cp = ((c & 0x1F) << 6) | (text[i+1] & 0x3F); len = 2; }
    else if (c < 0xF0)  { cp = ((c & 0x0F) << 12) | ((text[i+1] & 0x3F) << 6) | (text[i+2] & 0x3F); len = 3; }
    else                { cp = ((c & 0x07) << 18) | ((text[i+1] & 0x3F) << 12) | ((text[i+2] & 0x3F) << 6) | (text[i+3] & 0x3F); len = 4; }
    i += len;

    if (cp >= 0x0E00 && cp <= 0x0E7F) has_thai = true;
    else if (cp >= 0x3040 && cp <= 0x30FF) has_kana = true;
    else if (cp >= 0xAC00 && cp <= 0xD7AF) has_hangul = true;
    else if (cp >= 0x0400 && cp <= 0x04FF) has_cyrillic = true;
    else if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)) has_cjk = true;
    // traditional-chinese-specific characters (vs simplified)
    else if (cp == 0x9AD4 || cp == 0x95DC || cp == 0x8B93 || cp == 0x5F8C ||
             cp == 0x8207 || cp == 0x5617 || cp == 0x9084 || cp == 0x55CE) has_traditional = true;
  }

  if (has_thai)    return "th";
  if (has_kana)    return "ja";
  if (has_hangul)  return "ko";
  if (has_cyrillic) return "ru";
  if (has_cjk && has_traditional) return "zh-TW";
  if (has_cjk)     return "zh-CN";
  return "en";
}

/// 转发给 thin_agent core
/// @param chat_id   平台会话 ID（飞书 chat_id、微信用户 ID 等）
/// @param thread_id 平台话题 ID（飞书 root_id、Telegram message_thread_id 等），无话题为空
static void forward_to_agent(const std::string& text,
    const std::string& reply_msg_id, const std::string& chat_id,
    const std::string& thread_id, const std::string& platform) {
  const std::string cleaned = strip_im_mentions(text);
  if (cleaned.empty()) {
    std::cerr << "[gw] skip empty message after mention strip\n";
    return;
  }
  PendingReply pr{reply_msg_id, chat_id, "", platform, time(nullptr), ""};
  pr.thread_id = thread_id;  // v0.26.3: 话题 ID 透传
  pr.lang = detect_lang(cleaned);
  std::cerr << "[gw] lang=" << pr.lang << "\n";
  // v0.54.1 (R88): **先判连接再入队**。此前 pending 先 push、再判 g_agent_conn：agent 断线
  // 时消息被静默丢弃（只写一行日志），同时**在 pending 里留下一条永远不会被匹配的泄漏条目**
  // （用户 60s 后莫名收到 CrossMark）。现在断线直接给用户可见反馈且不入队。
  if (!g_agent_conn) {
    std::cerr << "[gw] agent not connected — dropping user message, user notified (CrossMark)\n";
    if (!reply_msg_id.empty()) feishu_add_reaction_sync(reply_msg_id, "CrossMark");
    return;
  }
  // v0.53.74: pending 上限 1024——60s 超时只清到期的,洪峰窗内无界
  /// 堆积=OOM 面;超限丢最老(与 agent send_queue 同哲学)
  if (g_pending_replies.size() >= 1024) {
    // v0.54.1: 淘汰必须**给被淘汰的那条用户一个可见收尾**（否则那条消息悄悄消失，
    // 且 FIFO 回退匹配可能把它的回复投给别的会话）
    auto& evicted = g_pending_replies.front();
    std::cerr << "[gw] pending overflow(1024) — evicting oldest, user notified (CrossMark)\n";
    if (!evicted.message_id.empty()) {
      if (!evicted.reaction_id.empty()) feishu_remove_reaction_sync(evicted.message_id, evicted.reaction_id);
      feishu_add_reaction_sync(evicted.message_id, "CrossMark");
    }
    g_pending_replies.erase(g_pending_replies.begin());
  }
  g_pending_replies.push_back(pr);
  // 先转发给 agent（不阻塞），再加 reaction
  auto req = nlohmann::json{
      {"type", "chat"},
      {"text", cleaned},
      {"chat_id", chat_id},
      {"thread_id", thread_id}
  };
  std::string payload = req.dump();
  if (g_agent_conn) {
    mg_ws_send(g_agent_conn, payload.c_str(), payload.size(), WEBSOCKET_OP_TEXT);
    std::cerr << "[gw] → agent: " << cleaned.substr(0, 60) << "\n";
    if (platform == "feishu" && !reply_msg_id.empty()) {
      g_pending_replies.back().reaction_id = feishu_add_reaction_sync(reply_msg_id, "Typing");
    }
  } else {
    std::cerr << "[gw] agent not connected\n";
  }
}

// ── 飞书二进制帧处理 ──

static void feishu_handle_binary(const uint8_t* data, size_t len) {
  FeishuFrame f;
  if (!pb_parse_frame(data, len, f)) { std::cerr << "[feishu] frame parse fail\n"; return; }

  if (f.service > 0) g_feishu_service_id = f.service;

  std::string type = f.header("type");
  std::string event_type;

  if (f.method == 0) {  // CONTROL
    if (type == "pong") {
      // pong 可能带 ClientConfig
      if (!f.payload.empty()) {
        try {
          auto cfg = nlohmann::json::parse(f.payload);
          std::cerr << "[feishu] pong cfg: reconnect=" << cfg.value("ReconnectInterval", 0) << "\n";
        } catch (...) {}
      }
    }
    return;
  }

  if (f.method != 1 || type != "event") return;  // DATA frame, type=event

  // payload 是 JSON 事件
  if (f.payload.empty()) return;
  try {
    auto j = nlohmann::json::parse(f.payload);

    // v2 事件: {schema, header:{event_type, event_id, ...}, event:{...}}
    auto header = j.value("header", nlohmann::json{});
    event_type = header.value("event_type", "");
    std::string msg_id = f.header("message_id");

    if (event_type == "im.message.receive_v1") {
      auto event = j.value("event", nlohmann::json{});
      auto msg = event.value("message", nlohmann::json{});
      if (msg.value("message_type", "") != "text") { feishu_ws_ack(f); return; }

      auto content_j = nlohmann::json::parse(msg.value("content", "{}"));
      std::string text = content_j.value("text", "");
      std::string message_id = msg.value("message_id", "");
      std::string chat_id = msg.value("chat_id", "");
      std::string root_id = msg.value("root_id", "");  // v0.26.1: 话题 ID

      if (!text.empty()) {
        std::cerr << "[feishu] msg: " << text.substr(0, 80) << "\n";
        forward_to_agent(text, message_id, chat_id, root_id, "feishu");
      }
    } else {
      std::cerr << "[feishu] unhandled event: " << event_type << "\n";
    }

    feishu_ws_ack(f);  // 确认收到
  } catch (const std::exception& e) {
    std::cerr << "[feishu] event parse error: " << e.what() << "\n";
  }
}

// ── 飞书 WS 回调 ──

static void feishu_ws_fn(struct mg_connection* c, int ev, void* ev_data) {
  gw_ws_tls_maybe_init(c, ev, ev_data);
  switch (ev) {
    case MG_EV_ERROR:
      std::cerr << "[feishu] ws error: " << (ev_data ? static_cast<const char*>(ev_data) : "") << "\n";
      break;

    case MG_EV_WS_OPEN:
      std::cerr << "[feishu] ws connected ✓\n";
      g_feishu_last_ping = time(nullptr);
      break;

    case MG_EV_WS_MSG: {
      auto* wm = static_cast<mg_ws_message*>(ev_data);
      feishu_handle_binary((const uint8_t*)wm->data.buf, wm->data.len);
      break;
    }

    case MG_EV_CLOSE:
      std::cerr << "[feishu] ws closed, will reconnect\n";
      g_feishu_ws = nullptr;
      break;
  }
}

static void feishu_connect() {
  if (!g_mgr || g_feishu_app_id.empty()) return;

  std::string ws_url = feishu_get_ws_url();
  if (ws_url.empty()) return;

  g_feishu_ws = mg_ws_connect(g_mgr, ws_url.c_str(), feishu_ws_fn, nullptr, nullptr);
  if (g_feishu_ws) std::cerr << "[feishu] connecting...\n";
  else std::cerr << "[feishu] connect failed\n";

  feishu_get_tenant_token();  // 预取 REST token
}

// ══════════════════════════════════════════════════════
// 微信 iLink 模块（HTTP JSON long-poll，无需变）
// ══════════════════════════════════════════════════════

static void wechat_send_reply(const std::string& chat_id, const std::string& text) {
  auto body = nlohmann::json{{"to", chat_id}, {"msgType", "text"}, {"content", text}};
  gw_http_post_async("https://ilinkai.weixin.qq.com/ilink/bot/sendmessage", body.dump());
}

static void wechat_poll_loop() {
  std::cerr << "[wechat] poll started\n";
  int64_t last_seq = 0;
  while (!g_stop) {
    if (g_wechat_bot_id.empty()) { sleep(5); continue; }
    auto body = nlohmann::json{{"botId", g_wechat_bot_id}, {"secret", g_wechat_secret},
                                {"seq", last_seq}, {"count", 10}};
    std::string resp = gw_http_post("https://ilinkai.weixin.qq.com/ilink/bot/getupdates", body.dump());
    try {
      auto j = nlohmann::json::parse(resp);
      if (j.value("code", -1) != 0) { sleep(5); continue; }
      for (auto& u : j.value("updates", nlohmann::json::array())) {
        last_seq = std::max(last_seq, (int64_t)u.value("seq", 0));
        if (u.value("msgType", "") != "text") continue;
        std::string text = u.value("content", "");
        if (!text.empty()) {
          std::cerr << "[wechat] msg: " << text.substr(0, 80) << "\n";
          forward_to_agent(text, u.value("msgId", ""), u.value("from", ""), "", "wechat");
        }
      }
    } catch (...) {}
    sleep(2);
  }
}

// ══════════════════════════════════════════════════════
// thin_agent core WS 客户端
// ══════════════════════════════════════════════════════

// v0.54.1 (R88): 连接级收尾（R76 律；v2 已在 v0.53.99 修，v1 是孪生漏网）。
// 断线/停机时把在途请求收尾：撤"思考中"表情 + 打 CrossMark + 计数日志。
static int finalize_pending_dropped(const char* reason) {
  const int n = static_cast<int>(g_pending_replies.size());
  for (auto& p : g_pending_replies) {
    if (!p.message_id.empty()) {
      if (!p.reaction_id.empty()) feishu_remove_reaction_sync(p.message_id, p.reaction_id);
      feishu_add_reaction_sync(p.message_id, "CrossMark");
    }
  }
  g_pending_replies.clear();
  if (n > 0) {
    std::cerr << "[gw] finalized " << n << " in-flight request(s), users notified: " << reason << "\n";
  }
  return n;
}

static void agent_ws_fn(struct mg_connection* c, int ev, void* ev_data) {
  gw_ws_tls_maybe_init(c, ev, ev_data);
  switch (ev) {
    case MG_EV_ERROR:
      std::cerr << "[agent] ws error: " << (ev_data ? static_cast<const char*>(ev_data) : "") << "\n";
      // v0.54.1: 不能只置空——在途请求要立刻收尾，并**按退避**排下一次重试
      if (g_agent_conn != nullptr) {
        g_agent_conn = nullptr;
        const int fin = g_agent_open ? finalize_pending_dropped("agent connection lost") : 0;
        g_agent_open = false;
        const int64_t delay = g_agent_backoff.next_delay_ms();
        g_agent_retry_at_ms = gw_now_ms() + delay;
        std::cerr << "[gw] agent disconnected (error, finalized=" << fin << "), retry in "
                  << delay << "ms\n";
      }
      break;

    case MG_EV_WS_OPEN:
      std::cerr << "[agent] connected ✓\n";
      g_agent_backoff.reset();
      g_agent_retry_at_ms = 0;
      g_agent_open = true;
      break;

    case MG_EV_WS_MSG: {
      auto* wm = static_cast<mg_ws_message*>(ev_data);
      std::string msg((const char*)wm->data.buf, wm->data.len);
      try {
        auto resp = nlohmann::json::parse(msg);
        std::string msg_type = resp.value("type", "");

        // v0.26.0: cron_notify — broadcast to all connected platforms
        if (msg_type == "cron_notify") {
          std::string cron_name = resp.value("name", "cron");
          std::string cron_text = resp.value("text", "");
          if (!cron_text.empty()) {
            std::string notify_msg = "🤖 [" + cron_name + "] " + cron_text;
            // Push to every connected platform's home channel
            if (!g_feishu_app_id.empty() && !g_feishu_chat_id.empty()) {
              feishu_rest_send_chat(g_feishu_chat_id, notify_msg);
              std::cerr << "[gw] cron_notify → feishu: " << cron_name << "\n";
            }
            // Future: add WeChat / other platforms here as their HOME_CHAT_ID envs are set
          }
          break;
        }

        // thinking 事件：agent 层已生成完整消息（含 emoji），直接透传
        if (msg_type == "thinking") {
          std::string thinking_msg = resp.value("msg", "");
          // v0.53.51: 按 chat_id 匹配(agent 回显字段优先;旧 agent 无
          /// 回显时回落队头——兼容)。此前恒 front():多会话并发时
          /// thinking 进度发错会话
          if (!g_pending_replies.empty() && !thinking_msg.empty()) {
            const std::string rchat = resp.value("chat_id", "");
            auto pit = g_pending_replies.begin();
            if (!rchat.empty()) {
              pit = std::find_if(g_pending_replies.begin(), g_pending_replies.end(),
                                 [&](const PendingReply& p) { return p.chat_id == rchat; });
              if (pit == g_pending_replies.end()) pit = g_pending_replies.begin();
            }
            auto& p = *pit;
            if (p.platform == "feishu" && !p.chat_id.empty()) {
              // agent 层已生成完整消息（含 emoji），直接透传
              feishu_rest_send_chat(p.chat_id, thinking_msg, p.thread_id);
            }
          }
          break;
        }

        std::string reply_text = resp.value("text", "");
        if (reply_text.empty()) break;

        if (!g_pending_replies.empty()) {
          // v0.53.51: 按 chat_id 匹配——多会话并发时 agent 4 worker
          /// 乱序完成,恒 front() 会把 B 的答案发给 A(回复错位);
          /// chat_id 来自 ws_agent_main v0.53.51 的回显
          const std::string rchat2 = resp.value("chat_id", "");
          auto pit2 = g_pending_replies.begin();
          if (!rchat2.empty()) {
            pit2 = std::find_if(g_pending_replies.begin(), g_pending_replies.end(),
                                [&](const PendingReply& p) { return p.chat_id == rchat2; });
            if (pit2 == g_pending_replies.end()) {
              // 无匹配(旧 agent/异常)——回落队头保底
              pit2 = g_pending_replies.begin();
            }
          }
          auto p = *pit2;
          g_pending_replies.erase(pit2);
          if (p.platform == "feishu") {
            // 先发回复（异步），再摘 reaction（cosmetic）
            if (!p.message_id.empty()) feishu_rest_reply(p.message_id, reply_text);
            else if (!p.chat_id.empty()) feishu_rest_send_chat(p.chat_id, reply_text, p.thread_id);
            feishu_remove_reaction_sync(p.message_id, p.reaction_id);
          } else if (p.platform == "wechat") {
            wechat_send_reply(p.chat_id, reply_text);
          }
          std::cerr << "[gw] replied via " << p.platform << "\n";
        }
      } catch (...) {}
      break;
    }

    case MG_EV_CLOSE:
      std::cerr << "[agent] closed, will reconnect\n";
      if (g_agent_conn != nullptr) {
        g_agent_conn = nullptr;
        const int fin = g_agent_open ? finalize_pending_dropped("agent connection lost") : 0;
        g_agent_open = false;
        const int64_t delay = g_agent_backoff.next_delay_ms();
        g_agent_retry_at_ms = gw_now_ms() + delay;
        std::cerr << "[gw] agent disconnected (close, finalized=" << fin << "), retry in "
                  << delay << "ms\n";
      }
      break;
  }
}

static void agent_connect() {
  if (!g_mgr || g_agent_url.empty()) return;
  g_agent_conn = mg_ws_connect(g_mgr, g_agent_url.c_str(), agent_ws_fn, nullptr, nullptr);
}

// ══════════════════════════════════════════════════════
// 主循环
// ══════════════════════════════════════════════════════

static void on_timer(void*) {
  if (g_stop) return;

  if (!g_dns_configured) {
    if (!gw_apply_system_dns(g_mgr)) {
      std::cerr << "[gw] resolv.conf: no nameserver yet, retry...\n";
      if (!g_agent_conn && !g_agent_url.empty() && gw_now_ms() >= g_agent_retry_at_ms) {
        std::cerr << "[gw] agent reconnect attempt (backoff delay now "
                  << g_agent_backoff.pending_delay_ms() << "ms, cap 60000)\n";
        agent_connect();
      }
      return;
    }
  }

  if (!g_feishu_ws && !g_feishu_app_id.empty()) feishu_connect();
  // v0.54.1 (R88): **按退避时刻**重试（此前每 tick(3s) 无脑重连）
  if (!g_agent_conn && !g_agent_url.empty() && gw_now_ms() >= g_agent_retry_at_ms) {
    std::cerr << "[gw] agent reconnect attempt (backoff delay now "
              << g_agent_backoff.pending_delay_ms() << "ms, cap 60000)\n";
    agent_connect();
  }

  // 飞书 PING 心跳
  if (g_feishu_ws) feishu_ping();

  // 清理过期 pending（60s 超时）
  auto now = time(nullptr);
  g_pending_replies.erase(
      std::remove_if(g_pending_replies.begin(), g_pending_replies.end(),
                     [now](const PendingReply& p) {
                       if (now - p.created_at > 60) {
                         // 飞书：移除 Typing → 换成 CrossMark（表示超时/失败）
                         if (p.platform == "feishu" && !p.message_id.empty()) {
                           feishu_remove_reaction_sync(p.message_id, p.reaction_id);
                           feishu_add_reaction_sync(p.message_id, "CrossMark");
                         }
                         return true;
                       }
                       return false;
                     }),
      g_pending_replies.end());
}

static void on_signal(int) { g_stop = true; }

}  // namespace

#ifndef THIN_GATEWAY_AS_LIB
int main(int argc, char** argv) {
  // 将 stdout/stderr 重定向到带轮转的日志文件（10MB，每次 write 检测）
  thin_agent::RotatingLogBuf g_log_buf(thin_agent::default_agent_gw_log_path(),
                                        10ULL * 1024 * 1024);
  auto* g_old_cout_rdbuf = std::cout.rdbuf(&g_log_buf);
  auto* g_old_cerr_rdbuf = std::cerr.rdbuf(&g_log_buf);
  std::cout << std::unitbuf;   // 每次 << 后自动 flush，确保日志实时落盘
  std::cerr << std::unitbuf;

  std::string agent_url = "ws://127.0.0.1:8765/ws";

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--agent" && i + 1 < argc) agent_url = argv[++i];
  }

  const char* feishu_id = std::getenv("FEISHU_APP_ID");
  const char* feishu_secret = std::getenv("FEISHU_APP_SECRET");
  if (feishu_id) g_feishu_app_id = feishu_id;
  if (feishu_secret) g_feishu_app_secret = feishu_secret;

  const char* feishu_chat = std::getenv("FEISHU_CHAT_ID");
  if (feishu_chat) g_feishu_chat_id = feishu_chat;

  const char* wechat_bot = std::getenv("WECHAT_BOT_ID");
  const char* wechat_sec = std::getenv("WECHAT_SECRET");
  if (wechat_bot) g_wechat_bot_id = wechat_bot;
  if (wechat_sec) g_wechat_secret = wechat_sec;

  g_agent_url = agent_url;

  curl_global_init(CURL_GLOBAL_DEFAULT);
  g_send_worker = std::thread(send_worker);

  std::cout << "[thin_agent_gw] starting\n"
            << "[thin_agent_gw] agent=" << agent_url << "\n"
            << "[thin_agent_gw] feishu=" << (g_feishu_app_id.empty() ? "off" : "on")
            << " wechat=" << (g_wechat_bot_id.empty() ? "off" : "on") << "\n";

  std::thread wechat_thread(wechat_poll_loop);

  mg_mgr mgr;
  mg_mgr_init(&mgr);
  g_mgr = &mgr;

  gw_apply_system_dns(&mgr);
  gw_load_system_ca_bundle();
  agent_connect();
  if (g_dns_configured && !g_feishu_app_id.empty()) feishu_connect();

  mg_timer_add(&mgr, 3000, MG_TIMER_REPEAT, on_timer, nullptr);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::cout << "[thin_agent_gw] event loop running\n";
  while (!g_stop) mg_mgr_poll(&mgr, 200);

  std::cout << "[thin_agent_gw] shutting down\n";
  // v0.54.1 (R88): 停机同样收尾在途请求（v2 已修，v1 是孪生漏网）：否则"思考中"表情永久停留
  finalize_pending_dropped("gateway shutting down");
  mg_mgr_free(&mgr);
  g_mgr = nullptr;
  if (wechat_thread.joinable()) wechat_thread.join();
  {
    std::lock_guard<std::mutex> lock(g_send_mutex);
    g_send_stop = true;
  }
  g_send_cv.notify_one();
  if (g_send_worker.joinable()) g_send_worker.join();
  curl_global_cleanup();

  // v0.45.4: 恢复 cout/cerr 的 rdbuf。RotatingLogBuf 是 main 的局部对象，
  // 若不恢复，main 返回后全局 iostream 析构会 flush 悬垂 rdbuf → SIGSEGV
  // （实测每次退出必崩，gdb 栈: __cxa_finalize → ios_base::Init::~Init → flush → 0x0）。
  std::cout.rdbuf(g_old_cout_rdbuf);
  std::cerr.rdbuf(g_old_cerr_rdbuf);
  return 0;
}
#endif // THIN_GATEWAY_AS_LIB
