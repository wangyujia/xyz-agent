// thin_agent C ABI 实现 — AgentService 包装层
//
// 将 C++ AgentService 包裹在不透明 C 句柄中，
// 对外暴露纯 C 接口，内部使用完整 C++ 实现。

#include "thin_agent/api/agent_api.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#ifndef _WIN32
#include <unistd.h>  // v0.53.65: MSVC 无此头——且本文件未用其符号,隔离
#endif

#include <nlohmann/json.hpp>

#include "mongoose.h"
#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/RequestDispatch.h"  // v0.53.82: 同 session 串行判据
#include "thin_agent/core/TaskEngine.h"
#include "thin_agent/fdbus/FakeDeviceControl.h"
#if THIN_AGENT_WITH_FDBUS
#include "thin_agent/fdbus/FdbusDeviceControl.h"
#endif
#include "thin_agent/llm/DemoConfigCompat.h"
#include "thin_agent/RuntimePaths.h"
#include "thin_agent/Version.h"

namespace {

// ── 内部实现结构 ──

// v0.53.44: chat 任务(事件线程入队,worker 池消费)
struct ChatJob {
    mg_connection* c;
    std::string sid;
    nlohmann::json req;
};

struct thin_agent_impl {
    thin_agent_config_t cfg;
    thin_agent::DemoConfigCompat demo_cfg;

    std::shared_ptr<thin_agent::ActionExecutor> executor;
    std::shared_ptr<thin_agent::TaskEngine>     task_engine;
    std::unique_ptr<thin_agent::AgentService>   service;

    std::unique_ptr<mg_mgr> mgr;
    std::thread             event_thread;
    std::atomic<bool>       running{false};
    int                     actual_ws_port = 0;

    mutable std::mutex  error_mutex;
    std::string         last_error;

    thin_agent_log_fn log_cb = nullptr;
    void*             log_user_data = nullptr;
    std::mutex        log_mu;  // v0.53.97: log_cb/user_data 的一致快照锁

    // v0.53.44: chat worker 池——此前 handle_request 在事件循环线程同步
    // 跑,FC 循环(LLM 秒级~分钟)期间 /health 不响应、其他连接全冻结、
    // ping 不回→客户端判死断连(W1)。2 worker+队列,析构 drain。
    // 发送走 pending 队列+mg_wakeup(mg_ws_send 只允许事件线程)。
    struct PendingSend {
        unsigned long id;  // v0.53.61: 连接 id(替代裸指针,防悬垂)
        std::string data;
    };
    std::deque<ChatJob> chat_queue;
    // v0.53.82: 同 session 串行计数（对齐 ws_agent_main v0.53.37）——此前本
    // 服务端缺这道闸：同一连接快速连发两条 chat 会被两个 worker 并发取走，
    // 一条 FC 跑几十秒期间第二条并发进入 = session 记忆/槽位交错 + 回复乱序
    // （孪生拷贝律：两个 WS 服务端各写一份调度必然漂移，故判据提为
    //  thin_agent::pick_unblocked_job 共享函数）。chat_mu 保护。
    std::map<std::string, int> sid_inflight;
    std::deque<PendingSend> send_queue;
    std::mutex chat_mu;
    std::condition_variable chat_cv;
    std::vector<std::thread> chat_workers;
    std::atomic<bool> chat_running{false};
    std::atomic<uint64_t> sid_seq{0};  // v0.53.44: sid 撞车防护(W2)
    mg_mgr* mgr_raw = nullptr;         // worker 线程 mg_wakeup 用(事件线程设置)

    // v0.53.44: 连接→稳定 sid 映射(指针值可能重用→撞车串台,W2)
    std::map<mg_connection*, std::string> conn_sids;
    std::string conn_sid(mg_connection* c) {
        std::lock_guard<std::mutex> lk(chat_mu);
        auto it = conn_sids.find(c);
        if (it != conn_sids.end()) return it->second;
        std::string sid = "ws-" + std::to_string(sid_seq.fetch_add(1) + 1);
        conn_sids[c] = sid;
        return sid;
    }
    std::string take_conn_sid(mg_connection* c) {
        std::lock_guard<std::mutex> lk(chat_mu);
        auto it = conn_sids.find(c);
        if (it == conn_sids.end()) return {};
        std::string sid = it->second;
        conn_sids.erase(it);
        return sid;
    }

