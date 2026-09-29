// thin_agent UDP 组播发现实现
//
// 服务端：监听组播端口，收到 probe 后回复 announce JSON
// 客户端：发送 probe → 收集回复 → 回调通知
//
// 组播地址：224.0.0.199:9876（可配置）
// 消息格式：JSON 单行 \n 结尾

#include "thin_agent/api/discovery.h"
#include <algorithm>
#include <chrono>

#ifdef _WIN32
// v0.53.65: Windows 真机适配——winsock2(close→closesocket+WSAStartup)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
// v0.54.16: 这三个 POSIX 网络头此前**无条件 include**（v0.53.65 补 winsock 时漏掉，修一漏一
// 同族）⇒ Windows 侧直接 fatal error: arpa/inet.h: No such file or directory。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#define CLOSE_SOCKET(s) closesocket(s)
#else
#define CLOSE_SOCKET(s) close(s)
#endif
// v0.54.16: setsockopt 的**值参数类型两平台不同**——POSIX 取 `const void*`，Windows/winsock 取
// `const char*` ⇒ 裸传 `int*`/`timeval*` 在 Windows 侧直接编不过（交叉编译实测
// error: cannot convert 'int*' to 'const char*'）。转换在**两平台都合法**（char* → void* 隐式），
// 故无需 #if，但统一经此 helper 以杜绝"修一漏一"（本轮实测 6 处同族漏网）。
#define SOCKOPT_VAL(p) reinterpret_cast<const char*>(p)

namespace {

constexpr int kDefaultMulticastPort = 9876;
constexpr const char* kDefaultMulticastGroup = "224.0.0.199";
constexpr int kDefaultTTL = 3;
constexpr int kMaxMsgSize = 2048;

// ── 服务端实现 ──

struct thin_discovery_server_impl {
    thin_discovery_config_t cfg;
    std::atomic<bool> running{false};
    std::thread worker;
    int sock = -1;

    ~thin_discovery_server_impl() {
        running = false;
        if (worker.joinable()) worker.join();
        if (sock >= 0) CLOSE_SOCKET(sock);
    }
};

void server_loop(thin_discovery_server_impl* srv) {
    char buf[kMaxMsgSize];
    while (srv->running) {
        struct sockaddr_in from;
        // v0.53.44: from_len 必须每轮重置——recvfrom 会改写该值,
        /// 递减后后续调用 from 截断/报 EINVAL(POSIX 语义)
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(srv->sock, buf, sizeof(buf) - 1, 0,
                            reinterpret_cast<struct sockaddr*>(&from), &from_len);
        if (n <= 0) {
          // v0.54.16: `usleep` 是 POSIX 名（MSVC 无，MinGW 亦需 _POSIX 宏）⇒ 换标准库
          // 可移植写法（两平台同形，无需 #if）。
          if (srv->running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          continue;
        }
        buf[n] = '\0';

        // 解析 probe
        try {
            auto probe = nlohmann::json::parse(buf);
            if (probe.value("type", "") != "thin_agent.discover") continue;
        } catch (...) {
            continue;  // 非 JSON 或格式不对，忽略
        }

        // 构造 announce
        nlohmann::json announce;
        announce["type"] = "thin_agent.announce";
        announce["version"] = "1";

        if (srv->cfg.announce_json) {
            // 使用自定义 announce JSON
            try {
                auto custom = nlohmann::json::parse(srv->cfg.announce_json);
                for (auto& [k, v] : custom.items()) announce[k] = v;
            } catch (...) {}
        }

        std::string resp = announce.dump() + "\n";
        sendto(srv->sock, resp.c_str(), resp.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&from), from_len);
    }
}

// ── 客户端实现 ──

struct thin_discovery_impl {
    thin_discovery_config_t cfg;
};

}  // namespace

// ══════════════════════════════════════════════════════
// 服务端 API
// ══════════════════════════════════════════════════════

extern "C" {


#ifdef _WIN32
// v0.53.65: winsock 惰性初始化——socket() 在无 WSAStartup 时恒失败
static bool ensure_winsock() {
  static bool done = []() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  }();
  return done;
}
#else
static bool ensure_winsock() { return true; }
#endif

