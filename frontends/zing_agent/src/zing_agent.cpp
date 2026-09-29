// zing_agent — 正式客户端桌面端（P1：聊天客户端壳）
// 与 agent_tool 同构的 webview 宿主：加载 web/chat.html（纯聊天 UI，
// 无调试面）。组播发现复用 thin_agent/api/discovery.h。
//
// P2/P3（服务代管/远程 ssh）在此宿主上扩展安装器与连接管理。

#include <cstdio>
#include <cstdlib>
#include <string>

#include "webview/webview.h"
#include "DebugLogger.h"
#include "ServiceManager.h"
#include "ConfigStore.h"
#include <ctime>
#include <atomic>
#include "RemoteBackend.h"
#include <nlohmann/json.hpp>

// P2: 服务代管（默认配置——后续 P3 做安装器后按平台自动发现路径）
static zing::ServiceManager* g_svc = nullptr;
static zing::ConfigStore* g_cfg = nullptr;
static zing::ConfigStore& cfg() {
    if (!g_cfg) {
        g_cfg = new zing::ConfigStore();
        const bool ok = g_cfg->load();
        fprintf(stderr, "[zing] cfg load=%d path=%s keys_json=%.200s\n",
                ok ? 1 : 0, g_cfg->path().c_str(), g_cfg->all_json().c_str());
    }
    return *g_cfg;
}
static void init_service_manager() {
    zing::ServiceConfig cfg;
    const char* home = std::getenv("HOME");
    const std::string h = home ? home : "/root";
    // 探测常见位置（WSL 开发布局）
    const std::vector<std::string> cands = {
        "/root/code/thin_agent/build/thin_agent",
        h + "/code/thin_agent/build/thin_agent",
    };
    for (const auto& c : cands) {
#ifdef _WIN32
        // MSVC 无 access/X_OK——远程场景 Windows 本地源用存在性探测
        // （thin_agent 在 WSL 里，Windows 侧探测本就是 WSL 探测的镜像）
        if (std::FILE* f = std::fopen(c.c_str(), "rb")) { std::fclose(f); cfg.binary = c; break; }
#else
        if (::access(c.c_str(), X_OK) == 0) { cfg.binary = c; break; }
#endif
    }
    if (const char* b = std::getenv("ZING_SERVICE_BIN")) cfg.binary = b;
    cfg.config_yaml = std::string(h) + "/code/thin_agent/config/demo.model.yaml";
    if (const char* c = std::getenv("ZING_SERVICE_CONFIG")) cfg.config_yaml = c;
    cfg.profile = "zai_main_demo";
    if (const char* p = std::getenv("ZING_SERVICE_PROFILE")) cfg.profile = p;
    cfg.port = 8765;
    if (const char* p = std::getenv("ZING_SERVICE_PORT")) cfg.port = std::atoi(p);
    cfg.env_file = std::string(h) + "/.thin_agent/zai.env";
    if (const char* e = std::getenv("ZING_SERVICE_ENV")) cfg.env_file = e;
#ifdef _WIN32
    // v4: Windows 临时目录（/tmp 无效；%TEMP% 保证可写）
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = "C:\\Windows\\Temp";
    cfg.log_path = std::string(tmp) + "\\zing_thin_agent.log";
    cfg.pid_file = std::string(tmp) + "\\zing_svc.pid";
#else
    cfg.log_path = "/tmp/zing_thin_agent.log";
    cfg.pid_file = "/tmp/zing_svc.pid";
#endif
    delete g_svc;
    g_svc = new zing::ServiceManager(std::move(cfg));
}

#ifdef _WIN32
  #define THIN_NO_DISCOVERY 1
  #include <chrono>
#else
  #include <chrono>
  #include <mutex>
  #include <thread>
  #include <vector>
  #include <unistd.h>
  #include "thin_agent/api/discovery.h"
#endif

#ifdef ZING_EMBEDDED_HTML
  #include "chat_html.h"
  #define HAS_EMBEDDED_HTML 1
#else
  #define HAS_EMBEDDED_HTML 0
#endif

#ifndef THIN_NO_DISCOVERY
static std::mutex               g_servers_mutex;
static std::vector<std::string> g_servers_json;