    void enqueue_send(mg_connection* c, const std::string& data) {
        {
            std::lock_guard<std::mutex> lk(chat_mu);
            if (send_queue.size() > 4096) send_queue.pop_front();  // 半开连接自保
            // v0.53.61: 存 id 而非裸指针——worker 长任务期间连接关闭,
            /// flush 侧 mg_ws_send(ps.c) 读悬垂连接=UAF 面;id 定位+存活性检查
            send_queue.push_back(PendingSend{c->id, data});
        }
        if (mgr_raw) (void)mg_wakeup(mgr_raw, c->id, nullptr, 0);
    }
    std::mutex& send_mu() { return chat_mu; }
    void push_send_locked(mg_connection* c, const std::string& data) {
        // 调用方已持 chat_mu(如 WS_MSG 的队列满分支)
        if (send_queue.size() > 4096) send_queue.pop_front();
        send_queue.push_back(PendingSend{c->id, data});  // v0.53.61
    }

    void flush_sends() {
        // v0.53.44: 网络 IO 绝不持锁——mg_ws_send 向慢/死 TCP 写会阻塞,
        /// 持 chat_mu 时=事件线程永久卡死(连 conn_sid 都进不来)
        std::deque<PendingSend> batch;
        {
            std::lock_guard<std::mutex> lk(chat_mu);
            batch.swap(send_queue);
        }
        for (auto& ps : batch) {
            // v0.53.61: 按 id 找活连接——找不到(已关)即弃,防悬垂发送
            mg_connection* c = nullptr;
            if (mgr_raw) {
                for (mg_connection* t = mgr_raw->conns; t != nullptr; t = t->next) {
                    if (t->id == ps.id) { c = t; break; }
                }
            }
            if (c && !c->is_closing) {
                mg_ws_send(c, ps.data.c_str(), ps.data.size(), WEBSOCKET_OP_TEXT);
            }
        }
    }

    void chat_worker_loop() {
        while (true) {
            ChatJob job;
            {
                std::unique_lock<std::mutex> lk(chat_mu);
                chat_cv.wait(lk, [&] { return !chat_running || !chat_queue.empty(); });
                // v0.53.97: 停机 = **立即停止消费**（不再"排空队列"，对齐 R81 的 ws_agent_main
                // 修复——本文件此前是同一缺陷的孪生漏网）。三点理由：
                //   ①stop() 旧顺序先 join 事件线程 → 投递通道已死 → 排空期间算出的结果全部丢弃
                //     （白烧 CPU/LLM；embedder 只看到断连）
                //   ②排空分支把同 session 串行让路（``idx = 0`` 直接取队首）——v0.53.82 立的铁律
                //     被停机路径绕过
                //   ③chat_queue 上限 1024 × FC 分钟级 → stop() 被拖成小时级（embedder 的 UI
                //     线程调用 stop 即卡死）
                // 现在：在途任务跑完即退（有界），未开始的排队任务由 stop() 回停机通告。
                if (!chat_running) return;
                if (chat_queue.empty()) return;
                std::vector<std::string> sids;
                sids.reserve(chat_queue.size());
                for (const auto& j : chat_queue) sids.push_back(j.sid);
                std::size_t idx = thin_agent::pick_unblocked_job(sids, sid_inflight);
                if (idx == thin_agent::kNoPickableJob) {
                    // 同 session 在飞——等任一完成再试（不阻塞其他 worker）
                    // v0.53.97: 停机时 wait_for 的谓词立即满足 → 回到循环顶部的
                    // `if (!chat_running) return;` 正常退出（不再有"停机排空"支路）
                    chat_cv.wait_for(lk, std::chrono::milliseconds(100),
                                     [&] { return !chat_running; });
                    continue;
                }
                auto it = chat_queue.begin() + static_cast<std::ptrdiff_t>(idx);
                job = std::move(*it);
                chat_queue.erase(it);
                ++sid_inflight[job.sid];
            }
            thin_agent::StreamCallback on_chunk =
                [this, c = job.c](const std::string& chunk, bool done) {
                    nlohmann::json r;
                    r["type"] = "chat_chunk";
                    r["chunk"] = chunk;
                    r["done"] = done;
                    enqueue_send(c, r.dump());
                };
            thin_agent::EventCallback on_event =
                [this, c = job.c](const std::string& t, const nlohmann::json& d) {
                    auto ev = d;
                    ev["type"] = t;
                    enqueue_send(c, ev.dump());
                };
            nlohmann::json resp;
            try {
                resp = service->handle_request(job.sid, job.req, on_chunk, on_event);
            } catch (const std::exception& e) {
                resp = {{"type", "error"}, {"message", e.what()}};
            }
            enqueue_send(job.c, resp.dump());
            {
                std::lock_guard<std::mutex> lk(chat_mu);
                auto it = sid_inflight.find(job.sid);
                if (it != sid_inflight.end() && --it->second <= 0) sid_inflight.erase(it);
            }
            chat_cv.notify_all();
        }
    }

