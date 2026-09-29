# 知识库规则定义

## 域

Leaptic 产品知识库 —— 涵盖相机 App、座舱、相机固件等产品的 PRD、技术规范、协议文档、Bug 根因分析和技术决策。

## 目录结构

```
knowledge_base/
├── SCHEMA.md          # 本文件：规则定义
├── index.md           # 全局索引（目录）
├── log.md             # 操作日志（追加写）
├── raw/               # 原始文档（只读，不可修改）
│   ├── prd/           #   产品需求文档
│   └── specs/         #   技术规范/协议文档
├── requirements/      # PRD 总览页（每份 PRD 一个 overview）
├── modules/           # 按产品模块拆分的知识页（PRD 功能点拆入此处）
├── concepts/          # 通用概念、术语解释
├── decisions/         # 技术决策记录 (ADR)
├── bugs/              # 重要 Bug 根因归档
├── .search.db         # SQLite FTS5 索引（自动生成）
└── scripts/           # 工具脚本
```

## 文档格式规范

每个知识页使用 YAML frontmatter + Markdown：

```yaml
---
title: 页面标题
type: module | concept | decision | bug
tags: [标签1, 标签2]
created: YYYY-MM-DD
updated: YYYY-MM-DD
sources: [raw/prd/xxx.md, raw/specs/xxx.md]
---
```

## 文件命名

- 小写英文，连字符分隔，无空格
- 例: `album.md`, `camera-protocol.md`, `mg-max-recv-size.md`
- Bug 文件以 Jira 号命名: `smar-21685.md`

## 标签体系

### 模块标签
- `相册` / `album` — 相册/文件管理
- `预览` / `preview` — 预览/座舱模式
- `websocket` / `通信` — WebSocket 通信层
- `相机控制` / `camera-control` — 相机参数/模式控制
- `录像` / `recording` — 录像/拍照
- `设置` / `settings` — App/相机设置
- `固件` / `firmware` — 相机固件
- `座舱` / `cockpit` — 座舱系统
- `OTA` — 固件升级
- `WebRTC` — 实时预览流

### 文档类型标签
- `prd` — 产品需求
- `spec` — 技术规范
- `protocol` — 通信协议
- `bug` — Bug 分析
- `decision` — 技术决策

### 状态标签
- `已修复` — Bug 已修复
- `待修复` — Bug 待修复
- `已废弃` — 功能/协议已废弃

> 规则：每个标签必须在此体系中存在。如需新增标签，先更新此处再使用。

## 知识页规则

### 创建条件
- PRD 总览一个文件（requirements/），拆出的功能点放 modules/
- 一个模块/功能一个知识页（modules/）
- 一个重要概念/术语一个页面（concepts/）
- 一个 Bug 根因分析一个页面（bugs/）
- 一个技术决策一个页面（decisions/）

### 内容要求
- 每个页面至少链接 2 个其他页面（`[[wikilink]]`）
- 模块页面需包含：概述、协议接口、关键参数、关联、已知问题
- Bug 页面需包含：现象、根因、影响范围、修复方案、教训
- 决策页面需包含：背景、方案对比、结论、风险

### 页面长度
- 单页不超过 200 行
- 超过时拆分为子主题页面

## 原始文档规则（raw/）

- **不可修改**：raw/ 下的文件一旦导入，永不修改
- 导入时记录 sha256，用于检测意外篡改
- 新版本的 PRD 作为新文件导入（如 camera-album-prd-v2.md）

## 索引规则

### index.md
- 每个知识页在 index.md 中有一行记录
- 格式：`- [[文件名]] — 一句话描述`
- 按类型分节（Modules / Concepts / Decisions / Bugs）
- 新增/删除页面时必须同步更新

### log.md
- 每次操作追加一条记录
- 格式：`[YYYY-MM-DD HH:MM] 动作 | 说明`
- 动作类型：`导入` / `更新` / `创建` / `搜索` / `归档`

### FTS5 索引（.search.db）
- 使用 jieba 分词
- 内容变更后需重建索引（调用 scripts/reindex.py）
- 索引字段：title, content, tags, module, path

## 输入方式

### 主要输入
1. **飞书对话**：用户粘贴文本或发送飞书文档链接给 Agent
2. **Agent 自动归档**：分析完 Bug/问题后，Agent 主动提议归档

### 长文档处理
- 用户说"分段发送" → Agent 进入拼接模式
- 用户发 `/end` 或说"发完了" → Agent 开始处理
- 短文档直接发送，无需特殊指令

### 导入处理流程
1. 识别文档类型（PRD / 规范 / 协议）
2. 原文存入 raw/（保留完整原文，计算 sha256）
3. 提取关键信息：涉及模块、新概念、关联已有知识
4. 创建/更新对应的 modules/ concepts/ 页面
5. 更新 index.md + FTS5 索引 + log.md
6. 回复用户确认结果

## 搜索使用

Agent 通过调用 `scripts/search.py` 进行搜索：
```bash
python ~/code/knowledge_base/scripts/search.py "查询关键词"
```

也可直接用 search_files 做简单文本搜索：
```
search_files "关键词" path="~/code/knowledge_base/" file_glob="*.md"
```

## 自动归档规则

Agent 在以下场景主动提议归档：
- 完成 Bug 根因分析后 → 归档到 bugs/
- 发现新的技术限制/约束 → 更新 modules/ 或创建 concepts/
- 做出技术决策后 → 归档到 decisions/

归档前需用户确认。
