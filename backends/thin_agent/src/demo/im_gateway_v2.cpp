/// thin_agent Gateway v2 — Multi-platform IM Gateway
///
/// 使用 PlatformAdapter 抽象接口管理多平台连接。
/// 架构：main loop → adapters → forward_message → agent WS → dispatch reply → adapter
///
/// 支持平台（按环境变量配置）：
///   - 飞书: FEISHU_APP_ID + FEISHU_APP_SECRET
///   - 微信: WECHAT_BOT_ID + WECHAT_SECRET
///   - Telegram: TELEGRAM_BOT_TOKEN

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "mongoose.h"

#include "thin_agent/gateway/HttpObservability.h"  // v0.53.83: 发送可观测
#include "thin_agent/gateway/ReconnectBackoff.h"  // v0.53.99: 重连退避策略
#include "thin_agent/gateway/SendQueuePolicy.h"  // v0.53.86: 发送队列有界
#include "thin_agent/RuntimePaths.h"
#include "thin_agent/api/PlatformAdapter.h"
#include "thin_agent/log/RotatingLogger.h"

// ── 前向声明 FeishuAdapter（定义在 FeishuAdapter.cpp 中）──
namespace thin_agent {
class FeishuAdapter;
class WechatAdapter;
class TelegramAdapter;
}  // namespace thin_agent

