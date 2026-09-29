# 产品知识库 — 架构设计

## 总体架构

```
┌───────────────────────────────────────────────────────────────────┐
│                         Client Agents                              │
│                                                                   │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐ │
│  │ Hermes Agent│  │ 其他 Agent  │  │ Cursor/IDE │  │  脚本/工具  │ │
│  │ (MCP 接入)  │  │ (HTTP 接入) │  │ (MCP 接入) │  │ (HTTP 接入) │ │
│  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘ │
│        │  MCP          │  HTTP         │  MCP          │  HTTP   │
└────────┼───────────────┼───────────────┼───────────────┼─────────┘
         │               │               │               │
         ▼               ▼               ▼               ▼
┌───────────────────────────────────────────────────────────────────┐
│                    Knowledge Base Server                           │
│                                                                   │
│  ┌─────────────────┐    ┌─────────────────────────────────────┐  │
│  │  MCP Server      │    │  FastAPI HTTP Server (:8900)         │  │
│  │  (stdio / SSE)   │    │                                     │  │
│  │                  │    │  GET  /search    ← 只读              │  │
│  │  tools:          │    │  GET  /doc/{p}   ← 只读              │  │
│  │  - kb_search     │    │  GET  /index     ← 只读              │  │
│  │  - kb_read       │    │  GET  /modules   ← 只读              │  │
│  │  - kb_import  🔒 │    │  POST /doc/{p}   ← 🔒 写（需 token）│  │
│  │  - kb_write   🔒 │    │  POST /import    ← 🔒 写（需 token）│  │
│  │  - kb_update  🔒 │    │  POST /reindex   ← 🔒 写（需 token）│  │
│  │  - kb_ask     🧠 │    │  DELETE /doc/{p} ← 🔒 写（需 token）│  │
│  └────────┬─────────┘    │  POST /ask       ← 🧠 智能问答       │  │
│           │              └────────────┬────────────────────────┘  │
│           │                           │                           │
│           └──────────┬────────────────┘                           │
│                      ▼                                            │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │                    Core Logic Layer                          │  │
│  │                                                             │  │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌───────────┐  │  │
│  │  │ searcher │  │ reader   │  │ writer   │  │ librarian │  │  │
│  │  │ (jieba+  │  │ (读文件  │  │ (写文件+ │  │ (知识管家  │  │  │
│  │  │  FTS5)   │  │  解析FM) │  │  乐观锁) │  │  LLM整合) │  │  │
│  │  └──────────┘  └──────────┘  └──────────┘  └───────────┘  │  │
│  └─────────────────────────────────────────────────────────────┘  │
│                      │                                            │
│                      ▼                                            │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │                    Storage Layer                             │  │
│  │                                                             │  │
│  │  ~/code/knowledge_base/                                     │  │
│  │  ├── .search.db          (SQLite FTS5)                      │  │
│  │  ├── raw/                (原始文档，不可变)                    │  │
│  │  ├── modules/            (知识页)                            │  │
│  │  ├── concepts/           (概念)                              │  │
│  │  ├── decisions/          (决策)                              │  │
│  │  └── bugs/               (Bug归档)                           │  │
│  └─────────────────────────────────────────────────────────────┘  │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

## 权限模型

```
┌─────────────────────────────────────────────────────────┐
│                    权限分层                               │
├──────────────┬───────────────────┬──────────────────────┤
│   角色        │   能力             │   认证方式           │
├──────────────┼───────────────────┼──────────────────────┤
│   reader     │   搜索、读文档     │   无需 token         │
│              │   列目录           │   （默认角色）        │
├──────────────┼───────────────────┼──────────────────────┤
│   writer     │   reader 全部 +    │   Header:            │
│   (知识管家)  │   写文档、导入、    │   X-KB-Token: xxx    │
│              │   删除、重建索引    │                      │
├──────────────┼───────────────────┼──────────────────────┤
│   librarian  │   writer 全部 +    │   同 writer token +  │
│   (智能问答)  │   /ask 智能整合    │   需配置 LLM         │
│              │   多文档回答        │                      │
└──────────────┴───────────────────┴──────────────────────┘
```

**实际部署**：知识管家 Agent 持有 writer token，其他 Agent 无需 token（只读）。

## 知识管家 Agent（Librarian）

### 职责

```
┌────────────────────────────────────────────────────┐
│                知识管家 Agent                        │
├────────────────────────────────────────────────────┤
│                                                    │
│  1. 智能问答                                        │
│     其他 Agent 问: "相册模块的分页大小是多少？"        │
│     管家: 搜索多篇文档 → 整合 → 返回精准答案          │
│                                                    │
│  2. 文档导入整理                                     │
│     用户发 PRD → 管家理解内容 → 拆分 → 存入对应模块    │
│                                                    │
│  3. 知识维护                                        │
│     - 发现过时信息 → 更新                            │
│     - 检测矛盾 → 标记                               │
│     - 定期审计（orphan pages、broken links）          │
│                                                    │
│  4. Bug 归档                                        │
│     分析完 Bug → 自动整理 → 写入 bugs/               │
│                                                    │
└────────────────────────────────────────────────────┘
```

### /ask 接口 — 智能问答流程

```
其他 Agent:
  POST /ask { "question": "WebSocket 断开的已知原因有哪些？" }

