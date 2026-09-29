// thin_agent C ABI — agent 核心库公共接口
//
// 设计原则：
//   1. 纯 C 接口（extern "C"），跨编译器/跨语言兼容
//   2. 不透明句柄（thin_agent_t），内部仍是完整 C++ 实现
//   3. 所有字符串为 UTF-8 C string，所有权归调用方
//   4. 线程安全：各函数内部加锁，可从多线程调用
//
// 平台动态库：
//   Linux/macOS/Android: libthin_agent_core.so / .dylib
//   Windows:            thin_agent_core.dll
//   iOS:                ThinAgentCore.framework

#ifndef THIN_AGENT_API_AGENT_H
#define THIN_AGENT_API_AGENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── 导出宏 ──
#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef THIN_AGENT_BUILDING_DLL
    #define THIN_API __declspec(dllexport)
  #else
    #define THIN_API __declspec(dllimport)
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  #define THIN_API __attribute__((visibility("default")))
#else
  #define THIN_API
#endif

// ── 不透明类型 ──
typedef struct thin_agent_t thin_agent_t;

// ── 配置结构体 ──
// 所有字段为 C string（NULL 表示使用默认值），生命周期只需在 thin_agent_create() 调用期间有效。
typedef struct {
    const char* config_yaml;       // demo.model.yaml 路径（必填）
    const char* profile;           // profile 名，默认 "default"
    int         ws_port;           // WebSocket 端口，默认 8765，0=不启 WS
    const char* ws_bind_addr;      // 绑定地址，默认 "127.0.0.1"（"0.0.0.0" 允许远程连接）
    int         multicast_port;    // 组播端口，默认 9876，0=禁用
    const char* multicast_group;   // 组播地址，默认 "224.0.0.199"
    int         discovery_enabled; // 是否启用组播发现，默认 1
    const char* data_dir;          // 数据目录，默认 ~/.thin_agent
    int         log_level;         // 0=silent, 1=info, 2=debug，默认 1
} thin_agent_config_t;

// ── 日志回调 ──
// level: 0=error, 1=warn, 2=info, 3=debug
typedef void (*thin_agent_log_fn)(int level, const char* message, void* user_data);

// ══════════════════════════════════════════════════════
// 生命周期
// ══════════════════════════════════════════════════════

/// 创建 agent 实例（加载配置、初始化组件，不启动服务）。
/// @return 成功返回句柄，失败返回 NULL。可用 thin_agent_last_error() 获取错误信息。
THIN_API thin_agent_t* thin_agent_create(const thin_agent_config_t* config);

/// 启动 agent 服务（WebSocket 服务器 + 组播响应）。
/// @return 0=成功，非0=失败。
THIN_API int thin_agent_start(thin_agent_t* agent);

/// 停止 agent 服务。**不等待未开始的排队请求**（它们会收到 code 29001 停机通告）；
/// 在途请求跑完即返回（有界）；连接随 mgr 释放关闭。
/// @return 0=成功，非0=失败。
THIN_API int thin_agent_stop(thin_agent_t* agent);

/// 销毁 agent 实例，释放所有资源。
THIN_API void thin_agent_destroy(thin_agent_t* agent);

// ══════════════════════════════════════════════════════
// 查询
// ══════════════════════════════════════════════════════

/// 版本号（如 "v0.12.0"）。
/// @return 静态字符串，不需要释放。
THIN_API const char* thin_agent_version(void);

/// 是否正在运行。
THIN_API int thin_agent_is_running(thin_agent_t* agent);

/// 获取 agent 信息 JSON：
///   {"hostname":"dev-pc","os":"Linux 6.8","arch":"x86_64",
///    "ip":"192.168.1.100","ws_port":8765,"profiles":["default"],...}
/// @return 需要 thin_agent_free_string() 释放。
THIN_API char* thin_agent_get_info_json(thin_agent_t* agent);

/// 上一次错误的描述文本。
/// @return **线程局部副本**（同线程下次调用本函数前有效），不需要释放。
THIN_API const char* thin_agent_last_error(thin_agent_t* agent);

// ══════════════════════════════════════════════════════
// 回调设置
// ══════════════════════════════════════════════════════

/// 设置日志回调（线程安全：内部加锁取一致快照，且回调在锁外调用——
/// 回调中可安全再调用本 API）。传 NULL 取消。
THIN_API void thin_agent_set_log_callback(thin_agent_t* agent,
                                           thin_agent_log_fn callback,
                                           void* user_data);

// ══════════════════════════════════════════════════════
// 内存管理
// ══════════════════════════════════════════════════════

/// 释放 thin_agent_get_info_json 等函数返回的字符串。
THIN_API void thin_agent_free_string(char* str);

#ifdef __cplusplus
}
#endif

#endif // THIN_AGENT_API_AGENT_H