    void set_error(const std::string& msg) {
        std::lock_guard<std::mutex> lk(error_mutex);
        last_error = msg;
    }
    // v0.53.97: 头文件承诺 set_log_callback "线程安全"，但此前 log()/set 均无同步 →
    // worker 线程可能在 log_cb 刚换、log_user_data 未换的窗口读到**错配**的 (cb,ud)
    // 组合（嵌入方按旧 ud 解引用 = 崩），且属数据竞争(UB)。改：加 log_mu 取**一致快照**，
    // 且**锁外**调用回调（回调内若再调本 API，持锁即自死锁面）。 */
    void log(int level, const char* msg) {
        thin_agent_log_fn cb = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lk(log_mu);
            cb = log_cb;
            ud = log_user_data;
        }
        if (cb) cb(level, msg, ud);
    }
};

// Mongoose 3-arg 签名: void fn(mg_connection *c, int ev, void *ev_data)
//   c->fn_data  = 用户数据（thin_agent_impl*），由 mg_http_listen 第4参数设置
//   ev_data     = 事件特定数据（mg_http_message* / mg_ws_message* 等）

void ws_event_handler(mg_connection* c, int ev, void* ev_data) {
    auto* impl = static_cast<thin_agent_impl*>(c->fn_data);
    if (!impl || !impl->service) return;

    switch (ev) {
        case MG_EV_HTTP_MSG: {
            fprintf(stderr, "[DBG] HTTP_MSG\n");
            auto* hm = static_cast<mg_http_message*>(ev_data);
            if (mg_match(hm->uri, mg_str("/ws"), nullptr)) {
                mg_ws_upgrade(c, hm, nullptr);
            } else if (mg_match(hm->uri, mg_str("/health"), nullptr)) {
                mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                    "{\"status\":\"ok\",\"service\":\"thin_agent\"}\n");
            } else if (mg_match(hm->uri, mg_str("/info"), nullptr)) {
                char* info = thin_agent_get_info_json(
                    reinterpret_cast<thin_agent_t*>(impl));
                mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                    "%s\n", info);
                thin_agent_free_string(info);
            } else {
                mg_http_reply(c, 404, "", "Not Found\n");
            }
            break;
        }
        case MG_EV_WS_OPEN: {
            std::string sid = impl->conn_sid(c);
            impl->service->on_session_open(sid);
            auto hello = impl->service->hello(sid);
            std::string s = hello.dump();
            mg_ws_send(c, s.c_str(), s.size(), WEBSOCKET_OP_TEXT);
            break;
        }
        case MG_EV_WS_MSG: {
            // v0.53.44: chat 投递 worker 池——事件线程不再被 FC 循环卡死
            auto* wm = static_cast<mg_ws_message*>(ev_data);
            std::string msg(reinterpret_cast<const char*>(wm->data.buf),
                           wm->data.len);

            try {
                auto req = nlohmann::json::parse(msg);
                // v0.53.44 修:conn_sid 内部也 lock(chat_mu)——std::mutex
                // 不可重入,嵌套锁=自死锁(首条 chat 即挂,实测 gdb 实锤)。
                // sid 解析移到外层锁之前。
                std::string sid = impl->conn_sid(c);
                {
                    std::lock_guard<std::mutex> lk(impl->chat_mu);
                    if (impl->chat_queue.size() > 1024) {
                        // 队列自保——恶意洪泛直接拒(锁内不调 enqueue_send
                        //——它也要 chat_mu,同类陷阱)
                        nlohmann::json errj{{"type", "error"},
                                            {"message", "chat queue full"}};
                        std::string es = errj.dump();
                        std::lock_guard<std::mutex> lk2(impl->send_mu());
                        impl->push_send_locked(c, es);
                        break;
                    }
                    impl->chat_queue.push_back(
                        ChatJob{c, std::move(sid), std::move(req)});
                }
                impl->chat_cv.notify_one();
            } catch (const std::exception& e) {
                nlohmann::json err;
                err["type"] = "error";
                err["message"] = std::string("parse error: ") + e.what();
                std::string s = err.dump();
                mg_ws_send(c, s.c_str(), s.size(), WEBSOCKET_OP_TEXT);
            }
            break;
        }
        case MG_EV_WAKEUP: {
            // v0.53.44: worker 入队结果后唤醒,事件线程真正发送
            impl->flush_sends();
            break;
        }
        case MG_EV_POLL:
            // v0.53.44: 兜底 flush(wakeup 可能因时序丢失,poll 幂等)
            impl->flush_sends();
            break;
        case MG_EV_CLOSE: {
            std::string sid = impl->take_conn_sid(c);
            impl->service->on_session_close(sid);
            break;
        }
        default:
            break;
    }
}