知识管家内部:
  1. jieba 分词 → FTS5 搜索相关文档
  2. 读取 top-N 相关文档全文
  3. LLM 整合回答（带引用来源）
  4. 返回结构化答案

响应:
  {
    "answer": "已知有3种原因：1. getFileTail批量过大...",
    "sources": ["modules/websocket.md", "bugs/smar-21685.md"],
    "confidence": "high"
  }
```

### 与普通搜索的区别

| | /search（关键词搜索） | /ask（智能问答） |
|--|--|--|
| 输入 | 关键词 | 自然语言问题 |
| 输出 | 匹配文档列表+摘要 | 整合后的答案 |
| 消耗 | 零 LLM token | 需要 LLM 调用 |
| 速度 | 毫秒级 | 秒级 |
| 精度 | 依赖关键词准确 | 能理解模糊问题 |
| 适合 | Agent 精确查参数 | Agent 问复杂问题 |

## 乐观锁设计

```
写入流程:

1. Client 读文档 → 拿到 sha256
   GET /doc/modules/album.md
   → { "content": "...", "sha256": "abc123..." }

2. Client 修改后提交，带上原始 sha256
   POST /doc/modules/album.md
   Body: { "content": "新内容...", "base_sha256": "abc123..." }

3. Server 检查:
   - 当前文件 sha256 == base_sha256 → 写入成功 ✅
   - 当前文件 sha256 != base_sha256 → 冲突拒绝 ❌
     → 返回 409 + 当前最新内容

冲突处理:
   - 知识管家 Agent 收到 409 → 重新读取 → 合并 → 重试
```

## MCP Server 设计

### 工具定义

```json
{
  "tools": [
    {
      "name": "kb_search",
      "description": "搜索产品知识库，返回匹配文档列表",
      "inputSchema": {
        "type": "object",
        "properties": {
          "query": { "type": "string", "description": "搜索关键词" },
          "module": { "type": "string", "description": "按模块过滤（可选）" },
          "limit": { "type": "integer", "default": 5 }
        },
        "required": ["query"]
      }
    },
    {
      "name": "kb_read",
      "description": "读取知识库中的一篇文档",
      "inputSchema": {
        "type": "object",
        "properties": {
          "path": { "type": "string", "description": "文档路径，如 modules/album.md" }
        },
        "required": ["path"]
      }
    },
    {
      "name": "kb_ask",
      "description": "向知识管家提问，获取整合后的答案（会消耗 LLM token）",
      "inputSchema": {
        "type": "object",
        "properties": {
          "question": { "type": "string", "description": "自然语言问题" }
        },
        "required": ["question"]
      }
    },
    {
      "name": "kb_import",
      "description": "【需写权限】导入原始文档到知识库",
      "inputSchema": {
        "type": "object",
        "properties": {
          "title": { "type": "string" },
          "content": { "type": "string" },
          "type": { "type": "string", "enum": ["prd", "spec", "protocol"] }
        },
        "required": ["title", "content"]
      }
    },
    {
      "name": "kb_write",
      "description": "【需写权限】创建或更新知识页",
      "inputSchema": {
        "type": "object",
        "properties": {
          "path": { "type": "string", "description": "文档路径" },
          "content": { "type": "string", "description": "完整 Markdown 内容" },
          "base_sha256": { "type": "string", "description": "乐观锁：上次读取的 sha256" }
        },
        "required": ["path", "content"]
      }
    }
  ]
}
```

### MCP 传输方式

| 方式 | 场景 |
|------|------|
| stdio | 本机 Agent（Hermes config.yaml 配置） |
| SSE (HTTP) | 远程 Agent（跨机器访问） |

**Hermes 配置示例（本机 stdio）：**
```yaml
# ~/.hermes/config.yaml
mcp:
  servers:
    knowledge-base:
      command: python
      args: ["/root/code/knowledge_base/server/mcp_server.py"]
      env:
        KB_WRITE_TOKEN: "xxx"  # 知识管家才配这个
```

**远程 Agent（SSE 方式）：**
```yaml
mcp:
  servers:
    knowledge-base:
      url: "http://kb-server:8900/mcp/sse"
```

## HTTP API 详细定义

### 只读接口（无需认证）

```
GET /search?q={query}&module={module}&limit={n}
→ 200: { "results": [...], "total": N, "query_tokenized": "..." }

GET /doc/{path}
→ 200: { "path": "...", "title": "...", "content": "...", "frontmatter": {...}, "sha256": "..." }
→ 404: { "error": "not found" }

GET /index
→ 200: { "content": "index.md 全文", "stats": { "modules": N, "concepts": N, ... } }

GET /modules
→ 200: { "modules": [{ "path": "...", "title": "...", "tags": [...] }, ...] }

