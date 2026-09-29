// thin_agent C ABI — UDP 组播发现接口
//
// 协议：
//   Client 发送 probe 到组播地址 → 局域网内所有 agent 后端回复 announce
//   组播地址: 224.0.0.199:9876（可配置）
//   消息格式: JSON 单行，以 \n 结尾
//
// 安全：
//   组播仅局域网可达（路由器默认不转发）。
//   ⚠ **HMAC 签名防伪造尚未实现**（v0.53.98 据实修正：此前文档声称"可选 HMAC"，
//     但实现里没有任何签名/校验代码——任何主机都能伪造 announce，请勿依赖该防护）。

#ifndef THIN_AGENT_API_DISCOVERY_H
#define THIN_AGENT_API_DISCOVERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef THIN_API
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
#endif

// ── 不透明类型 ──
typedef struct thin_discovery_t       thin_discovery_t;
typedef struct thin_discovery_server_t thin_discovery_server_t;

// ── 发现的服务器信息 ──
typedef struct {
    char* server_id;
    char* hostname;
    char* os;
    char* arch;
    char* ip;
    int   ws_port;
    char* agent_version;
    char* profiles_json;       // JSON 数组 ["default","gpu-server"]
    char* capabilities_json;   // JSON 数组 ["agent","gateway"]
    float load;                // CPU 负载 0.0–1.0
    int   uptime_seconds;
} thin_discovery_entry_t;

// ── 服务端配置 ──
typedef struct {
    int         port;          // 组播端口，默认 9876
    const char* group;         // 组播地址，默认 "224.0.0.199"
    const char* announce_json; // 宣告内容 JSON，NULL 则自动生成
    int         ttl;           // TTL，默认 3
} thin_discovery_config_t;

// ── 客户端回调 ──
// 每发现一个服务器调用一次。
// entry 仅在回调期间有效，不要保存指针。
typedef void (*thin_discovery_on_found_fn)(const thin_discovery_entry_t* entry,
                                            void* user_data);

// ══════════════════════════════════════════════════════
// 服务端（在 thin_agentd 中使用）
// ══════════════════════════════════════════════════════

/// 创建组播响应器。
/// announce_json: 自定义宣告内容（NULL = 自动生成）。
/// ws_port、version: 供自动生成 announce 使用。
THIN_API thin_discovery_server_t* thin_discovery_server_create(
    const thin_discovery_config_t* config);

/// 启动组播响应（监听 probe，回复 announce）。
THIN_API int thin_discovery_server_start(thin_discovery_server_t* ds);

/// 停止响应。
THIN_API void thin_discovery_server_stop(thin_discovery_server_t* ds);

/// 销毁。
THIN_API void thin_discovery_server_destroy(thin_discovery_server_t* ds);

// ══════════════════════════════════════════════════════
// 客户端（在 thin_agent_cli / 移动 App 中使用）
// ══════════════════════════════════════════════════════

/// 创建组播发现客户端。
THIN_API thin_discovery_t* thin_discovery_client_create(
    const thin_discovery_config_t* config);

/// 发送一次 probe，等待 timeout_ms 毫秒收集回复。
/// 每收到一个回复调用 on_found 回调。
/// @return 发现的服务器数量。
THIN_API int thin_discovery_probe(thin_discovery_t* dc,
                                   int timeout_ms,
                                   thin_discovery_on_found_fn on_found,
                                   void* user_data);

/// 销毁客户端。
THIN_API void thin_discovery_client_destroy(thin_discovery_t* dc);

// ══════════════════════════════════════════════════════
// 工具
// ══════════════════════════════════════════════════════

/// 释放 thin_discovery_entry_t 中的动态字符串。
THIN_API void thin_discovery_free_entry(thin_discovery_entry_t* entry);

#ifdef __cplusplus
}
#endif

#endif // THIN_AGENT_API_DISCOVERY_H