// v15: 网络来源字符串的 JSON 转义——hostname/agent_version 来自组播报文,
// 含 "/控制字符=JSON 注入(畸形帧→JS parse 炸→整个发现列表废)
static std::string json_escape(const char* s) {
    if (!s) return "";
    std::string out;
    for (const char* p = s; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\') out += '\\';
        out += *p;  // 控制字符：跳过（防御性——正常报文不含）
        if (c < 0x20) out.pop_back();
    }
    return out;
}
static void on_found(const thin_discovery_entry_t* entry, void* /*user_data*/) {
    std::string json = "{";
    if (entry->hostname && entry->hostname[0])
        json += "\"name\":\"" + json_escape(entry->hostname) + "\",";
    else
        json += "\"name\":\"unknown\",";
    if (entry->ip) json += "\"ip\":\"" + json_escape(entry->ip) + "\",";
    json += "\"port\":" + std::to_string(entry->ws_port) + ",";
    if (entry->agent_version)
        json += "\"version\":\"" + json_escape(entry->agent_version) + "\",";
    json += "\"load\":" + std::to_string(entry->load) + "}";
    std::lock_guard<std::mutex> lock(g_servers_mutex);
    g_servers_json.push_back(json);
}

static void do_discovery(int timeout_ms = 2000) {
    thin_discovery_config_t cfg{};
    cfg.port  = 9876;
    cfg.group = "224.0.0.199";
    cfg.ttl   = 3;
    thin_discovery_t* dc = thin_discovery_client_create(&cfg);
    if (!dc) return;
    thin_discovery_probe(dc, timeout_ms, on_found, nullptr);
    thin_discovery_client_destroy(dc);
}

// v17: 异步远端线程回调前的窗口存活检查——detach 线程跑在 webview 析构后
// 时 resolve() 是 UB。窗口正常关闭路径先置 false。
static std::atomic<bool> g_zing_alive{false};

static void bind_discover(const std::string& seq, const std::string& /*req*/, void* arg) {
    auto* w = static_cast<webview::webview*>(arg);
    {
        std::lock_guard<std::mutex> lock(g_servers_mutex);
        g_servers_json.clear();
    }
    do_discovery(2000);
    std::string result = "[";
    {
        std::lock_guard<std::mutex> lock(g_servers_mutex);
        for (size_t i = 0; i < g_servers_json.size(); ++i) {
            if (i > 0) result += ",";
            result += g_servers_json[i];
        }
    }
    result += "]";
    w->resolve(seq.c_str(), 0, result.c_str());
}
#else
static void bind_discover_stub(const std::string& seq, const std::string& /*req*/, void* arg) {
    auto* w = static_cast<webview::webview*>(arg);
    w->resolve(seq.c_str(), 0, "[]");
}
#endif

static std::string get_html() {
#if HAS_EMBEDDED_HTML
    return std::string(reinterpret_cast<const char*>(chat_html), chat_html_len);
#else
    const char* paths[] = {
        "web/chat.html",
        "../web/chat.html",
    };
    for (const char* p : paths) {
        FILE* f = fopen(p, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        std::string s(sz, '\0');
        size_t n = fread(&s[0], 1, sz, f);  // v17: 检查实际读取量
        s.resize(n);
        fclose(f);
        return s;
    }
    return "<h2>chat.html not found</h2>";
#endif
}

static const char* kInitJS = R"(
if (typeof window !== 'undefined') {
    window.__thin_agent_webview__ = true;
    window.addEventListener('DOMContentLoaded', function() {
        if (typeof window.discoverBackends === 'function') {
            window.discoverBackends().then(function(list) {
                console.log('[zing_agent] discovered', list.length, 'backends');
            });
        }
    });
})";

// P2 CLI: zing_agent --svcctl status|start|stop（服务代管自动化验证口）
static int svcctl_main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s --svcctl status|start|stop\n", argv[0]); return 2; }
    init_service_manager();
    const std::string cmd = argv[1];
    if (cmd == "status") {
        printf("%s\n", g_svc->status_json().c_str());
        return g_svc->probe() ? 0 : 1;
    }
    if (cmd == "start") {
        const bool ok = g_svc->start();
        printf("%s\n", g_svc->status_json().c_str());
        return ok ? 0 : 1;
    }
    if (cmd == "stop") {
        const bool ok = g_svc->stop();
        printf("%s\n", g_svc->status_json().c_str());
        return ok ? 0 : 1;
    }
    if (cmd == "watch") {
        // 守护模式：拉起+每 5s 探活自动复活（Ctrl-C 退出）
        if (!g_svc->start()) fprintf(stderr, "watch: 首次拉起未健康，守护继续\n");
        g_svc->start_watch();
        printf("watching... (Ctrl-C 退出)\n");
        while (true) std::this_thread::sleep_for(std::chrono::seconds(3600));
    }
    fprintf(stderr, "unknown svcctl: %s\n", cmd.c_str());
    return 2;
}

