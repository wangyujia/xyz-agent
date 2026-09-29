# 产品知识库技术方案

## 概述

基于 **Markdown Wiki + SQLite FTS5 (jieba 分词) + HTTP API + MCP** 的知识库方案。

### 核心特点
- **多 Agent 共享**：通过 HTTP API / MCP 协议访问，任何框架的 Agent 可用
- **读写分离**：只有知识管家 Agent 有写权限，其他 Agent 只读
- **智能问答**：/ask 接口由知识管家 LLM 整合多文档回答
- **乐观锁**：基于 sha256 的并发写控制

## 架构

详见 [ARCHITECTURE.md](./ARCHITECTURE.md)

```
其他 Agent (只读) ──→ HTTP API / MCP ──→ 知识库文件 + FTS5 索引
                           ↑
知识管家 Agent (读写) ─────┘
```

## 接入方式

### 1. HTTP API（通用，任何语言/框架）

```bash
# 搜索
curl http://kb-server:8900/search?q=相册+协议

# 读文档
curl http://kb-server:8900/doc/modules/album.md

# 智能问答
curl -X POST http://kb-server:8900/ask \
  -H "Content-Type: application/json" \
  -d '{"question": "WebSocket断开的原因有哪些？"}'
```

### 2. MCP（Hermes / Claude / Cursor 等）

```yaml
# Agent 配置
mcp:
  servers:
    knowledge-base:
      command: python
      args: ["path/to/mcp_server.py"]
```

Agent 直接调用工具：`kb_search`, `kb_read`, `kb_ask`

### 3. 知识管家 Agent（写入）

```bash
# 写接口需要 token
curl -X POST http://kb-server:8900/import \
  -H "X-KB-Token: your-token" \
  -H "Content-Type: application/json" \
  -d '{"title": "相册PRD", "type": "prd", "content": "..."}'
```

## 本地开发

```bash
cd ~/code/knowledge_base/server
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 8900 --reload
```

## 目录结构

```
knowledge_base/
├── SCHEMA.md            # 知识库规则
├── ARCHITECTURE.md      # 架构设计（详细）
├── README.md            # 本文件（快速入门）
├── index.md             # 全局索引
├── log.md               # 操作日志
├── raw/                 # 原始文档（只读）
├── modules/             # 模块知识页
├── concepts/            # 概念/术语
├── decisions/           # 技术决策
├── bugs/                # Bug 归档
├── .search.db           # FTS5 索引
├── server/              # API 服务端
└── scripts/             # 工具脚本
```

## 依赖

- Python 3.11+
- FastAPI + uvicorn
- jieba（中文分词）
- PyYAML
- mcp Python SDK
- httpx（LLM 调用）