namespace {

// v0.53.86: 信号 handler 只许置 sig_atomic 标志（YY4 家族收口——此前是普通 bool，
// handler 里赋值 + 主循环/worker 读取 = 非信号安全的竞态/UB；thin_agentd 早已改）
volatile std::sig_atomic_t g_stop = 0;
struct mg_mgr* g_mgr = nullptr;
std::string g_dns4_url_storage;
bool g_dns_configured = false;
std::string g_tls_ca_bundle;

// ── thin_agent core 连接 ──
struct mg_connection* g_agent_conn = nullptr;
std::string g_agent_url;
// v0.53.99 (R86): agent 重连退避状态。此前 on_timer 每 3s 无脑重试（无退避）——
// agent 长时间不可用时连接风暴 + 日志刷屏，永不收敛。
// 基准/封顶可用环境变量覆盖（**可测性钩子**：e2e 用 400ms/3200ms 在几秒内验证档位递增，
// 否则默认 3s→60s 需要 45s+ 才能观察到 4 档）。
thin_agent::ReconnectBackoff g_agent_backoff{
    getenv("THIN_AGENT_GW_RECONNECT_BASE_MS") ? atoll(getenv("THIN_AGENT_GW_RECONNECT_BASE_MS")) : 3000,
    getenv("THIN_AGENT_GW_RECONNECT_CAP_MS") ? atoll(getenv("THIN_AGENT_GW_RECONNECT_CAP_MS")) : 60000};
int64_t g_agent_retry_at_ms = 0;   // steady_clock 毫秒；0=立即可试
bool    g_agent_open = false;      // 本连接是否真的 open 过（区分"连上后断开"与"连不上"）

static int64_t gw_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ── 平台适配器 ──
std::vector<std::unique_ptr<thin_agent::PlatformAdapter>> g_platforms;

// ── 待回复 ──
struct PendingReply {
  std::string message_id;
  std::string chat_id;
  std::string thread_id;
  thin_agent::PlatformAdapter* platform = nullptr;
  time_t created_at;
  std::string reaction_id;
  std::string lang = "zh-CN";
  std::string last_thinking_msg_id;
};
std::vector<PendingReply> g_pending_replies;
std::mutex g_pending_mutex;

// ── 发送队列（单线程保序）──
struct QueuedSend {
  std::string url;
  std::string body;
  std::vector<std::string> headers;
  std::string method;
  int seq = 0;
};
std::queue<QueuedSend> g_send_queue;
std::mutex g_send_mutex;
std::condition_variable g_send_cv;
std::thread g_send_worker;
bool g_send_stop = false;
int g_enqueue_seq = 0;

// ══════════════════════════════════════════════════════
// HTTP / DNS / TLS 基础设施
// ══════════════════════════════════════════════════════

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

static std::string gw_http_post(const std::string& url, const std::string& body,
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
  static const char* ca[] = {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                             "/etc/ssl/cert.pem", nullptr};
  for (const char** p = ca; *p; ++p) {
    std::ifstream ifs(*p);
    if (ifs.good()) { curl_easy_setopt(curl, CURLOPT_CAINFO, *p); break; }
  }
  // v0.53.83: 统一观测（孪生拷贝——v1 im_gateway_main 早已捕获 rc，v2 漏网）
  thin_agent::gateway::curl_perform_observed(curl, "gateway_v2", url);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  return response;
}

static std::string gw_http_delete(const std::string& url,
                                  const std::vector<std::string>& headers = {},
                                  int timeout_ms = 5000) {
  CURL* curl = curl_easy_init();
  if (!curl) return "";
  std::string response;
  struct curl_slist* hdrs = nullptr;
  for (const auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  // v0.53.83: 统一观测（孪生拷贝——v1 im_gateway_main 早已捕获 rc，v2 漏网）
  thin_agent::gateway::curl_perform_observed(curl, "gateway_v2", url);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);
  return response;
}

static bool gw_load_system_ca_bundle() {
  if (!g_tls_ca_bundle.empty()) return true;
  static const char* paths[] = {"/etc/ssl/certs/ca-certificates.crt",
                                "/etc/pki/tls/certs/ca-bundle.crt",
                                "/etc/ssl/cert.pem", nullptr};
  for (const char** p = paths; *p; ++p) {
    std::ifstream ifs(*p, std::ios::binary);
    if (!ifs.is_open()) continue;
    g_tls_ca_bundle.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    if (!g_tls_ca_bundle.empty()) return true;
  }
  return false;
}

static void gw_ws_tls_maybe_init(struct mg_connection* c, int ev, void* ev_data) {
  if (ev != MG_EV_OPEN || !ev_data) return;
  const char* url = static_cast<const char*>(ev_data);
  if (!mg_url_is_ssl(url)) return;
  gw_load_system_ca_bundle();
  struct mg_tls_opts opts = {};
  opts.name = mg_url_host(url);
  opts.ca = g_tls_ca_bundle.empty() ? mg_str("")
           : mg_str_n(g_tls_ca_bundle.data(), g_tls_ca_bundle.size());
  mg_tls_init(c, &opts);
}

static bool gw_apply_system_dns(struct mg_mgr* mgr) {
  if (!mgr) return false;
  std::ifstream ifs("/etc/resolv.conf");
  if (!ifs.is_open()) return false;
  std::string line;
  while (std::getline(ifs, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (line.compare(0, 11, "nameserver ") != 0) continue;
    std::string ip = line.substr(11);
    while (!ip.empty() && std::isspace(static_cast<unsigned char>(ip.front()))) ip.erase(ip.begin());
    while (!ip.empty() && std::isspace(static_cast<unsigned char>(ip.back()))) ip.pop_back();
    if (ip.empty()) continue;
    g_dns4_url_storage = (ip.find(':') != std::string::npos) ? "udp://[" + ip + "]:53"
                                                             : "udp://" + ip + ":53";
    mgr->dns4.url = g_dns4_url_storage.c_str();
    g_dns_configured = true;
    return true;
  }
  return false;
}

// ══════════════════════════════════════════════════════
// 发送队列 work线程
// ══════════════════════════════════════════════════════

static void enqueue_send(std::string url, std::string body,
                         std::vector<std::string> headers, std::string method = "POST") {
  int seq = ++g_enqueue_seq;
  std::lock_guard<std::mutex> lock(g_send_mutex);
  // v0.53.86: 有界（此前无上限 → 平台变慢/回复洪峰时无界增长）。
  // 满了丢**最旧**而不是拒绝新：用户最近的话最可能要回复；丢弃可观测。
  if (thin_agent::gateway::send_queue_needs_drop(g_send_queue.size())) {
    g_send_queue.pop();
    static int dropped_total = 0;
    ++dropped_total;
    std::cerr << "[gw] send queue full (cap=" << thin_agent::gateway::kSendQueueCap
              << "), dropped oldest total=" << dropped_total << "\n";
  }
  g_send_queue.push({std::move(url), std::move(body), std::move(headers), std::move(method), seq});
  g_send_cv.notify_one();
}

static void send_worker() {
  while (true) {
    std::unique_lock<std::mutex> lock(g_send_mutex);
    g_send_cv.wait(lock, [] { return !g_send_queue.empty() || g_send_stop; });
    if (g_send_stop && g_send_queue.empty()) break;
    if (g_send_queue.empty()) continue;
    auto q = std::move(g_send_queue.front());
    g_send_queue.pop();
    lock.unlock();
    if (q.method == "DELETE") gw_http_delete(q.url, q.headers);
    else (void)gw_http_post(q.url, q.body, q.headers);
  }
}

// ══════════════════════════════════════════════════════
// 消息转发到 Agent
// ══════════════════════════════════════════════════════

static void forward_to_agent(const std::string& text, const std::string& message_id,
                             const std::string& chat_id, const std::string& thread_id,
                             thin_agent::PlatformAdapter* platform) {
  if (!g_agent_conn) {
    // v0.53.99 (R86): 此前只写一行日志 → **用户消息静默丢失**（用户看不到任何反馈）。
    // 现在给消息打 CrossMark，让"没被处理"对用户可见（R76 律：请求级状态必须可见收尾）。
    std::cerr << "[gw] agent not connected — dropping user message, user notified (CrossMark)\n";
    if (platform && !message_id.empty()) platform->add_reaction(message_id, "CrossMark");
    return;
  }

  nlohmann::json payload;
  payload["type"] = "chat";
  payload["text"] = text;
  payload["message_id"] = message_id;
  payload["chat_id"] = chat_id;
  payload["thread_id"] = thread_id;
  payload["platform"] = platform ? platform->name() : "unknown";

  std::string payload_str = payload.dump();
  mg_ws_send(g_agent_conn, payload_str.c_str(), payload_str.size(), WEBSOCKET_OP_TEXT);

  // 添加 pending + reaction
  {
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    PendingReply pr;
    pr.message_id = message_id;
    pr.chat_id = chat_id;
    pr.thread_id = thread_id;
    pr.platform = platform;
    pr.created_at = time(nullptr);
    if (platform) pr.reaction_id = platform->add_reaction(message_id, "Typing");
    // v0.53.74: pending 上限 1024(与 v1 同哲学——超时只清到期,洪峰
    /// 窗内无界堆积=OOM 面;vector 版丢最旧=erase begin)
    if (g_pending_replies.size() >= 1024) g_pending_replies.erase(g_pending_replies.begin());
    g_pending_replies.push_back(std::move(pr));
  }
  std::cerr << "[gw] → agent: " << text.substr(0, 60) << "\n";
}

// ══════════════════════════════════════════════════════
// Agent WS 客户端
// ══════════════════════════════════════════════════════

// v0.53.99 (R86): **连接级收尾**（R76 律：请求级 UI 状态必须有连接级收尾）。
// 此前 agent 连接断掉时只把 g_agent_conn 置空：在途请求继续挂在 pending 里，
// 用户要等到 60s 超时扫描才看到一个**无声**的 CrossMark（期间没有任何提示）；
// 停机路径更是完全不管（表情永久停留）。
// 现在：断线/停机都立刻把在途请求收尾（撤思考表情 + 打 CrossMark + 计数日志）。
static int finalize_pending_dropped(const char* reason) {
  std::vector<PendingReply> dropped;
  {
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    dropped.swap(g_pending_replies);
  }
  for (auto& p : dropped) {
    if (p.platform) {
      p.platform->remove_reaction(p.message_id, p.reaction_id);
      p.platform->add_reaction(p.message_id, "CrossMark");
    }
  }
  if (!dropped.empty()) {
    std::cerr << "[gw] finalized " << dropped.size()
              << " in-flight request(s), users notified: " << reason << "\n";
  }
  return static_cast<int>(dropped.size());
}

static void agent_ws_fn(struct mg_connection* c, int ev, void* ev_data) {
  gw_ws_tls_maybe_init(c, ev, ev_data);
  if (ev != MG_EV_WS_MSG) {
    if (ev == MG_EV_WS_OPEN) {
      // v0.53.99: 连接成功 → 退避复位（下次断线从 3s 重新开始）+ 可观测
      g_agent_backoff.reset();
      g_agent_retry_at_ms = 0;
      g_agent_open = true;
      std::cerr << "[gw] agent connected\n";
      return;
    }
    if (ev == MG_EV_ERROR || ev == MG_EV_CLOSE) {
      if (g_agent_conn != nullptr) {  // 仅在"确实掉了"时收尾/排期（ERROR+CLOSE 可能都来）
        g_agent_conn = nullptr;
        const int finalized = g_agent_open ? finalize_pending_dropped("agent connection lost") : 0;
        g_agent_open = false;
        const int64_t delay = g_agent_backoff.next_delay_ms();
        g_agent_retry_at_ms = gw_now_ms() + delay;
        std::cerr << "[gw] agent disconnected (http=" << (ev == MG_EV_ERROR ? "error" : "close")
                  << ", finalized=" << finalized << "), retry in " << delay << "ms\n";
      }
      return;
    }
    return;
  }

  auto* wm = static_cast<mg_ws_message*>(ev_data);
  std::string msg(static_cast<const char*>(wm->data.buf), wm->data.len);
  try {
    auto resp = nlohmann::json::parse(msg);
    std::string msg_type = resp.value("type", "");

    // cron_notify → 广播到所有平台
    if (msg_type == "cron_notify") {
      std::string cron_text = resp.value("text", "");
      if (!cron_text.empty()) {
        for (auto& plat : g_platforms) {
          if (!plat->is_enabled()) continue;
          // Feishu 用 chat_id，其他平台用空（走各自的默认逻辑）
          plat->send_chat("", "[Cron] " + cron_text);
        }
      }
      return;
    }

    // thinking → 透传到对应平台
    if (msg_type == "thinking") {
      std::string thinking_msg = resp.value("msg", "");
      if (thinking_msg.empty()) return;
      std::lock_guard<std::mutex> lock(g_pending_mutex);
      if (!g_pending_replies.empty()) {
        // v0.53.61: 按 chat_id 匹配(EE5 同款)——agent 多 worker 乱序
        /// 完成时 front()=thinking 发错会话
        const std::string rchat = resp.value("chat_id", "");
        auto pit = g_pending_replies.begin();
        if (!rchat.empty()) {
          pit = std::find_if(g_pending_replies.begin(), g_pending_replies.end(),
                             [&](const PendingReply& p) { return p.chat_id == rchat; });
          if (pit == g_pending_replies.end()) pit = g_pending_replies.begin();
        }
        auto& p = *pit;
        if (p.platform) p.platform->send_chat(p.chat_id, thinking_msg, p.thread_id);
      }
      return;
    }

    // 普通回复
    std::string reply_text = resp.value("text", "");
    if (reply_text.empty()) return;

    PendingReply p;
    {
      std::lock_guard<std::mutex> lock(g_pending_mutex);
      if (g_pending_replies.empty()) return;
      // v0.53.61: 按 chat_id 匹配(EE5 同款)+作用域结束自然解锁——
      /// 此前 lock.~lock_guard() 手动析构=作用域尾二次析构 UB
      const std::string rchat = resp.value("chat_id", "");
      auto pit = g_pending_replies.begin();
      if (!rchat.empty()) {
        // v0.54.6: lambda 形参改名 pr——此前 `p` 与外层 PendingReply 变量同名（-Wshadow=local 判官命中）
        pit = std::find_if(g_pending_replies.begin(), g_pending_replies.end(),
                           [&](const PendingReply& pr) { return pr.chat_id == rchat; });
        if (pit == g_pending_replies.end()) pit = g_pending_replies.begin();
      }
      p = *pit;
      g_pending_replies.erase(pit);
    }

    if (p.platform) {
      if (!p.message_id.empty()) p.platform->send_reply(p.message_id, reply_text);
      else p.platform->send_chat(p.chat_id, reply_text, p.thread_id);
      p.platform->remove_reaction(p.message_id, p.reaction_id);
    }
    std::cerr << "[gw] replied via " << (p.platform ? p.platform->name() : "?") << "\n";
  } catch (...) {}
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
    if (!gw_apply_system_dns(g_mgr)) return;
  }

  // 各平台维护
  for (auto& plat : g_platforms) {
    if (plat->is_enabled()) plat->on_timer();
  }

  // Agent 重连（v0.53.99: **按退避时刻**重试；此前每 tick(3s) 无脑重连）
  if (!g_agent_conn && !g_agent_url.empty()) {
    const int64_t now_ms = gw_now_ms();
    if (now_ms >= g_agent_retry_at_ms) {
      std::cerr << "[gw] agent reconnect attempt (backoff delay now "
                << g_agent_backoff.pending_delay_ms() << "ms, cap 60000)\n";
      agent_connect();
    }
  }

  // 清理过期 pending（60s 超时）
  auto now = time(nullptr);
  {
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    g_pending_replies.erase(
        std::remove_if(g_pending_replies.begin(), g_pending_replies.end(),
                       [now](PendingReply& p) {
                         if (now - p.created_at > 60) {
                           if (p.platform) {
                             p.platform->remove_reaction(p.message_id, p.reaction_id);
                             p.platform->add_reaction(p.message_id, "CrossMark");
                           }
                           return true;
                         }
                         return false;
                       }),
        g_pending_replies.end());
  }
}

static void on_signal(int) { g_stop = true; }

}  // namespace

