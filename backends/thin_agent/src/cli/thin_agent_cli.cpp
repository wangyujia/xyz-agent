// thin_agent_cli — 前端 CLI 客户端
//
// 职责：
//   1. 组播扫描发现后端 → 用户选择
//   2. WebSocket 连接后端
//   3. REPL：输入→发送 chat 请求→流式接收→打印
//   4. 命令：/connect /discover /backends /help /quit
//
// 不依赖 libthin_agent_core.so ——前端只做 WS 客户端 + 组播发现。

// v0.54.16: **本文件此前零 Windows 适配**（4 个 POSIX 网络头无条件 include ⇒ Windows 侧
// 直接 fatal error: arpa/inet.h: No such file or directory）。按本仓既有约定（与
// src/api/discovery.cpp 同款、文件内隔离、不跨文件封装）补 winsock 分支。
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET(s) closesocket(s)
// setsockopt 值参数两平台签名不同（POSIX const void* / winsock const char*）
#define SOCKOPT_VAL(p) reinterpret_cast<const char*>(p)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET(s) close(s)
#define SOCKOPT_VAL(p) (p)
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include "mongoose.h"

namespace {

// ── 后端条目 ──
struct Backend {
    std::string hostname;
    std::string os;
    std::string ip;
    int         ws_port = 8765;
    std::string version;
};

std::vector<Backend> g_backends;       // 扫描到的后端列表
std::string          g_current_url;    // 当前 WS URL
std::atomic<bool>    g_connected{false};
std::atomic<bool>    g_stop{false};
mg_mgr               g_mgr;
mg_connection*       g_ws_conn = nullptr;
std::mutex           g_print_mutex;    // 保护终端输出
bool                 g_quiet = false;  // 静默模式（用于内部命令）
// v0.53.45: 本条回复是否已有流式 chunk——chat_result 防双打
std::atomic<bool>    g_got_stream_chunk{false};

// v0.53.94: 回复检测标志（修"收到答案后空转满 120s"）。此前 CLI **没有任何
// "本请求已回复"检测**：等待循环只认 g_connected 变假或 120s 上限 → 服务端答完
// 但连接不断时，用户要干等 120s（实测 WALL=122s），且退出码恒 0。
std::atomic<bool>    g_reply_seen{false};   // 本请求已有终局帧（结果/错误/done）
std::atomic<bool>    g_reply_error{false};  // 终局帧是 error（服务端明确失败）

/// 等待本请求回复的结论（可区分成功/超时/断线/服务端错误——脚本可用退出码判断）
enum class WaitOutcome { Replied, Timeout, Disconnected, ServerError };

/// 退出码契约（v0.53.94）：0 成功 / 2 超时 / 3 断线 / 4 服务端错误
int exit_code_for(WaitOutcome o) {
  switch (o) {
    case WaitOutcome::Replied:      return 0;
    case WaitOutcome::Timeout:      return 2;
    case WaitOutcome::Disconnected: return 3;
    case WaitOutcome::ServerError:  return 4;
  }
  return 1;
}

/// 等本请求终局：**收到回复立即返回**（不再空转到上限）；超时/断线有明确结论。
WaitOutcome wait_for_reply(int wait_sec) {
  auto start = std::chrono::steady_clock::now();
  while (g_connected) {
    mg_mgr_poll(&g_mgr, 50);
    if (g_reply_seen.load()) {
      return g_reply_error.load() ? WaitOutcome::ServerError : WaitOutcome::Replied;
    }
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - start).count();
    if (sec >= wait_sec) return WaitOutcome::Timeout;
  }
  return WaitOutcome::Disconnected;
}