void event_loop_run(thin_agent_impl* impl) {
    impl->log(2, "thin_agent event loop started");
    while (impl->running) {
        mg_mgr_poll(impl->mgr.get(), 100);
    }
    // v0.53.97 尾刷：停机前把 send_queue 里残余（在途任务结果 + 停机通告）真正写出去。
    // 旧实现直接退出循环 → 这些消息随 mg_mgr_free 一起消失（这就是"停机丢结果"的
    // 另一半：即使结果入了队也没人发）。
    impl->flush_sends();
    // v0.53.97: mg_ws_send 只把数据排进 conn->send——**真正写 socket 要靠 mongoose
    // poll**。停机尾刷若只 flush_sends 不 poll，通告/在途结果仍留在缓冲区里随
    // mg_mgr_free 一起消失（"以为发了其实没发"）。故驱动若干次 0ms poll，并在其间
    // 再 flush（worker 退出前最后一批可能刚入队）。
    for (int i = 0; i < 3; ++i) {
        mg_mgr_poll(impl->mgr.get(), 0);
        impl->flush_sends();
    }
    impl->log(2, "thin_agent event loop stopped");
}

std::string get_hostname() {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf)) == 0) return buf;
    return "unknown";
}

const char* os_name() {
#ifdef _WIN32
    return "Windows";
#elif __APPLE__
    return "macOS";
#else
    return "Linux";
#endif
}

const char* cpu_arch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#else
    return "unknown";
#endif
}

}  // namespace

// ══════════════════════════════════════════════════════
// C ABI 导出
// ══════════════════════════════════════════════════════