GET /health
→ 200: { "status": "ok", "docs_count": N, "last_reindex": "..." }
```

### 写接口（需 X-KB-Token header）

```
POST /doc/{path}
Headers: X-KB-Token: {writer_token}
Body: { "content": "...", "base_sha256": "..." }
→ 200: { "path": "...", "sha256": "...", "updated": true }
→ 409: { "error": "conflict", "current_sha256": "...", "current_content": "..." }
→ 403: { "error": "forbidden" }

POST /import
Headers: X-KB-Token: {writer_token}
Body: { "title": "...", "type": "prd|spec|protocol", "content": "...", "tags": [...] }
→ 200: { "raw_path": "...", "sha256": "..." }

DELETE /doc/{path}
Headers: X-KB-Token: {writer_token}
→ 200: { "deleted": true }

POST /reindex
Headers: X-KB-Token: {writer_token}
→ 200: { "indexed": N, "duration_ms": ... }
```

### 智能问答接口

```
POST /ask
Body: { "question": "...", "context": "可选的额外上下文" }
→ 200: {
    "answer": "整合后的回答...",
    "sources": ["modules/websocket.md", "bugs/smar-21685.md"],
    "confidence": "high|medium|low",
    "tokens_used": 1234
  }
```

## 目录结构（更新后）

```
~/code/knowledge_base/
├── SCHEMA.md
├── index.md
├── log.md
├── README.md
├── ARCHITECTURE.md          # 本文件
│
├── raw/                     # 原始文档（只读）
│   ├── prd/
│   └── specs/
│
├── modules/                 # 知识页
├── concepts/
├── decisions/
├── bugs/
│
├── .search.db               # FTS5 索引
│
├── server/                  # 服务端代码
│   ├── main.py              #   FastAPI 主入口
│   ├── mcp_server.py        #   MCP Server
│   ├── core/                #   核心逻辑
│   │   ├── searcher.py      #     搜索（jieba + FTS5）
│   │   ├── reader.py        #     读取 + frontmatter 解析
│   │   ├── writer.py        #     写入 + 乐观锁
│   │   ├── importer.py      #     文档导入
│   │   └── librarian.py     #     知识管家 LLM 整合
│   ├── config.py            #   配置（token、LLM、路径）
│   └── requirements.txt     #   Python 依赖
│
└── scripts/                 # 独立工具脚本（保留）
    ├── import.py
    ├── reindex.py
    └── search.py
```

## 部署

```bash
# 安装依赖
cd ~/code/knowledge_base/server
pip install -r requirements.txt

# 配置
export KB_WRITE_TOKEN="your-secret-token"
export KB_LLM_API_URL="http://..."        # /ask 用的 LLM
export KB_LLM_API_KEY="..."

# 启动
uvicorn main:app --host 0.0.0.0 --port 8900

# 或后台运行
nohup uvicorn main:app --host 0.0.0.0 --port 8900 &
```

## 典型交互流程

### 场景1：其他 Agent 查询知识

```
[Agent A] → GET /search?q=WebSocket断开
         ← results: [modules/websocket.md, bugs/smar-21685.md]
         
[Agent A] → GET /doc/modules/websocket.md
         ← { content: "...", sha256: "..." }
```

### 场景2：其他 Agent 问复杂问题

```
[Agent A] → POST /ask { "question": "相册模块请求相机时有哪些已知的坑？" }
         ← {
              "answer": "已知3个坑：1. getFileTail不能超过100个ID...",
              "sources": ["modules/album.md", "bugs/smar-21685.md"],
              "confidence": "high"
            }
```

### 场景3：知识管家导入新 PRD

```
[用户 via 飞书] → "导入这个PRD：[内容]"
[知识管家 Agent] → POST /import { title, content, type: "prd" }
                 → 分析内容，拆分模块
                 → POST /doc/modules/xxx.md { content, base_sha256 }
                 → POST /reindex
                 ← "✅ 已导入，更新了3个知识页"
```

### 场景4：知识管家自动归档 Bug

```
[知识管家] 分析完 Bug 后:
         → POST /doc/bugs/smar-21685.md { content }
         → 读取 modules/websocket.md
         → POST /doc/modules/websocket.md { 更新内容, base_sha256 }
         → POST /reindex
```

## 技术依赖

| 依赖 | 用途 | 版本 |
|------|------|------|
| Python | 运行环境 | 3.11+ |
| FastAPI | HTTP 服务 | 最新 |
| uvicorn | ASGI 服务器 | 最新 |
| jieba | 中文分词 | 最新 |
| PyYAML | frontmatter 解析 | 最新 |
| mcp (Python SDK) | MCP Server | 最新 |
| httpx | /ask 调 LLM | 最新 |
| SQLite3 | FTS5 索引 | 内置 |

## 后续扩展

- [ ] 飞书文档 webhook：文档更新时自动通知知识管家
- [ ] 向量搜索层：/search 增加 semantic=true 选项
- [ ] 多租户：按团队/项目隔离知识库
- [ ] Web UI：人类浏览/搜索界面
- [ ] 知识图谱可视化：模块间关系图