THIN_API thin_discovery_server_t* thin_discovery_server_create(
    const thin_discovery_config_t* config) {
  ensure_winsock();  // v0.53.65
    if (!config) return nullptr;

    auto* srv = new thin_discovery_server_impl();
    srv->cfg = *config;
    if (srv->cfg.port <= 0) srv->cfg.port = kDefaultMulticastPort;
    if (!srv->cfg.group || !srv->cfg.group[0])
        srv->cfg.group = kDefaultMulticastGroup;
    if (srv->cfg.ttl <= 0) srv->cfg.ttl = kDefaultTTL;

    // 创建 UDP socket
    srv->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (srv->sock < 0) {
        delete srv;
        return nullptr;
    }

    // SO_REUSEADDR
    int reuse = 1;
    // v0.54.16: **两平台签名不同**——POSIX `setsockopt` 取 `const void*`，Windows/winsock 取
    // `const char*` ⇒ 裸传 `int*` 在 Windows 侧编不过（交叉编译实测 error: cannot convert ‘int*’
    // to ‘const char*’）。`reinterpret_cast<const char*>` 两平台都合法（char* → void* 隐式转换）。
    setsockopt(srv->sock, SOL_SOCKET, SO_REUSEADDR, SOCKOPT_VAL(&reuse), sizeof(reuse));

    // v0.53.44: 接收超时——析构置 running=false 后 join 需要本线程退出,
    /// 无超时+组播安静环境=join 永久挂(WSL 实测复现路径)
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 250 * 1000;  // 250ms
    setsockopt(srv->sock, SOL_SOCKET, SO_RCVTIMEO, SOCKOPT_VAL(&tv), sizeof(tv));

    // 绑定
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(srv->cfg.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(srv->sock, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        CLOSE_SOCKET(srv->sock);
        delete srv;
        return nullptr;
    }

    // 加入组播组
    struct ip_mreq mreq = {};
    mreq.imr_multiaddr.s_addr = inet_addr(srv->cfg.group);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(srv->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   SOCKOPT_VAL(&mreq), sizeof(mreq)) < 0) {
        CLOSE_SOCKET(srv->sock);
        delete srv;
        return nullptr;
    }

    // TTL
    int ttl = srv->cfg.ttl;
    setsockopt(srv->sock, IPPROTO_IP, IP_MULTICAST_TTL, SOCKOPT_VAL(&ttl), sizeof(ttl));

    return reinterpret_cast<thin_discovery_server_t*>(srv);
}

THIN_API int thin_discovery_server_start(thin_discovery_server_t* ds) {
    if (!ds) return -1;
    auto* srv = reinterpret_cast<thin_discovery_server_impl*>(ds);
    if (srv->running) return 0;

    srv->running = true;
    srv->worker = std::thread(server_loop, srv);
    return 0;
}

THIN_API void thin_discovery_server_stop(thin_discovery_server_t* ds) {
    if (!ds) return;
    auto* srv = reinterpret_cast<thin_discovery_server_impl*>(ds);
    srv->running = false;
    if (srv->worker.joinable()) srv->worker.join();
}

THIN_API void thin_discovery_server_destroy(thin_discovery_server_t* ds) {
    if (!ds) return;
    auto* srv = reinterpret_cast<thin_discovery_server_impl*>(ds);
    thin_discovery_server_stop(ds);
    delete srv;
}

// ══════════════════════════════════════════════════════
// 客户端 API
// ══════════════════════════════════════════════════════

THIN_API thin_discovery_t* thin_discovery_client_create(
    const thin_discovery_config_t* config) {
  ensure_winsock();  // v0.53.65
    // v0.53.98: NULL 检查——此前直接 `*config` 解引用即崩，而其兄弟
    // thin_discovery_server_create 有 `if (!config) return nullptr;`（兄弟不一致）。
    if (!config) return nullptr;
    auto* dc = new thin_discovery_impl();
    dc->cfg = *config;
    if (dc->cfg.port <= 0) dc->cfg.port = kDefaultMulticastPort;
    if (!dc->cfg.group || !dc->cfg.group[0])
        dc->cfg.group = kDefaultMulticastGroup;
    if (dc->cfg.ttl <= 0) dc->cfg.ttl = kDefaultTTL;
    return reinterpret_cast<thin_discovery_t*>(dc);
}