/// 终局输出（json 模式给**结构化**错误；此前 json 模式下超时/断线 stdout 全空）
void report_outcome(WaitOutcome o, bool json_mode, long elapsed_ms) {
  if (o == WaitOutcome::Replied) return;
  const char* reason = o == WaitOutcome::Timeout      ? "timeout"
                       : o == WaitOutcome::Disconnected ? "disconnected"
                                                        : "server_error";
  if (json_mode) {
    nlohmann::json e{{"type", "cli_error"}, {"reason", reason}, {"elapsed_ms", elapsed_ms}};
    std::lock_guard<std::mutex> lk(g_print_mutex);
    std::cout << e.dump() << "\n" << std::flush;
  } else if (o == WaitOutcome::Timeout) {
    std::lock_guard<std::mutex> lk(g_print_mutex);
    std::cerr << "[timeout] no reply in " << (elapsed_ms / 1000) << "s\n";
  } else if (o == WaitOutcome::Disconnected) {
    std::lock_guard<std::mutex> lk(g_print_mutex);
    std::cerr << "[disconnected] connection lost before reply\n";
  }
  // ServerError：具体原因已由事件处理器的 [error] 行给出，此处不重复刷屏
}

// ── 组播发现（内嵌，不依赖外部库）──

std::vector<Backend> discover_backends(int timeout_ms = 1500) {
    std::vector<Backend> results;

#ifdef _WIN32
    // v0.54.16: winsock 惰性初始化——未调用 WSAStartup 时 socket() 恒失败（返回 INVALID_SOCKET）。
    // 与 src/api/discovery.cpp 的 ensure_winsock() 同款：static lambda-once，只初始化一次。
    static const bool wsa_ready = []() {
      WSADATA d{};
      return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)wsa_ready;
#endif

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return results;

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, SOCKOPT_VAL(&reuse), sizeof(reuse));

    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(0);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, (struct sockaddr*)&local, sizeof(local));

    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, SOCKOPT_VAL(&tv), sizeof(tv));

    int ttl = 3;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, SOCKOPT_VAL(&ttl), sizeof(ttl));

    // 发送 probe
    nlohmann::json probe;
    probe["type"] = "thin_agent.discover";
    probe["version"] = "1";
    std::string probe_str = probe.dump() + "\n";

    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(9876);
    dst.sin_addr.s_addr = inet_addr("224.0.0.199");
    sendto(sock, probe_str.c_str(), probe_str.size(), 0,
           (struct sockaddr*)&dst, sizeof(dst));

    // 收集回复
    char buf[2048];
    while (true) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const auto n = recvfrom(sock, buf, sizeof(buf) - 1, 0,  // auto: POSIX=ssize_t / winsock=int
                            (struct sockaddr*)&from, &from_len);
        if (n <= 0) break;
        buf[n] = '\0';

        try {
            auto resp = nlohmann::json::parse(buf);
            if (resp.value("type", "") != "thin_agent.announce") continue;

            Backend b;
            b.hostname = resp.value("hostname", "unknown");
            b.os       = resp.value("os", "unknown");
            b.ip       = inet_ntoa(from.sin_addr);
            b.ws_port  = resp.value("ws_port", 8765);
            b.version  = resp.value("agent_version", "");

            // 去重
            bool dup = false;
            for (auto& existing : results) {
                if (existing.ip == b.ip && existing.ws_port == b.ws_port) {
                    dup = true; break;
                }
            }
            if (!dup) results.push_back(b);
        } catch (...) { continue; }
    }

    CLOSE_SOCKET(sock);
    return results;
}

// ── Mongoose WS 事件处理 ──

