---
title: 需求文档目录
module: general
tags: [prd, requirements, index]
date: 2026-06-08
---

# 需求文档 (Requirements)

本目录存放 PRD 拆分后的总览页，每份 PRD 对应一个 overview 文件。

## 格式规范

每个 overview 文件包含：
- PRD 基本信息（版本、日期、负责人）
- 需求摘要（核心目标、范围）
- 功能列表 + 对应知识页链接
- 排期/状态

## 命名规范

```
{产品}-{版本}-overview.md
```

示例：
- app-v2.1-overview.md
- cockpit-v3.0-overview.md
- firmware-v1.5-overview.md

## 与 modules/ 的关系

```
requirements/app-v2.1-overview.md  （PRD 总览，链接到各功能页）
    → modules/camera-ota-upgrade.md
    → modules/photo-album-sync.md
    → modules/ws-protocol-v2.md
```

requirements/ 是索引，modules/ 是具体内容。