// ══════════════════════════════════════════════════════
// 平台适配器工厂函数（定义在各 adapter .cpp 中）
// ══════════════════════════════════════════════════════
namespace thin_agent {
std::unique_ptr<PlatformAdapter> create_feishu_adapter();
std::unique_ptr<PlatformAdapter> create_wechat_adapter();
std::unique_ptr<PlatformAdapter> create_telegram_adapter();
}  // namespace thin_agent

#ifndef THIN_GATEWAY_AS_LIB
int main(int argc, char** argv) {
  thin_agent::RotatingLogBuf log_buf(thin_agent::default_agent_gw_log_path(), 10ULL * 1024 * 1024);
  auto* old_cout = std::cout.rdbuf(&log_buf);
  auto* old_cerr = std::cerr.rdbuf(&log_buf);
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  std::string agent_url = "ws://127.0.0.1:8765/ws";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--agent" && i + 1 < argc) agent_url = argv[++i];
  }
  g_agent_url = agent_url;

  curl_global_init(CURL_GLOBAL_DEFAULT);
  g_send_worker = std::thread(send_worker);

  // ── 注册平台适配器 ──
  {
    auto feishu = thin_agent::create_feishu_adapter();
    if (feishu && feishu->is_enabled()) {
      feishu->set_message_callback(
          [](const std::string& text, const std::string& msg_id, const std::string& chat_id,
             const std::string& thread_id, thin_agent::PlatformAdapter* plat) {
            forward_to_agent(text, msg_id, chat_id, thread_id, plat);
          });
      g_platforms.push_back(std::move(feishu));
    }
  }
  {
    auto wechat = thin_agent::create_wechat_adapter();
    if (wechat && wechat->is_enabled()) {
      wechat->set_message_callback(
          [](const std::string& text, const std::string& msg_id, const std::string& chat_id,
             const std::string& thread_id, thin_agent::PlatformAdapter* plat) {
            forward_to_agent(text, msg_id, chat_id, thread_id, plat);
          });
      g_platforms.push_back(std::move(wechat));
    }
  }
  {
    auto tg = thin_agent::create_telegram_adapter();
    if (tg && tg->is_enabled()) {
      tg->set_message_callback(
          [](const std::string& text, const std::string& msg_id, const std::string& chat_id,
             const std::string& thread_id, thin_agent::PlatformAdapter* plat) {
            forward_to_agent(text, msg_id, chat_id, thread_id, plat);
          });
      g_platforms.push_back(std::move(tg));
    }
  }

  std::cout << "[thin_agent_gw] starting\n"
            << "[thin_agent_gw] agent=" << agent_url << "\n"
            << "[thin_agent_gw] platforms=" << g_platforms.size() << "\n";

  mg_mgr mgr;
  mg_mgr_init(&mgr);
  g_mgr = &mgr;

  gw_apply_system_dns(&mgr);
  gw_load_system_ca_bundle();
  agent_connect();

  // 各平台连接
  for (auto& plat : g_platforms) {
    if (plat->is_enabled()) plat->connect(&mgr);
  }

  mg_timer_add(&mgr, 3000, MG_TIMER_REPEAT, on_timer, nullptr);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::cout << "[thin_agent_gw] event loop running\n";
  while (!g_stop) mg_mgr_poll(&mgr, 200);

  std::cout << "[thin_agent_gw] shutting down\n";
  // v0.53.99: 停机同样要收尾在途请求（否则表情永久停留、用户零反馈）
  finalize_pending_dropped("gateway shutting down");
  for (auto& plat : g_platforms) plat->disconnect();
  mg_mgr_free(&mgr);
  g_mgr = nullptr;

  {
    std::lock_guard<std::mutex> lock(g_send_mutex);
    g_send_stop = true;
  }
  g_send_cv.notify_one();
  if (g_send_worker.joinable()) g_send_worker.join();
  curl_global_cleanup();

  std::cout.rdbuf(old_cout);
  std::cerr.rdbuf(old_cerr);
  return 0;
}
#endif