void ws_event_handler(mg_connection* c, int ev, void* ev_data) {
    switch (ev) {
        case MG_EV_WS_OPEN: {
            g_connected = true;
            if (!g_quiet) {
                std::lock_guard<std::mutex> lk(g_print_mutex);
                std::cout << "\n[connected to " << g_current_url << "]\n\n";
            }
            break;
        }
        case MG_EV_WS_MSG: {
            auto* wm = static_cast<mg_ws_message*>(ev_data);
            std::string msg((const char*)wm->data.buf, wm->data.len);

            try {
                auto resp = nlohmann::json::parse(msg);
                std::string type = resp.value("type", "");

                if (type == "chat_chunk") {
                    g_got_stream_chunk.store(true);
                    if (resp.value("done", false)) g_reply_seen.store(true);  // v0.53.94 流式终局
                    std::lock_guard<std::mutex> lk(g_print_mutex);
                    std::cout << resp.value("chunk", "") << std::flush;
                    if (resp.value("done", false)) std::cout << "\n\n";
                } else if (type == "thinking") {
                    // v0.53.45: v0.53.38 起透传真思考流(content 增量+live)
                    ///——逐条 \r 覆盖会丢内容;live 流追加显示,done 清行
                    std::string content = resp.value("content", "");
                    bool live = resp.value("live", false);
                    std::lock_guard<std::mutex> lk(g_print_mutex);
                    if (live && !content.empty()) {
                        std::cout << content << std::flush;
                    } else if (!content.empty()) {
                        std::cout << content << "\n" << std::flush;
                    } else {
                        int tier = resp.value("tier", 0);
                        // v0.54.6: 改名 progress_msg——此前与外层 `msg` 同名（-Wshadow=local 判官命中）
                        std::string progress_msg = resp.value("msg", "");
                        const char* icons[] = {"", "🔍", "🎯", "🧠"};
                        std::string icon = (tier >= 1 && tier <= 3) ? icons[tier] : "⏳";
                        std::cout << "\r\033[K" << icon << " " << progress_msg << std::flush;
                    }
                } else if (type == "chat_result") {
                    g_reply_seen.store(true);   // v0.53.94: 非流式终局
                    // v0.53.45: 非流式路径的服务端回复只发 chat_result
                    ///(不发 chunk)——此前 CLI 完全不消费=静默丢答;
                    /// 流式模式下 chunk 已打,text 与流重复时跳过
                    std::string text = resp.value("text", "");
                    if (!text.empty() && !g_got_stream_chunk.exchange(false)) {
                        // 非流式(或流式中断兜底)——直接打全文;
                        /// 流式回复已由 chunk 打过,此处重置标志防双打
                        std::lock_guard<std::mutex> lk(g_print_mutex);
                        std::cout << "\r\033[K" << text << "\n\n";
                    } else {
                        g_got_stream_chunk.store(false);
                    }
                } else if (type == "hello") {
                    // 握手，不打印
                } else if (type == "error") {
                    g_reply_error.store(true);
                    g_reply_seen.store(true);   // v0.53.94: 服务端明确失败也算终局（不得再空等）
                    std::lock_guard<std::mutex> lk(g_print_mutex);
                    std::cerr << "[error] " << resp.value("message", "unknown")
                              << "\n";
                }
                // 其他非流式响应（chat_result 等）已经在 chat_chunk 流中
            } catch (...) {}
            break;
        }
        case MG_EV_CLOSE: {
            g_connected = false;
            if (!g_quiet) {
                std::lock_guard<std::mutex> lk(g_print_mutex);
                std::cout << "\n[disconnected]\n";
            }
            break;
        }
        case MG_EV_ERROR: {
            g_connected = false;
            if (!g_quiet) {
                std::lock_guard<std::mutex> lk(g_print_mutex);
                std::cerr << "[connection error]\n";
            }
            break;
        }
        default: break;
    }
}

// ── 连接后端 ──

bool connect_to(const std::string& url) {
    if (g_ws_conn) {
        g_ws_conn->is_closing = 1;
        g_ws_conn = nullptr;
    }
    g_current_url = url;
    g_ws_conn = mg_ws_connect(&g_mgr, url.c_str(), ws_event_handler, nullptr, nullptr);
    return g_ws_conn != nullptr;
}

// ── 命令处理 ──

void cmd_discover() {
    std::cout << "Scanning for thin_agent backends...\n";
    g_backends = discover_backends();

    if (g_backends.empty()) {
        std::cout << "No backends found on LAN.\n";
        return;
    }

    std::cout << "\nFound " << g_backends.size() << " backend(s):\n";
    for (size_t i = 0; i < g_backends.size(); ++i) {
        auto& b = g_backends[i];
        std::cout << "  [" << (i + 1) << "] " << b.hostname
                  << "  " << b.os << "  " << b.version
                  << "  ws://" << b.ip << ":" << b.ws_port << "/ws\n";
    }
    std::cout << "\nType /connect <n> to connect, or /connect <url>\n";
}

