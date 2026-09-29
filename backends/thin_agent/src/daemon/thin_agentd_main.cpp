// thin_agentd — Agent 守护进程
//
// 职责：
//   1. dlopen libthin_agent_core.so
//   2. 创建 + 启动 agent
//   3. 启动组播发现
//   4. 等待 SIGTERM/SIGINT
//   5. 清理退出
//
// 用法：thin_agentd [--config config/demo.model.yaml] [--profile default] [--port 8765]

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

// v0.54.17: 动态库加载的平台隔离（Windows 无 dlfcn.h；库名 .dll 而非 .so）。文件内隔离，不跨文件封装。
#ifdef _WIN32
#include <windows.h>
#define TA_DLOPEN(p)   reinterpret_cast<void*>(LoadLibraryA(p))
#define TA_DLSYM(h, s) reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), s))
#define TA_DLCLOSE(h)  FreeLibrary(static_cast<HMODULE>(h))
static std::string ta_dlerror() { return "Win32 error " + std::to_string(GetLastError()); }
#define TA_LIBNAME "thin_agent_core.dll"
#else
#include <dlfcn.h>
#define TA_DLOPEN(p)   ::dlopen(p, RTLD_NOW)
#define TA_DLSYM(h, s) ::dlsym(h, s)
#define TA_DLCLOSE(h)  ::dlclose(h)
static std::string ta_dlerror() { return ::dlerror(); }
#define TA_LIBNAME "libthin_agent_core.so"
#endif

#include "thin_agent/api/agent_api.h"
#include "thin_agent/api/discovery.h"

