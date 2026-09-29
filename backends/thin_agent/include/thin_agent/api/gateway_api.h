// thin_agent C ABI — IM 网关库公共接口
//
// 职责：
//   - 飞书 Long Connection（Protobuf 二进制帧 + WSS）
//   - 微信 iLink（HTTP JSON long-poll）
//   - 消息收发，转发至 Agent WebSocket
//
// 网关独立于 agent 核心库部署，通过 WS 连接 agent 后端。

#ifndef THIN_AGENT_API_GATEWAY_H
#define THIN_AGENT_API_GATEWAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 复用 agent_api.h 的 THIN_API 宏（由构建系统定义 THIN_AGENT_BUILDING_DLL）
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
typedef struct thin_gateway_t thin_gateway_t;

// ── 配置 ──
typedef struct {
    const char* feishu_app_id;       // 飞书 App ID
    const char* feishu_app_secret;   // 飞书 App Secret
    const char* feishu_domain;       // 飞书域名，默认 "https://open.feishu.cn"
    const char* wechat_bot_id;       // 微信 Bot ID（可选）
    const char* wechat_secret;       // 微信 Secret（可选）
    const char* agent_ws_url;        // 后端 agent WS 地址，如 "ws://127.0.0.1:8765/ws"
    int         reconnect_interval;  // 重连间隔秒数，默认 5
    int         log_level;           // 0=silent, 1=info, 2=debug
} thin_gateway_config_t;

// ── 消息回调 ──
// platform: "feishu" / "wechat"
// chat_id: 会话 ID
// message: JSON 文本
typedef void (*thin_gateway_msg_fn)(const char* platform, const char* chat_id,
                                     const char* message, void* user_data);

// ══════════════════════════════════════════════════════
// 生命周期
// ══════════════════════════════════════════════════════

/// 创建网关实例。
THIN_API thin_gateway_t* thin_gateway_create(const thin_gateway_config_t* config);

/// 启动网关（连接飞书/微信长连接）。
THIN_API int thin_gateway_start(thin_gateway_t* gw);

/// 停止网关。
THIN_API int thin_gateway_stop(thin_gateway_t* gw);

/// 销毁网关实例。
THIN_API void thin_gateway_destroy(thin_gateway_t* gw);

// ══════════════════════════════════════════════════════
// 回调
// ══════════════════════════════════════════════════════

/// 设置收到消息的回调。
THIN_API void thin_gateway_set_message_callback(thin_gateway_t* gw,
                                                  thin_gateway_msg_fn callback,
                                                  void* user_data);

/// 主动发送消息到 IM 平台。
THIN_API int thin_gateway_send_message(thin_gateway_t* gw,
                                        const char* platform,
                                        const char* chat_id,
                                        const char* message_json);

/// 是否为运行状态。
THIN_API int thin_gateway_is_running(thin_gateway_t* gw);

#ifdef __cplusplus
}
#endif

#endif // THIN_AGENT_API_GATEWAY_H