void cmd_connect(const std::string& arg) {
    std::string url;

    // 尝试解析为序号
    try {
        int idx = std::stoi(arg) - 1;
        if (idx >= 0 && idx < (int)g_backends.size()) {
            auto& b = g_backends[idx];
            url = "ws://" + b.ip + ":" + std::to_string(b.ws_port) + "/ws";
        }
    } catch (...) {}

    // 否则作为 URL
    if (url.empty()) {
        url = arg;
        // 自动补全 ws:// 前缀和 /ws 后缀
        if (url.find("://") == std::string::npos) {
            url = "ws://" + url;
        }
        if (url.find("/ws") == std::string::npos) {
            // 提取 host:port 部分
            auto pos = url.find("://");
            auto host = url.substr(pos + 3);
            if (host.find('/') == std::string::npos) {
                url += "/ws";
            }
        }
    }

    std::cout << "Connecting to " << url << "...\n";
    if (connect_to(url)) {
        // 等待握手
        for (int i = 0; i < 50 && !g_connected; ++i) {
            mg_mgr_poll(&g_mgr, 50);
        }
    } else {
        std::cerr << "Failed to connect.\n";
    }
}

void cmd_backends() {
    if (g_backends.empty()) {
        std::cout << "No backends cached. Run /discover first.\n";
        return;
    }
    for (size_t i = 0; i < g_backends.size(); ++i) {
        auto& b = g_backends[i];
        std::cout << "  [" << (i + 1) << "] " << b.hostname
                  << "  ws://" << b.ip << ":" << b.ws_port << "/ws\n";
    }
}

void cmd_help() {
    std::cout << R"(
Commands:
  /discover          Scan LAN for thin_agent backends
  /connect <n|url>   Connect to backend #n or ws://host:port/ws
  /backends          List cached backends
  /help              This help
  /quit, /exit       Exit

CLI flags:
  -q, --query TEXT   One-shot: send query, print result, exit
  -j, --json         Output raw JSON (for scripting)
  -t, --timeout-sec N  Wait at most N seconds for a reply (default 120)
  --connect URL      Auto-connect on startup
  Pipe/stdin         Read query from stdin (non-TTY mode)

Examples:
  thin_agent_cli -q "who are you"
  echo "fix this bug" | thin_agent_cli -j
  thin_agent_cli --connect ws://192.168.1.5:8765/ws

Exit codes (one-shot/pipe):
  0 success / 2 timeout / 3 disconnected / 4 server error
)";
}

}  // namespace

// ── Main ──