extern "C" {

THIN_API thin_agent_t* thin_agent_create(const thin_agent_config_t* config) {
    if (!config) return nullptr;

    auto* impl = new thin_agent_impl();
    impl->cfg = *config;

    const char* yaml_path = config->config_yaml ? config->config_yaml
                                                 : "config/demo.model.yaml";
    const char* profile = config->profile ? config->profile : "default";

    try {
        impl->demo_cfg = thin_agent::load_demo_profile_compat(yaml_path, profile);
    } catch (const std::exception& e) {
        impl->set_error(std::string("config load failed: ") + e.what());
        delete impl;
        return nullptr;
    }

#if THIN_AGENT_WITH_FDBUS
    auto dc = std::make_shared<thin_agent::FdbusDeviceControl>();
#else
    auto dc = std::make_shared<thin_agent::FakeDeviceControl>();
#endif
    impl->executor = std::make_shared<thin_agent::ActionExecutor>(dc);

    impl->task_engine = std::make_shared<thin_agent::TaskEngine>(impl->executor);
    if (!impl->task_engine->init(thin_agent::default_task_db_path())) {
        impl->set_error("task engine init failed");
        delete impl;
        return nullptr;
    }

    try {
        impl->service = std::make_unique<thin_agent::AgentService>(
            impl->demo_cfg, impl->executor, impl->task_engine);
    } catch (const std::exception& e) {
        impl->set_error(std::string("agent service create failed: ") + e.what());
        delete impl;
        return nullptr;
    }

    impl->log(2, "thin_agent instance created");
    return reinterpret_cast<thin_agent_t*>(impl);
}

THIN_API int thin_agent_start(thin_agent_t* agent) {
    if (!agent) return -1;
    auto* impl = reinterpret_cast<thin_agent_impl*>(agent);
    if (impl->running) return 0;

    int port = impl->cfg.ws_port > 0 ? impl->cfg.ws_port : 8765;
    const char* bind_addr = impl->cfg.ws_bind_addr ? impl->cfg.ws_bind_addr
                                                    : "127.0.0.1";

    impl->mgr = std::make_unique<mg_mgr>();
    mg_mgr_init(impl->mgr.get());
    // v0.53.44: worker→事件线程的唤醒管道(跨线程发送必需)
    if (!mg_wakeup_init(impl->mgr.get())) {
        impl->set_error("mg_wakeup_init failed");
        mg_mgr_free(impl->mgr.get());
        impl->mgr.reset();
        return -1;
    }

    std::string listen_url = std::string("http://") + bind_addr + ":"
                           + std::to_string(port);
    auto* conn = mg_http_listen(impl->mgr.get(), listen_url.c_str(),
                                 ws_event_handler, impl);
    if (!conn) {
        impl->set_error("failed to listen on " + listen_url);
        mg_mgr_free(impl->mgr.get());
        impl->mgr.reset();
        return -1;
    }

    impl->actual_ws_port = port;
    impl->running = true;
    impl->mgr_raw = impl->mgr.get();  // v0.53.44: worker 线程 mg_wakeup 用
    // v0.53.44: chat worker 池启动(2 worker——嵌入式单实例的轻量池)
    impl->chat_running = true;
    for (int i = 0; i < 2; ++i) {
        impl->chat_workers.emplace_back(&thin_agent_impl::chat_worker_loop, impl);
    }
    impl->event_thread = std::thread(event_loop_run, impl);

    impl->log(1, ("thin_agent WS server started on " + listen_url).c_str());
    return 0;
}

THIN_API int thin_agent_stop(thin_agent_t* agent) {
    if (!agent) return -1;
    auto* impl = reinterpret_cast<thin_agent_impl*>(agent);
    if (!impl->running) return 0;

    // v0.53.97 停机三段律（对齐 R81 的 ws_agent_main 修复；本文件此前是孪生漏网）：
    //   ①**停止消费**：worker 立即退出（不再排空队列；在途任务跑完即退 = 有界）
    //   ②**通告**：未开始的排队请求回一帧带原因的 error（客户端不再只见断连）
    //   ③**送达**：**事件线程必须活到最后**——先停 worker 并 join（在途结果已入 send_queue），
    //     再入队通告，最后才让事件线程退出（其退出前 flush_sends 尾刷）。
    // 旧顺序（先 join 事件线程 → 再 drain worker）导致停机期间算出的结果全部丢弃，
    // 且 stop() 被排空队列拖成分钟~小时级（v0.53.44 注释里的"析构顺序安全"只看到了
    // 线程安全，没看到"投递通道已死"）。
    impl->chat_running = false;
    impl->chat_cv.notify_all();
    for (auto& w : impl->chat_workers) {
        if (w.joinable()) w.join();
    }
    impl->chat_workers.clear();

    std::deque<ChatJob> leftover;
    {
        std::lock_guard<std::mutex> lk(impl->chat_mu);
        leftover.swap(impl->chat_queue);  // 锁内搬出、锁外入队（持 chat_mu 调 enqueue_send = 自死锁）
    }
    for (const auto& job : leftover) {
        nlohmann::json notice;
        notice["type"] = "error";
        notice["code"] = 29001;
        notice["message"] = "server shutting down: request not processed";
        impl->enqueue_send(job.c, notice.dump());
    }
    if (!leftover.empty()) {
        impl->log(1, ("shutdown: dropped " + std::to_string(leftover.size())
                      + " queued request(s), clients notified").c_str());
    }

    impl->running = false;  // 事件线程：退出循环 → 尾刷 send_queue → 返回
    if (impl->event_thread.joinable()) impl->event_thread.join();
    impl->mgr_raw = nullptr;
    if (impl->mgr) { mg_mgr_free(impl->mgr.get()); impl->mgr.reset(); }

    impl->log(1, "thin_agent stopped");
    return 0;
}

THIN_API void thin_agent_destroy(thin_agent_t* agent) {
    if (!agent) return;
    auto* impl = reinterpret_cast<thin_agent_impl*>(agent);
    thin_agent_stop(agent);
    impl->service.reset();
    impl->executor.reset();
    impl->task_engine.reset();
    delete impl;
}

THIN_API const char* thin_agent_version(void) {
    static std::string ver(thin_agent::kThinAgentVersion);
    return ver.c_str();
}

THIN_API int thin_agent_is_running(thin_agent_t* agent) {
    if (!agent) return 0;
    return reinterpret_cast<thin_agent_impl*>(agent)->running ? 1 : 0;
}

THIN_API char* thin_agent_get_info_json(thin_agent_t* agent) {
    nlohmann::json info;
    info["service"]  = "thin_agent";
    info["version"]  = thin_agent::kThinAgentVersion;
    info["hostname"] = get_hostname();
    info["os"]       = os_name();
    info["arch"]     = cpu_arch();

    if (agent) {
        auto* impl = reinterpret_cast<thin_agent_impl*>(agent);
        info["ws_port"] = impl->actual_ws_port > 0 ? impl->actual_ws_port :
            (impl->cfg.ws_port > 0 ? impl->cfg.ws_port : 8765);
        info["running"] = impl->running.load();
    } else {
        info["ws_port"] = 0;
        info["running"] = false;
    }
    info["profiles"]     = nlohmann::json::array();
    info["capabilities"] = {"agent"};

    std::string s = info.dump();
    char* result = static_cast<char*>(std::malloc(s.size() + 1));
    std::memcpy(result, s.c_str(), s.size() + 1);
    return result;
}

THIN_API const char* thin_agent_last_error(thin_agent_t* agent) {
    if (!agent) return "null agent handle";
    // v0.53.97: 返回**线程局部副本**。此前返回内部 std::string 的 c_str()，而锁在 return
    // 时已释放 → 其他线程 set_error 会重分配/改写该缓冲，调用方拿到悬垂或撕裂内容
    // （头文件却声明"静态字符串，不需要释放"）。线程局部拷贝满足该契约，且同线程下次
    // 调用前一直有效。
    thread_local std::string tl_err;
    auto* impl = reinterpret_cast<thin_agent_impl*>(agent);
    {
        std::lock_guard<std::mutex> lk(impl->error_mutex);
        tl_err = impl->last_error;
    }
    return tl_err.c_str();
}

THIN_API void thin_agent_set_log_callback(thin_agent_t* agent,
                                           thin_agent_log_fn callback,
                                           void* user_data) {
    if (!agent) return;
    auto* impl = reinterpret_cast<thin_agent_impl*>(agent);
    {
        std::lock_guard<std::mutex> lk(impl->log_mu);
        impl->log_cb = callback;
        impl->log_user_data = user_data;
    }
}

THIN_API void thin_agent_free_string(char* str) {
    std::free(str);
}

}  // extern "C"
