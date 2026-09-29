#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <curl/curl.h>
#include <deque>
#include <map>
#include <iostream>
#include <memory>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "mongoose.h"
#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/GatewaySessionMap.h"  // v0.52.28
#include "thin_agent/core/RequestDispatch.h"    // v0.53.82: 请求线程归属判据（默认异步）
#include "thin_agent/core/TaskEngine.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"
#if THIN_AGENT_WITH_FDBUS
#include "thin_agent/fdbus/FdbusDeviceControl.h"
#endif
#include "thin_agent/llm/DemoConfigCompat.h"
#include "thin_agent/log/RotatingLogger.h"
#include <sstream>   // v0.54.24: TA_LOG_LINE（整行单次写出）
#include "thin_agent/RuntimePaths.h"
#include "thin_agent/Version.h"
#include "thin_agent/agent/McpServer.h"


namespace {

// v0.54.24（R94 修复）：日志行**单次写出**。
// 背景：`std::cerr` 默认 unitbuf ⇒ `cerr << a << b << c << std::endl` 会**一行多次 write()**，
// 并发读者（e2e 断言、`tail -f`、日志采集器）因此可能读到**半行**——本轮实测到
// `… last_req=` 后换行再接 `ping` 的撕裂，并造成一条**假红**。约定与 include/thin_agent/log/LogEvent.h
// 一致：整行拼进 ostringstream，再用**一次** `cerr.write()` 输出（一次 xsputn ⇒ 一把锁 ⇒ 一次 fwrite）。
#define TA_LOG_LINE(expr)                                                                  \
  do {                                                                                     \
    std::ostringstream ta_oss;                                                             \
    ta_oss << expr;                                                                        \
    std::string ta_line = ta_oss.str();                                                    \
    if (ta_line.empty() || ta_line.back() != '\n') ta_line.push_back('\n');                 \
    std::cerr.write(ta_line.data(), static_cast<std::streamsize>(ta_line.size()));          \
  } while (0)

}  // namespace


// thin_agent WebSocket 入口：ws://host:port/ws
// IM 网关（飞书/微信长连接）见 thin_agent_gw（im_gateway_main.cpp）。

