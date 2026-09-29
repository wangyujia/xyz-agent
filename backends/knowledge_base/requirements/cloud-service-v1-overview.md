---
title: 云服务（云存储）PRD v1.0 总览
module: firmware
tags: [cloud, wifi, sync, ota, prd, v1]
source: PRD-云服务v1
date: 2026-06-08
status: confirmed
---

# 云服务（云存储）PRD v1.0

## 核心目标

构建设备与云平台之间安全、稳定、可扩展的数据通道，实现：
1. 设备通过 STA 模式连接公网 WiFi → 自动鉴权 → 连接 Leaptic Anywhere 云服务
2. 影像资料自动云端同步
3. 为 AI 智能创作、云剪辑等高级服务奠定基础

## 产品价值

- **提升体验**：设置一次，永久连接
- **数据闭环**：无需干预，自动上传
- **赋能云端**：AI 创作、云剪辑的数据基础
- **增强粘性**：无缝云服务体验

## 子需求拆分

| 子需求 | 知识页 | 核心内容 |
|--------|--------|---------|
| 设备自动连接 | [[cloud-wifi-config]] | WiFi 配置、多网络存储、自动重连、连接状态 |
| 鉴权与绑定 | [[cloud-auth-binding]] | 绑定云服务、OAuth 鉴权、心跳机制 |
| 影像同步 | [[cloud-image-sync]] | 自动/手动同步、增量同步、断点续传 |
| 影像传输 | [[cloud-transfer-process]] | 前置校验、传输状态、传输限制 |
| 云服务入口与设置 | [[cloud-service-ui]] | 下拉菜单入口、设置页面 |
| 异常处理 | [[cloud-exceptions]] | 连接异常、同步异常 |

## 关联文档

- 原始 PRD：raw/prd/cloud-service-v1.md
- 订阅与支付：另建文档（待补充）

## 涉及模块

- 相机固件（WiFi STA、存储、鉴权、传输）
- App（WiFi 配置、蓝牙传输、状态同步）
- 服务端（账号绑定、UAT 签发、文件接收）