THIN_API int thin_discovery_probe(thin_discovery_t* dc,
                                   int timeout_ms,
                                   thin_discovery_on_found_fn on_found,
                                   void* user_data) {
    if (!dc || !on_found) return -1;
    auto* impl = reinterpret_cast<thin_discovery_impl*>(dc);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    // 允许端口复用（同一主机多客户端同时 probe）
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, SOCKOPT_VAL(&reuse), sizeof(reuse));

    // 绑定本地端口接收回复
    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(0);  // 系统分配
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, reinterpret_cast<struct sockaddr*>(&local), sizeof(local));

    // v0.53.98: **单点钳制** timeout_ms。此前 tv 用原始值：timeout_ms<=0 → {0,0}
    // 对 SO_RCVTIMEO 意为"**无超时**"（阻塞到收到包为止）→ 无 announce 时 probe
    // **永久挂死**；而后面的 deadline 却按 max(timeout,100) 算 = 自相矛盾。
    // 现在两处共用同一个钳制后的值。
    if (timeout_ms < 100) timeout_ms = 100;
    // 设置接收超时
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, SOCKOPT_VAL(&tv), sizeof(tv));

    // TTL
    int ttl = impl->cfg.ttl;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, SOCKOPT_VAL(&ttl), sizeof(ttl));

    // 发送 probe
    nlohmann::json probe;
    probe["type"] = "thin_agent.discover";
    probe["version"] = "1";
    std::string probe_str = probe.dump() + "\n";

    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(impl->cfg.port);
    dst.sin_addr.s_addr = inet_addr(impl->cfg.group);
    sendto(sock, probe_str.c_str(), probe_str.size(), 0,
           reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst));

    // 收集回复
    int found = 0;
    char buf[kMaxMsgSize];
    // v0.53.44: wall-clock 上限——恶意持续 announce 流可让 while 永不
    /// 超时(每条续命);总窗=timeout×3 封顶
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms * 3);
    // v0.53.98: **去重**——头文件契约是"每发现一个服务器调用一次"，但同服务器
    // 可能对一次 probe 回多条 announce（或网络重复），旧实现把每条都回调一次、
    // 且 found 重复计数（CLI/App 列表出现重复项）。按 server_id 去重。
    std::vector<std::string> seen;
    while (std::chrono::steady_clock::now() < deadline) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                            reinterpret_cast<struct sockaddr*>(&from), &from_len);
        // v0.53.98: **瞬时错误不得中断收集**。UDP 上收到 ICMP port-unreachable 会让
        // 下一次 recvfrom 返回 ECONNREFUSED 等错误——旧代码 `n <= 0` 直接 break，
        // 一条无关错误就把整轮发现提前掐断（漏掉还在路上的 announce）。
        // 现在：错误/空包一律 continue，由 deadline 兜底（socket 有 SO_RCVTIMEO，
        // continue 不会忙等）。真正"无人应答"仍由 deadline 结束。
        if (n < 0) continue;
        if (n == 0) continue;

        buf[n] = '\0';
        try {
            auto resp = nlohmann::json::parse(buf);
            if (resp.value("type", "") != "thin_agent.announce") continue;

            thin_discovery_entry_t entry = {};
            std::string sid  = resp.value("server_id", "");
            std::string host = resp.value("hostname", "");
            std::string os   = resp.value("os", "");
            std::string arch = resp.value("arch", "");
            std::string ver  = resp.value("agent_version", "");
            std::string ip   = inet_ntoa(from.sin_addr);
            // 如果 announce 里包含 server_id，追加 server 本地 IP
            if (resp.contains("server_id") && resp["server_id"].is_string()) {
                sid = resp["server_id"].get<std::string>();
            }

            int port = resp.value("ws_port", kDefaultMulticastPort);

            // v0.53.98: 去重键 = server_id（缺省则退化为 ip:port）
            std::string dedup_key = sid.empty()
                                        ? (ip + ":" + std::to_string(port))
                                        : sid;
            if (std::find(seen.begin(), seen.end(), dedup_key) != seen.end()) {
                continue;  // 同一服务器已回调过
            }
            seen.push_back(dedup_key);

            entry.server_id    = strdup(sid.c_str());
            entry.hostname     = strdup(host.c_str());
            entry.os           = strdup(os.c_str());
            entry.arch         = strdup(arch.c_str());
            entry.ip           = strdup(ip.c_str());
            entry.ws_port      = port;
            entry.agent_version = strdup(ver.c_str());

            if (resp.contains("profiles")) {
                entry.profiles_json = strdup(resp["profiles"].dump().c_str());
            } else {
                entry.profiles_json = strdup("[]");
            }
            if (resp.contains("capabilities")) {
                entry.capabilities_json = strdup(resp["capabilities"].dump().c_str());
            } else {
                entry.capabilities_json = strdup("[]");
            }

            entry.load = resp.value("load", 0.0f);
            entry.uptime_seconds = resp.value("uptime_seconds", 0);

            on_found(&entry, user_data);
            thin_discovery_free_entry(&entry);
            found++;
        } catch (...) {
            continue;
        }
    }

    CLOSE_SOCKET(sock);
    return found;
}

THIN_API void thin_discovery_client_destroy(thin_discovery_t* dc) {
    delete reinterpret_cast<thin_discovery_impl*>(dc);
}

THIN_API void thin_discovery_free_entry(thin_discovery_entry_t* entry) {
    if (!entry) return;
    free(entry->server_id);
    free(entry->hostname);
    free(entry->os);
    free(entry->arch);
    free(entry->ip);
    free(entry->agent_version);
    free(entry->profiles_json);
    free(entry->capabilities_json);
}

}  // extern "C"