namespace {

// v0.53.87: 信号 handler 只许置 sig_atomic 标志（YY4 家族全仓收口；此前普通 bool）
volatile std::sig_atomic_t g_stop = 0;
std::unique_ptr<thin_agent::AgentService> g_agent;
// v0.26.1: chat-level sessions (persist across gateway reconnects)
// key = "chat_id"  or  "chat_id:thread_id"
// v0.52.28: 映射持久化（gateway_sessions.db）——重启后同 chat_id 仍映射
// 到原 sid，跨重启 resume（fc journal 按 sid 查）可达。open 失败退化纯内存。
std::unordered_map<std::string, std::string> g_chat_sessions;
thin_agent::GatewaySessionMap g_gw_sessions;
// Gateway-level hello sessions (per-connection, for initial handshake)
std::unordered_map<mg_connection*, std::string> g_conn_sessions;
std::set<mg_connection*> g_connections;      // v0.25.9: cron broadcast
uint64_t g_session_seq = 0;

// v0.47.2: WS 心跳 + 死连接清理
// mongoose mg_connection 无时间字段，自行维护最后活动时间戳。
// 每 30s 发一次应用层 heartbeat（TCP 断了 mg_ws_send 失败 → 自动 MG_EV_CLOSE）。
// v0.54.21: 原"超过 180s 无活动 → is_closing 清死连接"的收割器**已删除**（详见心跳 tick 处注释）：
// 它不可达（同 tick 先刷新全部连接再收割），且判据本身会误杀长任务静默客户端。
// v0.54.30: **只由客户端消息更新**（WS_OPEN 初始化 / MG_EV_WS_MSG 刷新）。`[ws-close]` 的 `idle_ms`
// 以此为准 ⇒ 含义 = "客户端静默了多久"。**不得**用心跳节拍或服务端发送去刷新它 —— 旧实现正是被
// 每 30s 心跳节拍刷新，导致静默 65s 的连接被报成 idle_ms≈10s（诊断字段撒谎，专项 e2e 实锤）。
std::unordered_map<mg_connection*, int64_t> g_conn_last_client_msg;  // ms, 客户端最后消息时刻
// v0.54.4 (R91): 归因用——记录"被心跳 reaper 主动标记关闭"的连接 id，
// 使 MG_EV_CLOSE 能回答"这次关闭是我们干的还是对端/网络干的"。
// 背景：R88 全量 -j4 下实测一次 `closedByServer=true` 但无任何服务端日志佐证，
// 归因不了（本文件此前 MG_EV_CLOSE 不打印任何原因，且没有 MG_EV_ERROR 分支）。
std::unordered_set<uint64_t> g_reaper_marked;
// v0.54.7 (R93): 每连接**最后一个请求类型**（仅事件循环线程读写，无须加锁）。
// 用途：A1 未定位项——"服务端主动关闭 connA"那两次，只知道"关闭被发现时即将发送的类型"，
// 不知道**关闭瞬间在飞的是哪个请求**。有了它，[ws-close]/[ws-ctl] 行直接点名在飞请求。
std::unordered_map<mg_connection*, std::string> g_conn_last_req;
static constexpr int64_t kHeartbeatIntervalMs = 30000;   // 30s 发一次心跳
int64_t g_last_heartbeat_ms = 0;

static int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ── v0.45.8: WS chat 异步化 ──
// ── v0.47.0: 单 worker → 多 worker 线程池 ──
// 背景：handle_request（chat FC 循环）同步阻塞 mongoose 事件循环，一个慢请求
// （云 LLM 数十秒）期间所有新连接/消息排队 → 连接超时、验证脚本回复串台。
// v0.45.8 修复：chat 请求入队由独立 worker 线程串行处理。
// v0.47.0 升级：单 worker → N worker 并发。不同 session 的请求可并行处理，
// 消除"一个慢请求阻塞所有后续请求"的瓶颈。AgentService 的并发安全基础已具备：
// session_chat_memory_ 有 mu_ 保护，SQLite 为 serialized mode，atomic 计数器。
// worker 数量由环境变量 THIN_AGENT_CHAT_WORKERS 控制，默认 min(4, max(2, cpu/2))。
struct PendingChat {
  unsigned long conn_id;  // v0.53.62: 连接 id(替代裸指针——worker 长任务
                          /// 期间连接关闭,job.c 悬垂读 c->id 同 UAF)
  std::string sid;
  nlohmann::json req;
  std::string cmd_id;  // v0.53.30: chunk 帧回带 cmd_id（多会话并发分流）
};
struct PendingSend {
  unsigned long id;  // v0.53.62: 连接 id(替代裸指针,NN5 同款悬垂面)
  std::string data;  // 预序列化，跨线程传递不搬动 nlohmann::json
};
std::mutex g_queue_mu;
std::condition_variable g_chat_cv;
std::deque<PendingChat> g_chat_queue;
// v0.53.37: 同 session 串行——zing v16 离线队列 350ms 错峰重发多条时,
// 服务端 FC 一条跑几十秒,第二条会被另一 worker 并发处理→记忆交错。
// 计数>0 表示该 sid 有 in-flight chat,worker 跳过取下一条。
static std::map<std::string, int> g_sid_inflight;
std::deque<PendingSend> g_send_queue;
std::vector<std::thread> g_chat_workers;     // v0.47.0: 多 worker 线程池
static constexpr size_t kMaxQueueSize = 128;  // v0.47.0 建 32 防 OOM；v0.53.82
// 判据反转后**所有**非即时请求共用此队列（原先只有 chat 类），32 会让一次
// 批量管理操作（仪表盘/脚本连发）撞"server busy"。128 条 JSON 请求仅占
// 百 KB 级，OOM 风险可忽略；真洪峰仍由上限+worker 池 per-sid 串行挡。
mg_mgr* g_mgr_ptr = nullptr;  // 仅 worker 线程写 mg_wakeup 用

// v0.53.32: WS/HTTP 握手鉴权（P0——远程 0.0.0.0 部署裸奔修复）。
// token 来源：--auth-token 参数 或 THIN_AGENT_AUTH_TOKEN env；空 = 鉴权关
// （本机 127.0.0.1 默认零配置不受影响）。校验形态：
//   WS:   ws://host:port/ws?token=xxx（upgrade 前校验）
//   HTTP: Authorization: Bearer xxx 或 ?token=xxx（/stats /webhook 等）
std::string g_auth_token;

static bool auth_ok(mg_http_message* hm) {
  if (g_auth_token.empty()) return true;  // 未配置 = 不启用
  // ① Authorization: Bearer <token>
  struct mg_str* auth_hdr = mg_http_get_header(hm, "Authorization");
  if (auth_hdr != nullptr) {
    const std::string v(auth_hdr->buf, auth_hdr->len);
    const std::string prefix = "Bearer ";
    if (v.rfind(prefix, 0) == 0 && v.substr(prefix.size()) == g_auth_token) return true;
  }
  // ② query 参数 token=（WS 握手唯一可用形态）
  char tbuf[256] = {0};
  if (mg_http_get_var(&hm->query, "token", tbuf, sizeof(tbuf)) > 0 &&
      g_auth_token == tbuf) return true;
  return false;
}

/// worker 线程：把结果/流式消息入队并唤醒事件循环（线程安全，不入 mongoose）
static constexpr size_t kMaxSendQueue = 4096;  // v0.53.37: 防半开连接堆积 OOM
void enqueue_send(unsigned long conn_id, const nlohmann::json& j) {
  std::string data = j.dump();
  {
    std::lock_guard<std::mutex> lk(g_queue_mu);
    g_send_queue.push_back(PendingSend{conn_id, std::move(data)});
    // v0.53.37: 半开连接(CLOSE 未触发)持续推送时上限自保——丢最老
    if (g_send_queue.size() > kMaxSendQueue) g_send_queue.pop_front();
  }
  // v0.53.62: mg_wakeup 第二参只影响唤醒 select 精度(传 0 全量唤醒),
  /// 传 conn_id 需活连接——id 无须解引用
  if (g_mgr_ptr) (void)mg_wakeup(g_mgr_ptr, 0, nullptr, 0);
}

/// 事件循环线程：把 worker 产生的消息真正发到 socket（仅本线程调 mongoose API）
void flush_pending_sends() {
  for (;;) {
    PendingSend ps;
    {
      std::lock_guard<std::mutex> lk(g_queue_mu);
      if (g_send_queue.empty()) return;
      ps = std::move(g_send_queue.front());
      g_send_queue.pop_front();
    }
    // v0.52.15b: 异步发送路径同样刷新活动时间——enqueue_send/flush
    // 与 send_json 是两条发送通道，长任务（分解波次）走异步路径，
    // 仅刷 send_json 拦不住 180s 静默踢除（真 e2e 复验 196s 仍被踢）。
    // v0.53.62: 按 id 定位活连接(NN5 同款)——ps.c 裸指针有 reuse 同址
    // 窗口:CLOSE erase 后新连接复用同地址,旧 pending 悬垂指针=新 c,
    /// g_connections.find 命中→旧会话数据发新连接=跨会话泄漏
    mg_connection* target = nullptr;
    if (g_mgr_ptr) {
      for (mg_connection* t = g_mgr_ptr->conns; t != nullptr; t = t->next) {
        if (t->id == ps.id) { target = t; break; }
      }
    }
    if (!target) continue;  // 已关闭(未找到即弃)
    if (g_connections.find(target) == g_connections.end()) continue;
    // v0.54.30: 不再刷新客户端活动时间（服务端主动发送 ≠ 客户端有活动；旧刷新是为已删除的 reaper 压枪）
    (void)mg_ws_send(target, ps.data.c_str(), ps.data.size(), WEBSOCKET_OP_TEXT);
    if (target->send.len > 0) {
      long n = mg_io_send(target, target->send.buf, target->send.len);
      if (n > 0) mg_iobuf_del(&target->send, 0, static_cast<size_t>(n));
    }
  }
}

/// worker 主循环：消费 chat 请求。v0.47.0: 多个 worker 线程并发消费同一队列，
/// 不同 session_id 的请求可并行处理（AgentService 内部 mu_ + SQLite serialized
/// 保证安全）。同一 session 的请求靠 gateway/client "发一条等回复" 的模式天然串行。
void chat_worker_loop() {
  for (;;) {
    PendingChat job;
    {
      std::unique_lock<std::mutex> lk(g_queue_mu);
      g_chat_cv.wait(lk, [] { return g_stop || !g_chat_queue.empty(); });
      // v0.53.93: 停机 = **立即停止消费**，不再"排空队列"。理由（实测缺陷）：
      // ①排空意味着停机要等完整个队列（FC/LLM 分钟级）②排空分支绕过同 session 串行
      // （直接取队首）③更致命的是**排空期间算出的结果送不出去**——主线程已把
      // g_mgr_ptr 置空，flush_pending_sends 里 `if (g_mgr_ptr)` 为假 → 白烧 CPU/配额。
      // 现在：在飞任务跑完即退（有界），排队任务由主线程回停机通告。
      if (g_stop) break;
      if (g_chat_queue.empty()) break;
      // v0.53.37: 同 session 串行——跳过 in-flight sid 的任务
      // v0.53.82: 判据提为共享函数（pick_unblocked_job）；并修停机忙等——
      // 原先 !picked 分支在 g_stop 时 wait_for(谓词=g_stop 即已满足)=立即返回,
      // 循环空转打满一核直到在飞任务结束(FC 分钟级=分钟级 100% CPU)
      std::vector<std::string> sids;
      sids.reserve(g_chat_queue.size());
      for (const auto& j : g_chat_queue) sids.push_back(j.sid);
      const size_t idx = thin_agent::pick_unblocked_job(sids, g_sid_inflight);
      if (idx == thin_agent::kNoPickableJob) {
        // 全部任务都是 in-flight sid(单 session 多条堆积)——等任一完成
        g_chat_cv.wait_for(lk, std::chrono::milliseconds(100),
                           [] { return g_stop; });
        continue;
      } else {
        auto it = g_chat_queue.begin() + static_cast<std::ptrdiff_t>(idx);
        job = std::move(*it);
        g_chat_queue.erase(it);
        ++g_sid_inflight[job.sid];
      }
    }
    try {
      thin_agent::StreamCallback on_chunk = [job](const std::string& chunk, bool done) {
        nlohmann::json f{{"type", "chat_chunk"}, {"chunk", chunk}, {"done", done}};
        if (!job.cmd_id.empty()) f["cmd_id"] = job.cmd_id;  // v0.53.30
        enqueue_send(job.conn_id, std::move(f));
      };
      thin_agent::EventCallback on_event = [job](const std::string& event_type,
                                                 const nlohmann::json& data) {
        auto ev = data;
        ev["type"] = event_type;
        enqueue_send(job.conn_id, ev);
      };
      auto result = g_agent->handle_request(job.sid, job.req, on_chunk, on_event);
      enqueue_send(job.conn_id, result);
      // v0.53.41: 回复已发——记忆入库移到分离线程(云端 embedding 秒级,
      // 同步会拖慢 worker 接单;此前普通 chat 从不 ingest,RAG 缺血)
      {
        const std::string utext = job.req.value("text", "");
        const std::string atext = result.value("text", "");
        if (g_agent && !utext.empty() && !atext.empty() &&
            result.value("type", "") == "chat_result") {
          std::thread([ag = g_agent.get(), sid = job.sid, utext, atext]() {
            try { ag->memory_ingest_conversation(sid, utext, atext); } catch (...) {}
          }).detach();
        }
      }
    } catch (const std::exception& e) {
      enqueue_send(job.conn_id, nlohmann::json{{"type", "error"}, {"message", e.what()}});
    }
    // v0.53.37: 释放 session in-flight 计数并唤醒等待的 worker
    {
      std::lock_guard<std::mutex> lk(g_queue_mu);
      auto it = g_sid_inflight.find(job.sid);
      if (it != g_sid_inflight.end() && --it->second <= 0) g_sid_inflight.erase(it);
    }
    g_chat_cv.notify_all();
  }
}

/// v0.26.1: Get or create a session for a chat/thread pair.
/// Sessions persist across WebSocket reconnects — chat context survives gateway restarts.
std::string ensure_chat_session(const std::string& chat_id, const std::string& thread_id) {
  if (chat_id.empty() && thread_id.empty()) {
    // Backward compat: no chat_id → fall back to connection-level session (global mixed)
    // This handles old gateways that don't send chat_id/thread_id
    static std::string fallback_sid;
    if (fallback_sid.empty()) {
      fallback_sid = "ws-" + std::to_string(++g_session_seq);
      if (g_agent) g_agent->on_session_open(fallback_sid);
    }
    return fallback_sid;
  }
  std::string key = thread_id.empty() ? chat_id : (chat_id + ":" + thread_id);
  auto it = g_chat_sessions.find(key);
  if (it != g_chat_sessions.end()) return it->second;
  // v0.52.28: 内存 miss → 查持久层（重启恢复）
  const std::string persisted = g_gw_sessions.lookup(key);
  if (!persisted.empty()) {
    g_chat_sessions[key] = persisted;
    // v0.53.28: 持久层恢复也触发 session_open（对齐新建分支——
    // v0.52.28 缺口：重启恢复的会话从不触发 session_start 钩子与
    // 记忆初始化；会话持久化回灌依赖此处）
    if (g_agent) g_agent->on_session_open(persisted);
    return persisted;
  }
  const std::string sid = "chat-" + std::to_string(++g_session_seq);
  g_chat_sessions[key] = sid;
  g_gw_sessions.assign(key, sid);  // v0.52.28: 新映射落盘
  if (g_agent) g_agent->on_session_open(sid);
  return sid;
}

void send_json(struct mg_connection* c, const nlohmann::json& j) {
  const std::string s = j.dump();
  // v0.52.15: 发送也刷新活动时间——死连接判定只认"收到客户端消息"
  //（MG_EV_WS_MSG），服务端发出的响应帧不刷新 → 长任务（波次>
  // 180s）期间静默等待的客户端被判死踢除，任务完成后响应发往已
  // 关连接，conclusion/gate 帧永久丢失（真 e2e 实测 180s 整被踢）。
  g_conn_last_client_msg[c] = now_ms();
  (void)mg_ws_send(c, s.c_str(), s.size(), WEBSOCKET_OP_TEXT);
  // 立即 flush send buffer：Mongoose 的 mg_send 对 TCP 只写 buffer 不写 socket，
  // 真正的 send() 只在事件循环收回控制权后才发生。这里手动 flush，
  // 确保 thinking 中间消息在 handle_request 同步阻塞期间立即到达客户端。
  if (c->send.len > 0) {
    long n = mg_io_send(c, c->send.buf, c->send.len);
    if (n > 0) mg_iobuf_del(&c->send, 0, (size_t)n);
  }
}

void fn(struct mg_connection* c, int ev, void* ev_data) {
  switch (ev) {
    case MG_EV_HTTP_MSG: {
      auto* hm = static_cast<mg_http_message*>(ev_data);
      // v0.53.32: /health 存活探针（帮助文本承诺过但从未实现——无凭据、零副作用）
      if (mg_match(hm->uri, mg_str("/health"), nullptr) &&
          mg_strcmp(hm->method, mg_str("GET")) == 0) {
        const std::string body = "{\"ok\":true,\"version\":\"" +
                                 std::string(thin_agent::kThinAgentVersion) + "\"}\n";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", body.c_str());
        break;
      }
      // v0.53.32: 鉴权闸（/health 除外——存活探针不应带凭据）
      if (!mg_match(hm->uri, mg_str("/health"), nullptr) && !auth_ok(hm)) {
        mg_http_reply(c, 401, "Content-Type: application/json\r\n",
                      "{\"error\":\"unauthorized: token required\"}\n");
        c->is_draining = 1;
        break;
      }
      if (mg_match(hm->uri, mg_str("/ws"), nullptr)) {
        mg_ws_upgrade(c, hm, nullptr);
        break;
      }
      // v0.29.0: webhook 端点 — 接收外部事件转化为 chat
      if (mg_match(hm->uri, mg_str("/webhook"), nullptr) &&
          mg_strcmp(hm->method, mg_str("POST")) == 0) {
        std::string body(hm->body.buf, hm->body.len);
        try {
          auto j = nlohmann::json::parse(body);
          std::string source = j.value("source", "unknown");
          std::string event  = j.value("event", "unknown");
          std::string payload_str = j.value("payload", j.dump());
          if (g_agent) {
            // v0.29.0: 非阻塞入队 — 不调用 handle_request，不阻塞 agent
            g_agent->enqueue_webhook(source, event, payload_str);
          }
          mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                        "{\"ok\":true}\n");
        } catch (const std::exception& e) {
          std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}\n";
          mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                        err.c_str());
        }
        break;
      }
      // v0.53.35: /metrics Prometheus 文本格式导出（抓取器用——鉴权闸内，
      // 生产建议 Authorization: Bearer <token> 抓取）
      if (mg_match(hm->uri, mg_str("/metrics"), nullptr) &&
          mg_strcmp(hm->method, mg_str("GET")) == 0) {
        if (g_agent) {
          const std::string body = g_agent->metrics_prometheus();
          mg_http_reply(c, 200, "Content-Type: text/plain; version=0.0.4\r\n",
                        body.c_str());
        } else {
          mg_http_reply(c, 503, "Content-Type: text/plain\r\n",
                        "agent not ready\n");
        }
        break;
      }
      // v0.50.8: /stats 运维统计口 — GET 汇总 version/uptime/usage/cache/cron
      // （只读、无副作用，毫秒级同步响应；数据源 ops_stats() 复用各子系统 stats）
      // v0.51.0: 加 CORS 头——ws_agent.html 以 file:// 或跨端口打开时 fetch 可达
      if (mg_match(hm->uri, mg_str("/stats"), nullptr) &&
          mg_strcmp(hm->method, mg_str("GET")) == 0) {
        if (g_agent) {
          std::string body = g_agent->ops_stats().dump() + "\n";
          mg_http_reply(c, 200,
                        "Content-Type: application/json\r\n"
                        "Access-Control-Allow-Origin: *\r\n",
                        body.c_str());
        } else {
          mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                        "{\"error\":\"agent not ready\"}\n");
        }
        break;
      }
      mg_http_reply(c, 404, "Content-Type: text/plain\r\n", "thin_agent ws server\n");
      break;
    }
    case MG_EV_WS_OPEN: {
      g_connections.insert(c);  // v0.25.9: track for broadcast
      g_conn_last_client_msg[c] = now_ms();  // v0.47.2: 初始化活动时间
      // v0.26.1: gateway-level hello session (not chat-scoped)
      std::string gw_sid = "gw-" + std::to_string(++g_session_seq);
      g_conn_sessions[c] = gw_sid;
      if (g_agent) {
        g_agent->on_session_open(gw_sid);
        send_json(c, g_agent->hello(gw_sid));
      }
      break;
    }
    case MG_EV_WS_MSG: {
      g_conn_last_client_msg[c] = now_ms();  // v0.47.2: 更新活动时间
      try {
        auto* wm = static_cast<mg_ws_message*>(ev_data);
        std::string msg(reinterpret_cast<const char*>(wm->data.buf), wm->data.len);
        nlohmann::json req;
        try {
          req = nlohmann::json::parse(msg);
        } catch (const std::exception& pe) {
          fprintf(stderr, "[WS-PARSE-ERROR] msg len=%zu first_byte=0x%02x err=%s\n",
                  msg.size(), msg.empty() ? 0 : (unsigned char)msg[0], pe.what());
          send_json(c, {{"type", "error"}, {"message", std::string("parse: ") + pe.what()}});
          break;
        }

        // v0.26.1: per-chat session isolation via chat_id + thread_id
        std::string chat_id = req.value("chat_id", "");
        std::string thread_id = req.value("thread_id", "");
        const std::string sid = ensure_chat_session(chat_id, thread_id);

        if (!g_agent) {
          send_json(c, {{"type", "error"}, {"message", "agent not ready"}});
          break;
        }
        // v0.45.8: chat 异步化 — 入队由 worker 线程处理，事件循环不再被
        // FC 循环/云 LLM 阻塞（一个慢请求不再卡住所有新连接/消息）。
        // v0.47.0: 多 worker 并发消费，队列超限返回 503。
        // v0.52.9/v0.53.19/v0.53.59: 长跑型请求（分解/编排/spawn）逐类并入。
        // v0.53.82: **判据反转**——手维护的"谁重"清单穷举必然漏（家族第 5 次
        // 复发：chat_approve/agent_decompose/cron_reply/switch_model/summarize/
        // task_submit 六处漏网，全部内联跑在事件循环里 = 心跳停发+全连接冻结）。
        // 改为默认入队，仅 is_instant_ws_request 白名单（ping/status/abort/
        // 指标类，毫秒级）留在事件循环；未知类型默认入队（fail-safe）。
        const std::string req_type = req.value("type", "");
        g_conn_last_req[c] = req_type;  // v0.54.7: 关闭归因用（在飞请求类型）
        const bool is_long_running = thin_agent::ws_request_needs_worker(req_type);
        if (is_long_running) {
          {
            std::lock_guard<std::mutex> lk(g_queue_mu);
            if (g_chat_queue.size() >= kMaxQueueSize) {  // v0.47.0: 队列满
              send_json(c, {{"type", "error"},
                            {"message", "server busy: chat queue full"},
                            {"retry_after_ms", 5000}});
              break;
            }
            g_chat_queue.push_back(PendingChat{c->id, sid, req,
                                               req.value("cmd_id", "")});
          }
          g_chat_cv.notify_one();
          break;
        }
        // 非 chat 的 WS API（checkpoint/cron/skills 等）毫秒级，保持同步
        // 流式回调：逐 token 推送 chat_chunk 消息
        const std::string sync_cmd_id = req.value("cmd_id", "");  // v0.53.30
        thin_agent::StreamCallback on_chunk = [c, sync_cmd_id](const std::string& chunk, bool done) {
          nlohmann::json cf{{"type", "chat_chunk"}, {"chunk", chunk}, {"done", done}};
          if (!sync_cmd_id.empty()) cf["cmd_id"] = sync_cmd_id;
          send_json(c, std::move(cf));
        };
        // 事件回调：推送 thinking / progress 中间状态
        thin_agent::EventCallback on_event = [c](const std::string& event_type, const nlohmann::json& data) {
          // v0.54.6: 改名 ev_payload——此前 `ev` 与外层 switch 分支的 `int ev` 同名（-Wshadow=local 判官命中）
          auto ev_payload = data;
          ev_payload["type"] = event_type;
          send_json(c, ev_payload);
        };
        send_json(c, g_agent->handle_request(sid, req, on_chunk, on_event));
      } catch (const std::exception& e) {
        send_json(c, {{"type", "error"}, {"message", e.what()}});
      }
      break;
    }
    case MG_EV_WAKEUP:
      // v0.45.8: worker 线程入队结果后经 mg_wakeup 唤醒，在此真正发送
      flush_pending_sends();
      break;
    case MG_EV_POLL: {
      // v0.45.8: 兜底 flush（wakeup 可能因时序丢失，poll 幂等）
      flush_pending_sends();

      // v0.47.2: 应用层心跳 + 死连接清理
      int64_t ts = now_ms();
      if (ts - g_last_heartbeat_ms >= kHeartbeatIntervalMs) {
        g_last_heartbeat_ms = ts;
        // 发心跳（TCP 断了 mg_ws_send 写失败 → mongoose 自动标记 close）
        const std::string hb =
            nlohmann::json({{"type", "heartbeat"}, {"ts", ts}}).dump();
        for (auto* conn : g_connections) {
          (void)mg_ws_send(conn, hb.c_str(), hb.size(), WEBSOCKET_OP_TEXT);
          if (conn->send.len > 0) {
            long n = mg_io_send(conn, conn->send.buf, conn->send.len);
            if (n > 0) mg_iobuf_del(&conn->send, 0, (size_t)n);
          }
          // v0.54.30: 此处**不再**刷新客户端活动时间 —— 心跳是**服务端**发出，与"客户端是否沉默"
          // 无关；旧实现每次心跳节拍刷新它，使 `idle_ms` 变成"距上次心跳"（静默 65s 报 10s）。
          // 死连接的真实信号仍是"写失败 ⇒ mongoose 标记 close"（上面的 send 分支）。
        }
        // ── v0.54.21（拍板：删除）：180s「死连接收割」不在此处 —— 该防线已按静态证明删除 ──
        // 删除依据（**勿凭"多加一道兜底更安全"再加回来**）：
        //   ① **不可达**（v0.54.15 静态证明）：本 tick 先把全部 `g_connections` 成员刷成 ts（上面循环），
        //      **紧接着**才收割 ⇒ `ts - last` 对候选恒为 0、候选集恒空；
        //   ② **判据本身是错的**：它判"180s 无*任何*活动"，而长任务期间客户端**天然静默** ——
        //      v0.52.15b 真 e2e 实测 196s 被误踢、conclusion/gate 帧永久丢失。现在之所以没误杀，
        //      正是靠上面那个刷新压住它 ⇒ "把它修成可达" = 退回误杀；
        //   ③ **真实防线已存在且更好**：心跳写出失败 ⇒ mongoose 标记 close（覆盖 RST/半开连接）；
        //      对端静默消失时，心跳每 30s 的写入会让内核 TCP 重传超时（`tcp_retries2` 默认 ~15min）
        //      触发写失败，从而由 ③ 收口。该路径的观测覆盖：e2e_ws_close_attribution（FIN/RST 探针）
        //      + e2e_ws_heartbeat_liveness（30s 心跳节拍 + 静默活连接不被误踢）。
        // **未覆盖的缺口（如实登记，未做）**：TCP 连接活着但**客户端应用冻结**（不回包也不读该 socket）
        //   —— 要抓它只能做**双向 liveness**（要求客户端回 pong，连续 N 次无 pong 才收割），
        //   属协议级改动（要同时改 frontends/ws_agent.html 与 CLI 客户端），见 CHANGELOG v0.54.21。
        // v0.54.30 更新说明：`g_conn_last_client_msg` 现在**只由客户端消息更新**，故 [ws-close] 的
        //   `idle_ms` = **客户端静默时长**（此前被本 tick 刷新 ⇒ 恒 ≤30s，属诊断字段撒谎，已修）；
        //   `g_reaper_marked`
        // 仍是 v0.54.15 关闭归因签名表的 `by_us(reaper)` 字段（现**恒 0** = "reaper 未参与"，
        // e2e_ws_close_attribution 断言该字段存在且为 0）。
      }
      break;
    }
    case MG_EV_ERROR: {
      // v0.54.4 (R91): 此前**没有** ERROR 分支——socket/协议错误完全无痕，故障只能靠猜。
      const char* msg = ev_data ? static_cast<const char*>(ev_data) : "";
      TA_LOG_LINE("[ws-error] id=" << c->id << " msg=" << (msg ? msg : "")
                                   << " last_req="
                                   << (g_conn_last_req.count(c) ? g_conn_last_req[c] : "-"));
      break;
    }
    case MG_EV_WS_CTL: {
      // v0.54.7 (R93): **对端控制帧归因（补上 A1 缺的另一半）**。
      // 关键机制：mongoose 收到对端 CLOSE 帧时会 echo 回去并置 `is_draining = 1`
      // （mongoose.c 的 WEBSOCKET_OP_CLOSE 分支）⇒ 随后的 MG_EV_CLOSE 报的也是
      // `is_closing=1 by_us(reaper)=0`，与"服务端自己关"**签名完全相同**，无法区分。
      // 于是 R86/R88 那两次"服务端主动关闭 connA"的结论始终无法证实——本分支把对端的
      // 关闭码与原因落盘：下次偶发里若出现 [ws-ctl] peer_close 行，即证明**是对端先关**。
      auto* wm = static_cast<mg_ws_message*>(ev_data);
      const uint8_t op = static_cast<uint8_t>(wm->flags & 15);
      if (op == 8) {  // WEBSOCKET_OP_CLOSE
        int code = -1;
        std::string reason;
        if (wm->data.len >= 2) {
          code = (static_cast<unsigned char>(wm->data.buf[0]) << 8) |
                 static_cast<unsigned char>(wm->data.buf[1]);
          if (wm->data.len > 2) reason.assign(wm->data.buf + 2, wm->data.len - 2);
        }
        TA_LOG_LINE("[ws-ctl] peer_close id=" << c->id << " code=" << code
                                             << " reason=" << reason.substr(0, 120)
                                             << " last_req="
                                             << (g_conn_last_req.count(c) ? g_conn_last_req[c] : "-")
                                             << " send.len=" << c->send.len);
      }
      break;
    }
    case MG_EV_CLOSE: {
      // v0.54.4 (R91): **关闭归因**——回答"谁关的"。此前无日志：R88 那次
      // `closedByServer=true` 在服务端完全查不到痕迹。
      {
        auto la = g_conn_last_client_msg.find(c);
        const int64_t idle_ms = la == g_conn_last_client_msg.end() ? -1 : (now_ms() - la->second);
        TA_LOG_LINE("[ws-close] id=" << c->id
                                     << " by_us(reaper)=" << (g_reaper_marked.count(c->id) ? 1 : 0)
                                     << " is_closing=" << c->is_closing
                                     << " is_draining=" << c->is_draining
                                     << " send.len=" << c->send.len
                                     << " idle_ms=" << idle_ms
                                     << " last_req="
                                     << (g_conn_last_req.count(c) ? g_conn_last_req[c] : "-"));
        g_reaper_marked.erase(c->id);
        g_conn_last_req.erase(c);  // v0.54.7: 防 map 无界增长
      }
      g_connections.erase(c);  // v0.25.9: cleanup broadcast tracking
      g_conn_last_client_msg.erase(c);  // v0.47.2: cleanup heartbeat tracking
      // v0.45.8: 丢弃该连接的待发送/待处理消息（连接已失效）
      {
        std::lock_guard<std::mutex> lk(g_queue_mu);
        for (auto it = g_send_queue.begin(); it != g_send_queue.end();) {
          if (it->id == c->id) it = g_send_queue.erase(it);  // v0.53.62: id
          else ++it;
        }
        for (auto it = g_chat_queue.begin(); it != g_chat_queue.end();) {
          if (it->conn_id == c->id) it = g_chat_queue.erase(it);  // v0.53.62
          else ++it;
        }
      }
      // v0.26.1: only close gateway-level hello session — chat sessions persist
      auto it = g_conn_sessions.find(c);
      if (it != g_conn_sessions.end()) {
        if (g_agent) g_agent->on_session_close(it->second);
        g_conn_sessions.erase(it);
      }
      break;
    }
    default:
      break;
  }
}

