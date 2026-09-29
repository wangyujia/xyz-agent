---
created: '2026-06-07'
module: app
tags:
- websocket
- bug
- camera
title: WebSocket 断连死循环
updated: '2026-06-07'
---

## 现象
App 与相机建立 WS 连接后每 ~18 秒断开重连。

## 根因
getFileTail 单次请求 1450 个 file_id，响应 JSON 超过相机 MG_MAX_RECV_SIZE 限制，触发断连。重连后重发同样请求形成死循环。

## 修复建议
客户端拆分批次为 50~100 个/批。
