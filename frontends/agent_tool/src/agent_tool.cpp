// thin_agent desktop client
// webview 桌面壳：组播发现后端 → WebView 加载 ws_agent.html
//
// 构建:
//   cmake -B build && cmake --build build
//   产物: thin_agent_desktop
//
// 平台:
//   Linux:   libwebkit2gtk-4.1-dev + UDP 组播发现
//   macOS:   WebKit framework (内置) + UDP 组播发现
//   Windows: Edge WebView2 (Win10+ 内置)；组播发现暂未适配（Phase 4）

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "webview/webview.h"
#include "DebugLogger.h"

// ── 组播发现（Unix 平台）──────────────────────────────────────────
#ifdef _WIN32
  // Windows: 组播发现暂未适配，提供空桩
  #define THIN_NO_DISCOVERY 1
#else
  #include <mutex>
  #include <thread>
  #include <vector>
  #include "thin_agent/api/discovery.h"
#endif

// ── 编译期嵌入的 ws_agent.html（cmake xxd -i 生成）─────────────
// 如果 CMake 未生成此头文件，回退到外部文件。
#ifdef THIN_DESKTOP_EMBEDDED_HTML
  #include "ws_agent_html.h"
  #define HAS_EMBEDDED_HTML 1
#else
  #define HAS_EMBEDDED_HTML 0
#endif

// ══════════════════════════════════════════════════════════════════
// 组播发现逻辑（仅 Unix）
// ══════════════════════════════════════════════════════════════════
#ifndef THIN_NO_DISCOVERY

// 组播发现的服务器列表（线程安全）
static std::mutex               g_servers_mutex;
static std::vector<std::string> g_servers_json;  // 每项一条 JSON 行

// 回调：每发现一台服务器追加到列表
static void on_found(const thin_discovery_entry_t* entry, void* /*user_data*/) {
    std::string json = "{";
    if (entry->hostname && entry->hostname[0])
        json += "\"name\":\"" + std::string(entry->hostname) + "\",";
    else if (entry->server_id && entry->server_id[0])
        json += "\"name\":\"" + std::string(entry->server_id) + "\",";
    else
        json += "\"name\":\"unknown\",";
    if (entry->ip)      json += "\"ip\":\""     + std::string(entry->ip)     + "\",";
    json += "\"port\":"  + std::to_string(entry->ws_port) + ",";
    if (entry->os)      json += "\"os\":\""     + std::string(entry->os)     + "\",";
    if (entry->agent_version)
        json += "\"version\":\"" + std::string(entry->agent_version) + "\",";
    json += "\"load\":"  + std::to_string(entry->load);
    json += "}";

    std::lock_guard<std::mutex> lock(g_servers_mutex);
    g_servers_json.push_back(json);
}

// 执行组播探测
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

// JS bind：返回所有发现的后端
static void bind_discover(const std::string& seq, const std::string& /*req*/, void* arg) {
    auto* w = static_cast<webview::webview*>(arg);

    // 每次调用都重新探测
    {
        std::lock_guard<std::mutex> lock(g_servers_mutex);
        g_servers_json.clear();
    }
    do_discovery(2000);

    // 拼装 JSON 数组
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

#else  // THIN_NO_DISCOVERY — Windows stub

// 空桩：返回空数组，用户可在 ws_agent.html 手动输入后端地址
static void bind_discover_stub(const std::string& seq, const std::string& /*req*/, void* arg) {
    auto* w = static_cast<webview::webview*>(arg);
    w->resolve(seq.c_str(), 0, "[]");
}

#endif  // THIN_NO_DISCOVERY

// ══════════════════════════════════════════════════════════════════
// HTML 读取
// ══════════════════════════════════════════════════════════════════

static std::string get_html() {
#if HAS_EMBEDDED_HTML
    return std::string(reinterpret_cast<const char*>(ws_agent_html),
                       ws_agent_html_len);
#else
    // fallback: 读取外部文件
    const char* paths[] = {
        "web/ws_agent.html",
        "../web/ws_agent.html",
        "../../thin_agent/ws_agent.html",
        "/root/code/thin_agent/ws_agent.html",
    };
    for (const char* p : paths) {
        FILE* f = fopen(p, "r");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        std::string s(sz, '\0');
        fread(&s[0], 1, sz, f);
        fclose(f);
        return s;
    }
    return "<h2>ws_agent.html not found</h2>";
#endif
}

// ── 注入 JS：页面加载后自动发现后端 ────────────────────────────
static const char* kInitJS = R"(
// 桌面壳注入：WebView 检测标记 + 调试日志桥接
if (typeof window !== 'undefined') {
    window.__thin_agent_webview__ = true;
    window.__thin_agent_log__ = function(msg) {
        // 调用 C++ DebugLogger::write()
        if (typeof window.thin_log === 'function') {
            window.thin_log(msg);
        }
    };

    // 连接/断开等生命周期事件会通过 ws_agent.html 的 appendDebug() 自动记录

    // 桥接确认：发送日志到 C++ DebugLogger 验证注入脚本已执行成功
    if (typeof window.thin_log === 'function') {
        window.thin_log('[AgentTool] kInitJS injected: __thin_agent_webview__=true, bridge ready');
    }

    window.addEventListener('DOMContentLoaded', function() {
        // 隐藏调试按钮（WebView 模式不需要浏览器调试面板）
        var btns = document.querySelectorAll('.debug-btn-browser');
        for (var i = 0; i < btns.length; i++) {
            btns[i].style.display = 'none';
        }
        // 隐藏调试面板区域
        var panel = document.getElementById('debugPanel');
        if (panel) panel.style.display = 'none';

        if (typeof window.discoverBackends === 'function') {
            window.discoverBackends().then(function(list) {
                console.log('[thin_desktop] discovered', list.length, 'backends');
            });
        }
    });
})";