// v0.25.9: broadcast event to all connected WS clients (for cron notifications etc.)
void broadcast_json(const nlohmann::json& j) {
  const std::string s = j.dump();
  for (auto* c : g_connections) {
    (void)mg_ws_send(c, s.c_str(), s.size(), WEBSOCKET_OP_TEXT);
  }
}

void on_signal(int) { g_stop = true; }

}  // namespace

int main(int argc, char** argv) {
  // --help 在日志重定向之前处理，确保输出到终端
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      std::cout << "thin_agent " << thin_agent::kThinAgentVersion << "\n\n"
                << "Usage: thin_agent [options]\n\n"
                << "Options:\n"
                << "  --host <ip>       Listen address (default: 127.0.0.1)\n"
                << "  --auth-token <t>  Require token for WS/HTTP (or THIN_AGENT_AUTH_TOKEN env)\n"
                << "  --port <port>     Listen port (default: 8765)\n"
                << "  --config <path>   YAML model config (default: ~/.thin_agent/demo.model.yaml)\n"
                << "  --profile <name>  Profile name in config (default: offline_demo)\n"
                << "  --mcp             Run as MCP server (stdio JSON-RPC)\n"
                << "  -h, --help        Show this help\n\n"
                << "Environment:\n"
                << "  THIN_AGENT_DEV_MODE=1   Enable developer mode (shell_exec blacklist)\n"
                << "  THIN_AGENT_AUTO_MODE=1  Local-first cascade before cloud\n\n"
                << "WebSocket endpoint: ws://<host>:<port>/ws\n"
                << "Health check:       http://<host>:<port>/health\n"
                << std::endl;
      return 0;
    }
    if (a == "--mcp") {
      // MCP Server 模式：stdio JSON-RPC，不启动 Mongoose WS 服务
      std::string mcp_config = thin_agent::default_demo_model_config_path();
      std::string mcp_profile = "offline_demo";
      for (int j = i + 1; j < argc; ++j) {
        std::string aa = argv[j];
        if (aa == "--config" && j + 1 < argc) mcp_config = argv[++j];
        else if (aa == "--profile" && j + 1 < argc) mcp_profile = argv[++j];
      }
      try {
        auto cfg = thin_agent::load_demo_profile_compat(mcp_config, mcp_profile);
        auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
        auto executor = std::make_shared<thin_agent::ActionExecutor>(dc);
        auto task_engine = std::make_shared<thin_agent::TaskEngine>(executor);
        auto agent_svc = std::make_unique<thin_agent::AgentService>(
            cfg, executor, task_engine);
        thin_agent::agent::McpServer mcp_server(agent_svc->skill_registry());
        mcp_server.set_server_info("thin_agent",
            std::string(thin_agent::kThinAgentVersion));
        mcp_server.run_stdio();
      } catch (const std::exception& e) {
        TA_LOG_LINE("[mcp] init failed: " << e.what());
        return 1;
      }
      return 0;
    }
  }
  // 将 stdout/stderr 重定向到带轮转的日志文件（20MB，每次 write 检测）
  // 必须放在所有 std::cout/cerr 之前
  thin_agent::RotatingLogBuf g_log_buf(thin_agent::default_agent_svc_log_path(),
                                        20ULL * 1024 * 1024);
  // v0.51.5: libcurl 全局初始化——多线程使用 libcurl 前必须调用（与
  // im_gateway_main 同款）。缺失时 TLS 状态不确定，偶发 200+空响应体
  // （编程验证实测：同请求直连正常、服务内 curl 偶发空 body 触发
  // empty_response_body 离线降级）。
  curl_global_init(CURL_GLOBAL_DEFAULT);
  auto* g_old_cout_rdbuf = std::cout.rdbuf(&g_log_buf);
  auto* g_old_cerr_rdbuf = std::cerr.rdbuf(&g_log_buf);
  std::cout << std::unitbuf;   // 每次 << 后自动 flush，确保日志实时落盘
  std::cerr << std::unitbuf;

  // RAII guard：析构时恢复 cout/cerr 原始 rdbuf，防止 static destruction
  // 阶段 std::ios_base::Init::~Init() flush 时访问已释放的 g_log_buf（SIGSEGV）。
  // 声明在 g_log_buf 之后 → 析构在 g_log_buf 之前 → 恢复后 static 析构安全。
  struct RdbufRestorer {
    std::streambuf* cout_buf;
    std::streambuf* cerr_buf;
    ~RdbufRestorer() {
      std::cout.rdbuf(cout_buf);
      std::cerr.rdbuf(cerr_buf);
    }
  } _rdbuf_restorer{g_old_cout_rdbuf, g_old_cerr_rdbuf};

  std::string host = "127.0.0.1";
  int port = 8765;
  std::string config = thin_agent::default_demo_model_config_path();
  std::string profile = "offline_demo";

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string& v) {
      if (i + 1 < argc) v = argv[++i];
    };
    if (a == "--host") next(host);
    else if (a == "--port") {
      std::string p;
      next(p);
      port = std::stoi(p);
    } else if (a == "--config") {
      next(config);
    } else if (a == "--profile") {
      next(profile);
    } else if (a == "--auth-token") {
      next(g_auth_token);  // v0.53.32: WS/HTTP 鉴权（空=关）
    }
  }
  // v0.53.32: env 亦可注入（systemd 单元/远程 start.sh 用）
  if (g_auth_token.empty()) {
    if (const char* env = std::getenv("THIN_AGENT_AUTH_TOKEN"); env && env[0]) g_auth_token = env;
  }
  if (!g_auth_token.empty()) {
    std::cout << "[ws_agent_cpp] auth: ENABLED (token len=" << g_auth_token.size() << ")\n";
  }

  try {
    auto cfg = thin_agent::load_demo_profile_compat(config, profile);
    thin_agent::thin_agent_set_profile(profile);  // v0.43.0: 隔离 data_dir/config_dir
    // 环境变量覆盖：THIN_AGENT_AUTO_MODE=1 → 本地优先级联路由
    if (const char* env = std::getenv("THIN_AGENT_AUTO_MODE"); env && env[0] == '1') {
      cfg.mode = "auto";
    }
#if THIN_AGENT_WITH_FDBUS
    auto dc = std::make_shared<thin_agent::FdbusDeviceControl>();
    std::cout << "[ws_agent_cpp] device_control=FdbusDeviceControl\n";
#else
    auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
    std::cout << "[ws_agent_cpp] device_control=FakeDeviceControl\n";
#endif
    auto executor = std::make_shared<thin_agent::ActionExecutor>(dc);
    auto task_engine = std::make_shared<thin_agent::TaskEngine>(executor);
    if (!task_engine->init(thin_agent::default_task_db_path())) {
      std::cerr << "fatal: task engine init failed\n";
      return 3;
    }
    g_agent = std::make_unique<thin_agent::AgentService>(cfg, executor, task_engine);
    // v0.53.25: WS 队列统计注入（/stats 聚合——实时状态卡的队列深度）
    g_agent->set_ws_stats_provider([]() -> nlohmann::json {
      std::lock_guard<std::mutex> lk(g_queue_mu);
      return nlohmann::json{
          {"queue_depth", g_chat_queue.size()},
          {"workers", g_chat_workers.size()},
          {"queue_capacity", kMaxQueueSize}};
    });
    g_agent->set_broadcast_callback([](const nlohmann::json& j) { broadcast_json(j); });

    // v0.52.30: hooks.json 启动加载——shell 钩子免编译接入。
    // 格式: [{"event":"tool_pre","shell_cmd":"...","tool_filter":["rm*"],"timeout_ms":5000}]
    {
      const std::string hooks_path = thin_agent::default_data_dir() + "/hooks.json";
      std::ifstream hf(hooks_path);
      if (hf.good()) {
        try {
          auto arr = nlohmann::json::parse(hf);
          int loaded = 0;
          if (arr.is_array()) {
            for (const auto& h : arr) {
              thin_agent::HookRegistration reg;
              reg.event = thin_agent::hook_event_from_string(
                  h.value("event", ""));
              if (reg.event == thin_agent::HookEvent::MessageIn &&
                  h.value("event", "") != "message_in")
                continue;  // 解析失败默认值=MessageIn，需甄别
              if (h.contains("tool_filter") && h["tool_filter"].is_array())
                for (const auto& t : h["tool_filter"])
                  if (t.is_string())
                    reg.tool_filter.push_back(t.get<std::string>());
              reg.shell_cmd = h.value("shell_cmd", "");
              reg.shell_timeout_ms = h.value("timeout_ms", 5000);
              if (reg.shell_cmd.empty()) continue;
              thin_agent::HookSystem::instance().register_hook(std::move(reg));
              ++loaded;
            }
          }
          TA_LOG_LINE("[ws_agent_cpp] hooks.json loaded: " << loaded << " hooks");
        } catch (const std::exception& e) {
          TA_LOG_LINE("[ws_agent_cpp] warn: hooks.json parse failed: " << e.what());
        }
      }
    }

    // v0.52.28: 会话映射持久化——重启后同 chat_id 找回原 sid
    //（跨重启 resume 的最后一公里）。open 失败退化为纯内存（旧行为）。
    if (!g_gw_sessions.open(thin_agent::default_data_dir()
                                + "/gateway_sessions.db")) {
      TA_LOG_LINE("[ws_agent_cpp] warn: gateway session map open failed,"
                    " session mapping is memory-only");
    } else {
      TA_LOG_LINE("[ws_agent_cpp] session map restored: " << g_gw_sessions.size()
                                                          << " mappings");
    }

    // v0.52.4: 启动断点扫描——active 且带分解 meta 的父目标 = 上次
    // 分解任务未完成（服务重启/网断中断）。只报告不自动续跑（续跑须
    // 用户显式 agent_decompose_resume，避免重启即烧 token）。
    {
      auto resume = g_agent->handle_request("",
          nlohmann::json{{"type", "agent_decompose_scan"}});
      if (resume.value("resumable", 0) > 0) {
        std::cout << "[ws_agent_cpp] 断点提示: " << resume.value("resumable", 0)
                  << " 个未完成分解任务，agent_decompose_resume 恢复\n";
      }
    }

    std::cout << "[ws_agent_cpp] config=" << config << " profile=" << profile << "\n";
    std::cout << "[ws_agent_cpp] mode=" << cfg.mode << " provider=" << cfg.provider
              << " model=" << cfg.model_name << " fallback=" << cfg.fallback << "\n";
    std::cout << "[ws_agent_cpp] version=" << thin_agent::kThinAgentVersion << "\n";
    std::cout << "[ws_agent_cpp] listen ws://" << host << ":" << port << "/ws\n";

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    mg_mgr mgr;
    mg_mgr_init(&mgr);
    g_mgr_ptr = &mgr;  // v0.45.8: worker 线程 mg_wakeup 用

    const std::string url = "http://" + host + ":" + std::to_string(port);
    if (mg_http_listen(&mgr, url.c_str(), fn, nullptr) == nullptr) {
      TA_LOG_LINE("failed to listen on " << url);
      g_mgr_ptr = nullptr;
      mg_mgr_free(&mgr);
      return 2;
    }

    // v0.45.8: 启动 chat worker 线程（异步处理，事件循环不再被阻塞）
    // v0.47.0: 启动 N 个 worker 并发消费队列
    {
      unsigned num_workers = 0;
      if (const char* env = std::getenv("THIN_AGENT_CHAT_WORKERS")) {
        num_workers = static_cast<unsigned>(std::max(1, std::atoi(env)));
      } else {
        unsigned hw = std::thread::hardware_concurrency();
        num_workers = std::min(4u, std::max(2u, hw / 2));
      }
      g_chat_workers.reserve(num_workers);
      for (unsigned i = 0; i < num_workers; ++i) {
        g_chat_workers.emplace_back(chat_worker_loop);
      }
      std::cout << "[ws_agent_cpp] chat_workers=" << num_workers << "\n";
    }

    while (!g_stop) {
      mg_mgr_poll(&mgr, 100);
    }

    // v0.53.93 停机收尾（顺序修正）：g_mgr_ptr 必须**保持有效**直到"通告 + 最终 flush"
    // 完成。旧代码在此处提前置空 → flush_pending_sends 的 `if (g_mgr_ptr)` 为假：
    // ①在飞任务刚算出的结果（worker 已 enqueue）②写给排队任务的停机通告 全部被静默丢弃
    // （停机期间白烧 CPU/LLM）。置空只为"防 worker 悬垂投递"，而 worker 已 join 完毕，
    // 因此把它移到通告/flush 之后。
    g_chat_cv.notify_all();
    for (auto& w : g_chat_workers) {
      if (w.joinable()) w.join();
    }
    g_chat_workers.clear();

    // 未开始处理的排队请求：回一帧带 cmd_id 的停机通告（客户端不再只有 R76 的
    // "连接中断"兜底文案，而是拿到明确原因），并记一条可观测日志。
    std::deque<PendingChat> leftover;   // 与 g_chat_queue 同型（deque）
    {
      std::lock_guard<std::mutex> lk(g_queue_mu);
      leftover = std::move(g_chat_queue);   // 先在锁内搬出，避免持锁调 enqueue_send（同锁=自死锁）
      g_chat_queue.clear();
    }
    for (const auto& job : leftover) {
      nlohmann::json notice{{"type", "error"},
                            {"code", 29001},
                            {"message", "server shutting down: request not processed"}};
      if (!job.cmd_id.empty()) notice["cmd_id"] = job.cmd_id;
      enqueue_send(job.conn_id, notice);
    }
    if (!leftover.empty()) {
      std::cout << "[ws_agent_cpp] shutdown: dropped " << leftover.size()
                << " queued request(s), clients notified\n";
    }

    flush_pending_sends();  // 停机通告 + 在飞任务结果（此时 g_mgr_ptr 仍有效）
    g_mgr_ptr = nullptr;    // worker 已全部退出 → 此刻起防悬垂
    mg_mgr_free(&mgr);
    return 0;
  } catch (const std::exception& e) {
    // v0.45.0: 启动失败（如 profile not found）时恢复 stderr 到终端，
    // 避免错误只进日志文件导致用户看不到原因。
    std::cerr.rdbuf(g_old_cerr_rdbuf);
    TA_LOG_LINE("fatal: " << e.what());
    return 1;
  }
}