int main(int argc, char** argv) {
    std::string auto_connect_url;
    std::string one_shot_query;
    bool json_mode = false;
    int wait_sec = 120;   // v0.53.94: 等待回复上限（可配，便于 e2e/脚本）

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--connect" && i + 1 < argc) auto_connect_url = argv[++i];
        else if ((a == "-q" || a == "--query") && i + 1 < argc)
            one_shot_query = argv[++i];
        else if (a == "-j" || a == "--json") json_mode = true;
        else if ((a == "--timeout-sec" || a == "-t") && i + 1 < argc)
            wait_sec = std::max(1, std::atoi(argv[++i]));
    }
    if (const char* env_t = std::getenv("THIN_AGENT_CLI_TIMEOUT_SEC")) {
        const int v = std::atoi(env_t);
        if (v > 0) wait_sec = v;
    }

    // Pipe/stdin 模式：stdin 不是 TTY → 读取全文作为查询
    bool pipe_mode = !isatty(STDIN_FILENO);
    if (pipe_mode && one_shot_query.empty()) {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!one_shot_query.empty()) one_shot_query += "\n";
            one_shot_query += line;
        }
    }

    // 非 one-shot/pipe 模式：显示 banner
    if (one_shot_query.empty()) {
        std::cout << "thin_agent_cli v0.13.0 — Type /help for commands\n";
    }

    mg_mgr_init(&g_mgr);

    // 连接后端
    if (!auto_connect_url.empty()) {
        cmd_connect(auto_connect_url);
    } else if (!one_shot_query.empty() && g_backends.empty()) {
        // One-shot/pipe: 自动扫描或使用默认 localhost
        g_backends = discover_backends(800);
        if (g_backends.empty()) {
            // Fallback to localhost
            auto_connect_url = "ws://127.0.0.1:8765/ws";
            cmd_connect(auto_connect_url);
        } else {
            auto& b = g_backends[0];
            auto_connect_url = "ws://" + b.ip + ":" + std::to_string(b.ws_port) + "/ws";
            cmd_connect(auto_connect_url);
        }
    } else if (one_shot_query.empty()) {
        // 交互模式：自动扫描
        cmd_discover();
    }

    // ── One-shot / pipe 模式 ──
    if (!one_shot_query.empty()) {
        if (!g_connected) {
            std::cerr << "Failed to connect to agent.\n";
            mg_mgr_free(&g_mgr);
            return 1;
        }

        nlohmann::json req;
        req["type"] = "chat";
        req["text"] = one_shot_query;

        g_reply_seen.store(false);
        g_reply_error.store(false);
        g_got_stream_chunk.store(false);
        std::string s = req.dump();
        mg_ws_send(g_ws_conn, s.c_str(), s.size(), WEBSOCKET_OP_TEXT);

        // v0.53.94: 收到终局帧**立即返回**（此前无回复检测 → 收到答案后空转到 120s）
        const auto t0 = std::chrono::steady_clock::now();
        const WaitOutcome outcome = wait_for_reply(wait_sec);
        const long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0).count();
        report_outcome(outcome, json_mode, elapsed_ms);

        if (g_ws_conn) g_ws_conn->is_closing = 1;
        mg_mgr_free(&g_mgr);
        return exit_code_for(outcome);   // 0 成功 / 2 超时 / 3 断线 / 4 服务端错误
    }

    // ── 交互 REPL ──
    std::string line;
    std::cout << "> " << std::flush;

    while (!g_stop && std::getline(std::cin, line)) {
        if (line.empty()) {
            std::cout << "> " << std::flush;
            continue;
        }

        // 命令
        if (line[0] == '/') {
            if (line == "/quit" || line == "/exit") {
                g_stop = true;
                break;
            } else if (line.rfind("/connect ", 0) == 0) {
                cmd_connect(line.substr(9));
            } else if (line == "/discover") {
                cmd_discover();
            } else if (line == "/backends") {
                cmd_backends();
            } else if (line == "/help") {
                cmd_help();
            } else {
                std::cout << "Unknown command. /help for list.\n";
            }
            std::cout << "> " << std::flush;
            continue;
        }

        // Chat 消息
        if (!g_connected) {
            std::cout << "[not connected] Type /discover or /connect\n> " << std::flush;
            continue;
        }

        nlohmann::json req;
        req["type"] = "chat";
        req["text"] = line;

        g_reply_seen.store(false);
        g_reply_error.store(false);
        g_got_stream_chunk.store(false);
        std::string s = req.dump();
        mg_ws_send(g_ws_conn, s.c_str(), s.size(), WEBSOCKET_OP_TEXT);

        // v0.53.94: 收到终局帧立即回到提示符（不再空转到 120s）
        const auto t1 = std::chrono::steady_clock::now();
        const WaitOutcome repl_outcome = wait_for_reply(wait_sec);
        report_outcome(repl_outcome, json_mode,
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t1).count());
        if (repl_outcome != WaitOutcome::Replied) std::cout << "\n";

        std::cout << "> " << std::flush;
    }

    std::cout << "\nGoodbye.\n";

    if (g_ws_conn) g_ws_conn->is_closing = 1;
    mg_mgr_free(&g_mgr);
    return 0;
}