namespace {

// 动态加载的函数指针
thin_agent_t* (*g_create)(const thin_agent_config_t*) = nullptr;
int (*g_start)(thin_agent_t*) = nullptr;
int (*g_stop)(thin_agent_t*) = nullptr;
void (*g_destroy)(thin_agent_t*) = nullptr;
const char* (*g_version)(void) = nullptr;
char* (*g_get_info)(thin_agent_t*) = nullptr;
void (*g_free_str)(char*) = nullptr;

thin_discovery_server_t* (*g_ds_create)(const thin_discovery_config_t*) = nullptr;
int (*g_ds_start)(thin_discovery_server_t*) = nullptr;
void (*g_ds_stop)(thin_discovery_server_t*) = nullptr;
void (*g_ds_destroy)(thin_discovery_server_t*) = nullptr;

thin_agent_t* g_agent = nullptr;
thin_discovery_server_t* g_discovery = nullptr;
void* g_lib_handle = nullptr;

volatile sig_atomic_t g_shutdown = 0;  // v0.53.72: handler 只置 flag
void on_signal(int) {
    // v0.53.72: handler 内只做 async-signal-safe 操作——此前直接调
    /// g_ds_stop/g_stop(join 线程/取锁)非信号安全,与持锁线程相交=
    /// 自死锁面;与 ws_agent_main on_signal 只置 flag 同款,停机统一
    /// 交给 pause() 返回后的主流程清理段
    g_shutdown = 1;
}

bool load_symbols(void* lib) {
    g_create   = (decltype(g_create))  TA_DLSYM(lib, "thin_agent_create");
    g_start    = (decltype(g_start))   TA_DLSYM(lib, "thin_agent_start");
    g_stop     = (decltype(g_stop))    TA_DLSYM(lib, "thin_agent_stop");
    g_destroy  = (decltype(g_destroy)) TA_DLSYM(lib, "thin_agent_destroy");
    g_version  = (decltype(g_version)) TA_DLSYM(lib, "thin_agent_version");
    g_get_info = (decltype(g_get_info))TA_DLSYM(lib, "thin_agent_get_info_json");
    g_free_str = (decltype(g_free_str))TA_DLSYM(lib, "thin_agent_free_string");

    g_ds_create  = (decltype(g_ds_create)) TA_DLSYM(lib, "thin_discovery_server_create");
    g_ds_start   = (decltype(g_ds_start))  TA_DLSYM(lib, "thin_discovery_server_start");
    g_ds_stop    = (decltype(g_ds_stop))   TA_DLSYM(lib, "thin_discovery_server_stop");
    g_ds_destroy = (decltype(g_ds_destroy))TA_DLSYM(lib, "thin_discovery_server_destroy");

    if (!g_create || !g_start || !g_stop || !g_destroy || !g_version) {
        std::cerr << "[thin_agentd] missing required symbols\n";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* config_yaml = "config/demo.model.yaml";
    const char* profile = "default";
    int ws_port = 8765;
    const char* lib_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_yaml = argv[++i];
        else if (a == "--profile" && i + 1 < argc) profile = argv[++i];
        else if (a == "--port" && i + 1 < argc) ws_port = std::stoi(argv[++i]);
        else if (a == "--lib" && i + 1 < argc) lib_path = argv[++i];
    }

    // 查找核心库（v0.54.17: 平台化的库名——POSIX `lib*.so` / Windows `*.dll`）
#ifdef _WIN32
    const char* search_paths[] = {
        lib_path,
        "build/thin_agent_core.dll",
        "./thin_agent_core.dll",
        "../build/thin_agent_core.dll",
    };
#else
    const char* search_paths[] = {
        lib_path,
        "build/libthin_agent_core.so",
        "./libthin_agent_core.so",
        "../build/libthin_agent_core.so",
        "/usr/local/lib/libthin_agent_core.so",
    };
#endif

    void* lib = nullptr;
    for (auto* p : search_paths) {
        if (!p) continue;
        lib = TA_DLOPEN(p);
        if (lib) {
            std::cout << "[thin_agentd] loaded " << p << "\n";
            break;
        }
    }

    if (!lib) {
        std::cerr << "[thin_agentd] failed to load libthin_agent_core.so: "
                  << ta_dlerror() << "\n";
        return 1;
    }

    g_lib_handle = lib;
    if (!load_symbols(lib)) { TA_DLCLOSE(lib); return 2; }

    std::cout << "[thin_agentd] version=" << g_version() << "\n";

    // 创建 agent
    thin_agent_config_t cfg = {};
    cfg.config_yaml = config_yaml;
    cfg.profile = profile;
    cfg.ws_port = ws_port;
    cfg.ws_bind_addr = "0.0.0.0";
    cfg.multicast_port = 9876;
    cfg.log_level = 1;

    g_agent = g_create(&cfg);
    if (!g_agent) {
        std::cerr << "[thin_agentd] agent create failed\n";
        TA_DLCLOSE(lib);
        return 3;
    }

    if (g_start(g_agent) != 0) {
        std::cerr << "[thin_agentd] agent start failed\n";
        g_destroy(g_agent);
        TA_DLCLOSE(lib);
        return 4;
    }

    // 启动组播发现
    thin_discovery_config_t ds_cfg = {};
    ds_cfg.port = 9876;
    ds_cfg.ttl = 3;

    if (g_ds_create) {
        g_discovery = g_ds_create(&ds_cfg);
        if (g_discovery && g_ds_start) g_ds_start(g_discovery);
        std::cout << "[thin_agentd] multicast discovery started\n";
    }

    // 打印信息
    char* info = g_get_info(g_agent);
    std::cout << "[thin_agentd] " << info << "\n";
    g_free_str(info);

    // 等待信号
    // v0.54.17: Windows 侧 `getpid`→`GetCurrentProcessId`、`pause`→短睡轮询 `g_shutdown`
    // （Windows 无 pause()；Ctrl+C 经 CRT 转成 SIGINT，短睡可被中断，退出延迟 ≤200ms）。
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef _WIN32
    std::cout << "[thin_agentd] running (pid=" << GetCurrentProcessId()
              << "), press Ctrl+C to stop\n";
    while (!g_shutdown) {
        Sleep(200);
    }
#else
    std::cout << "[thin_agentd] running (pid=" << getpid()
              << "), press Ctrl+C to stop\n";

    pause();
#endif

    // 清理(v0.53.72: 统一主流程——signal handler 不再抢跑)
    if (g_agent && g_stop) g_stop(g_agent);
    if (g_discovery && g_ds_stop) g_ds_stop(g_discovery);
    if (g_discovery && g_ds_destroy) g_ds_destroy(g_discovery);
    g_destroy(g_agent);
    TA_DLCLOSE(lib);
    std::cout << "[thin_agentd] stopped\n";
    return 0;
}