// ══════════════════════════════════════════════════════════════════
int main() {
    try {
#ifndef THIN_NO_DISCOVERY
        // 启动时立即做一次探测（预热）
        do_discovery(2000);
#endif

        // 创建 webview 窗口
        webview::webview w(false, nullptr);  // debug=false
        w.set_title("AgentTool");
#ifdef _WIN32
        // ── 方案C: 离屏 + set_size + SW_HIDE + SetWindowPlacement ────
        // 问题: webview 的 set_size 强制 window_show(); w.init() 若
        //       m_is_window_shown=false 会排队默认 set_size(640,480)
        // 解法: 移到当前显示器上方 → set_size(离屏) → SW_HIDE →
        //       SetWindowPlacement(复原尺寸居中) → dispatch 最大化
        const int winW = 1680, winH = 1024;

        // 检测当前鼠标所在显示器 — 窗口将在该显示器上弹出和复原
        POINT cursor;
        GetCursorPos(&cursor);
        HMONITOR curMon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(MONITORINFO);
        GetMonitorInfo(curMon, &mi);

        int monW = mi.rcWork.right - mi.rcWork.left;
        int monH = mi.rcWork.bottom - mi.rcWork.top;
        int nx = mi.rcWork.left + (monW - winW) / 2;
        int ny = mi.rcWork.top  + (monH - winH) / 2;

        // 1) 移到显示器上方（离屏，不可见）
        {
            auto result = w.window();
            if (result.ok()) {
                HWND hwnd = static_cast<HWND>(result.value());
                if (hwnd) {
                    SetWindowPos(hwnd, nullptr,
                                mi.rcWork.left, mi.rcWork.top - 10000,
                                0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
        }
        // 2) set_size — 离屏显示，m_is_window_shown=true
        w.set_size(winW, winH, WEBVIEW_HINT_NONE);
        // 3) 隐藏 + 设定复原尺寸居中于当前显示器
        {
            auto result = w.window();
            if (result.ok()) {
                HWND hwnd = static_cast<HWND>(result.value());
                if (hwnd) {
                    ShowWindow(hwnd, SW_HIDE);
                    WINDOWPLACEMENT wp = {};
                    wp.length = sizeof(WINDOWPLACEMENT);
                    GetWindowPlacement(hwnd, &wp);
                    wp.rcNormalPosition = {nx, ny, nx + winW, ny + winH};
                    wp.showCmd = SW_HIDE;
                    SetWindowPlacement(hwnd, &wp);
                }
            }
        }
#else
        w.set_size(1680, 1024, WEBVIEW_HINT_NONE);
#endif

        // 注册 native 函数供 JS 调用
#ifdef THIN_NO_DISCOVERY
        w.bind("discoverBackends", bind_discover_stub, &w);
#else
        w.bind("discoverBackends",
            [](std::string seq, std::string req, void* arg) {
                bind_discover(seq, req, arg);
            }, &w);
#endif

        // 注册调试日志桥接（JS: window.thin_log(msg) → C++: DebugLogger::write()）
        w.bind("thin_log", [](std::string /*seq*/, std::string req, void* /*arg*/) {
            // req 可能是 JSON 字符串或裸字符串，直接写日志
            DebugLogger::instance().write(req);
        }, nullptr);

        // 初始化 DebugLogger（运行目录下的 logs/agent_tool.log）
        DebugLogger::instance().init(".");

        // 注入初始化 JS
        w.init(kInitJS);

        // 加载 HTML
        w.set_html(get_html().c_str());

#ifdef _WIN32
        // 离屏 setup 完成，dispatch 一步最大化弹出 + 设窗口图标
        w.dispatch([&w]() {
            auto result = w.window();
            if (result.ok()) {
                HWND hwnd = static_cast<HWND>(result.value());
                if (hwnd) {
                    // 从 exe 资源加载图标（IDI_AGENT_TOOL=1，定义在 agent_tool.rc.in）
                    HICON hIcon = (HICON)LoadImage(
                        GetModuleHandle(NULL),
                        MAKEINTRESOURCE(1),  // IDI_AGENT_TOOL
                        IMAGE_ICON,
                        GetSystemMetrics(SM_CXSMICON),
                        GetSystemMetrics(SM_CYSMICON),
                        0);
                    if (hIcon) {
                        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
                        SendMessage(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIcon);
                    }
                    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
                }
            }
        });
#endif

        // 事件循环（阻塞直到窗口关闭）
        w.run();

    } catch (const webview::exception& e) {
        fprintf(stderr, "thin_agent_desktop error: %s\n", e.what());
        return 1;
    }

    return 0;
}