int main(int argc, char** argv) {
    // P2: --svcctl CLI 模式（不进 GUI）
    if (argc >= 2 && std::strcmp(argv[1], "--svcctl") == 0)
        return svcctl_main(argc - 1, argv + 1);
    try {
#ifndef THIN_NO_DISCOVERY
        do_discovery(2000);
#endif
        webview::webview w(false, nullptr);
        w.set_title("Zing Agent");
#ifndef _WIN32
        w.set_size(1280, 840, WEBVIEW_HINT_NONE);
#else
        w.set_size(1280, 840, WEBVIEW_HINT_NONE);
#endif
#ifdef THIN_NO_DISCOVERY
        w.bind("discoverBackends", bind_discover_stub, &w);
#else
        w.bind("discoverBackends",
            [](std::string seq, std::string req, void* arg) {
                bind_discover(seq, req, arg);
            }, &w);
#endif
        w.bind("thin_log", [](std::string /*seq*/, std::string req, void* /*arg*/) {
            DebugLogger::instance().write(req);
        }, nullptr);

        // v7: 用户数据持久化桥（JS 端 KV——替代 webview localStorage）
        w.bind("cfgGetAll", [](std::string seq, std::string /*req*/, void* arg) {
            auto* w = static_cast<webview::webview*>(arg);
            w->resolve(seq.c_str(), 0, cfg().all_json().c_str());
        }, &w);
        w.bind("cfgSet", [](std::string seq, std::string req, void* arg) {
            auto* w = static_cast<webview::webview*>(arg);
            try {
                // webview 库把调用参数包成数组：req='["{\"k\":..,\"v\":..}"]'
                // →解包首元素再解析对象（kvSet 的 JS 侧单参数 JSON 串）
                nlohmann::json j = nlohmann::json::parse(req);
                if (j.is_array() && !j.empty() && j[0].is_string())
                    j = nlohmann::json::parse(j[0].get<std::string>());
                if (j.is_object() && j.contains("k") && j.contains("v"))
                    cfg().set(j["k"].get<std::string>(), j["v"].get<std::string>());
                w->resolve(seq.c_str(), 0, "true");
            } catch (...) {
                w->resolve(seq.c_str(), 0, "false");
            }
        }, &w);
        w.bind("cfgDel", [](std::string seq, std::string req, void* arg) {
            auto* w = static_cast<webview::webview*>(arg);
            try {
                nlohmann::json j = nlohmann::json::parse(req);
                if (j.is_array() && !j.empty() && j[0].is_string())
                    j = nlohmann::json::parse(j[0].get<std::string>());
                std::string k = j.is_object() ? j.value("k", "") : "";
                if (!k.empty()) cfg().del(k);
                w->resolve(seq.c_str(), 0, "true");
            } catch (...) {
                w->resolve(seq.c_str(), 0, "false");
            }
        }, &w);

        // P2: 服务代管桥（JS → ServiceManager）
        w.bind("svcStatus", [](std::string seq, std::string req, void* arg) {
            auto* w = static_cast<webview::webview*>(arg);
            if (!g_svc) init_service_manager();
            w->resolve(seq.c_str(), 0, g_svc->status_json().c_str());
        }, &w);
        w.bind("svcStart", [](std::string seq, std::string req, void* arg) {
            auto* w = static_cast<webview::webview*>(arg);
            if (!g_svc) init_service_manager();
            g_svc->start();
            w->resolve(seq.c_str(), 0, g_svc->status_json().c_str());
        }, &w);
        w.bind("svcStop", [](std::string seq, std::string req, void* arg) {
            auto* w = static_cast<webview::webview*>(arg);
            if (!g_svc) init_service_manager();
            g_svc->stop();
            w->resolve(seq.c_str(), 0, g_svc->status_json().c_str());
        }, &w);

        // P3: 远程后端桥（req JSON: {name,host,ssh_port,user,key_path,remote_dir,agent_port}）
        // 注意：不能 static——MSVC C3495 禁止 lambda 简单捕获 static 存储期变量
        auto host_from_req = [](const std::string& req) -> zing::HostProfile {
            zing::HostProfile h;
            try {
                auto j = nlohmann::json::parse(req.empty() ? "{}" : req);
                h.name = j.value("name", "remote");
                h.host = j.value("host", "");
                h.ssh_port = j.value("ssh_port", 22);
                h.user = j.value("user", "root");
                h.key_path = j.value("key_path", "");
                h.remote_dir = j.value("remote_dir", "~/zing");
                h.agent_port = j.value("agent_port", 8765);
                h.auth_token = j.value("auth_token", "");  // v0.53.32
            } catch (...) {}
            return h;
        };
        auto rb_json = [](zing::RemoteBackend& rb) -> std::string {
            nlohmann::json j;
            j["reachable"] = rb.reachable();
            j["healthy"] = rb.agent_healthy();
            j["ws_url"] = rb.ws_url();
            return j.dump();
        };
        w.bind("remoteProbe", [host_from_req, rb_json](std::string seq, std::string req, void* arg) {
            auto* w = static_cast<webview::webview*>(arg);
            zing::RemoteBackend rb(host_from_req(req));
            w->resolve(seq.c_str(), 0, rb_json(rb).c_str());
        }, &w);
        // v17: 耗时远端操作异步化——bind 回调在 GTK 主线程 dispatch 执行,
        // install(含 scp 180s)同步跑会让 UI 冻结数分钟。工作线程执行,
        // 完成回 resolve(resolve 内部自带 dispatch 回主线程,线程安全)。
        w.bind("remoteInstall", [host_from_req](std::string seq, std::string req, void* arg) {
            auto* w = static_cast<webview::webview*>(arg);
            if (!g_svc) init_service_manager();  // v3: 安装源路径依赖（用户可先装远程不启本机）
            zing::RemoteBackend rb(host_from_req(req));
            std::string bin = g_svc ? g_svc->config().binary : "";
            std::string env_f = g_svc ? g_svc->config().env_file : "";
            std::string conf = g_svc ? g_svc->config().config_yaml : "";
            std::thread([w, seq, rb, bin, env_f, conf]() mutable {
                if (!g_zing_alive.load()) return;  // v17: 窗口已关,放弃回调
                std::string err;
                const int rc = rb.install(bin, env_f, conf, &err);
                nlohmann::json j;
                j["ok"] = (rc == 0);
                j["error"] = err;
                j["auth_token"] = rb.profile().auth_token;  // v0.53.32: 自动生成的 token 回传 UI
                j["ws_url"] = rb.ws_url();                  // 已带 token 的直连地址
                w->resolve(seq.c_str(), 0, j.dump().c_str());
            }).detach();
        }, &w);
        // v17: 同 remoteInstall——start(25s) 不冻结 UI
        w.bind("remoteStart", [host_from_req, rb_json](std::string seq, std::string req, void* arg) {
            auto* w = static_cast<webview::webview*>(arg);
            zing::RemoteBackend rb(host_from_req(req));
            std::thread([w, seq, rb, rb_json]() mutable {
                if (!g_zing_alive.load()) return;  // v17: 窗口已关,放弃回调
                std::string err;
                const int rc = rb.start(&err);
                nlohmann::json j = nlohmann::json::parse(rb_json(rb));
                j["ok"] = (rc == 0);
                j["error"] = err;
                w->resolve(seq.c_str(), 0, j.dump().c_str());
            }).detach();
        }, &w);
        // v17: 同 remoteInstall——stop(含 ssh 往返)不冻结 UI
        w.bind("remoteStop", [host_from_req, rb_json](std::string seq, std::string req, void* arg) {
            auto* w = static_cast<webview::webview*>(arg);
            zing::RemoteBackend rb(host_from_req(req));
            std::thread([w, seq, rb, rb_json]() mutable {
                if (!g_zing_alive.load()) return;  // v17: 窗口已关,放弃回调
                const int rc = rb.stop();
                nlohmann::json j = nlohmann::json::parse(rb_json(rb));
                j["ok"] = (rc == 0);
                w->resolve(seq.c_str(), 0, j.dump().c_str());
            }).detach();
        }, &w);
        DebugLogger::instance().init(".");
        // v7: 启动即初始化持久层（写探针键=确定性验证 C++ 落盘链路）
        cfg().set("zing_boot_ts", std::to_string(time(nullptr)));
        w.init(kInitJS);
        w.set_html(get_html().c_str());
        g_zing_alive.store(true);   // v17: 窗口进入事件循环——异步远端回调放行
        w.run();
        g_zing_alive.store(false);  // v17: 事件循环退出(窗口关闭)——拦截在途线程
    } catch (const webview::exception& e) {
        fprintf(stderr, "zing_agent error: %s\n", e.what());
        return 1;
    }
    return 0;
}
