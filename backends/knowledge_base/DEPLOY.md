# 产品知识库 — 部署指南

> 本文档供 Hermes Agent 或运维人员参照，在新服务器上部署知识库服务。

## 概述

知识库服务包含两个组件：
1. **HTTP API Server** (:8900) — FastAPI，提供搜索/读写/问答/Web UI
2. **MCP Server** (stdio) — 供 AI Agent 原生接入

两者共享同一份代码和数据目录。

---

## 前置条件

| 项目 | 要求 |
|------|------|
| OS | Linux (Ubuntu 20.04+ / Debian 11+ / CentOS 8+) |
| Python | 3.11+ |
| 磁盘 | ≥500MB（代码 + 数据 + 索引） |
| 网络 | 内网可达，或有公网 IP + 防火墙规则 |
| 端口 | 8900 (HTTP API) 需放行 |

可选：
- Nginx（反代 + HTTPS）
- Hermes Agent（知识管家 bot 接入飞书）
- LLM 服务（/ask 接口需要，可用 Ollama 本地或远程 API）

---

## 一、环境准备

```bash
# 1. 系统依赖
sudo apt update
sudo apt install -y python3 python3-pip python3-venv git curl lsof

# 2. 确认 Python 版本
python3 --version  # 需要 3.11+

# 3. 创建部署目录
sudo mkdir -p /opt/knowledge_base
sudo chown $(whoami):$(whoami) /opt/knowledge_base
```

---

## 二、获取代码和数据

### 方式 A：从开发机打包传输（推荐首次部署）

在开发机上：
```bash
cd ~/code
tar czf knowledge_base.tar.gz \
  --exclude='knowledge_base/.search.db' \
  --exclude='knowledge_base/.server.pid' \
  --exclude='knowledge_base/.server.log' \
  --exclude='knowledge_base/server/__pycache__' \
  --exclude='knowledge_base/server/core/__pycache__' \
  knowledge_base/

scp knowledge_base.tar.gz user@目标服务器:/tmp/
```

在目标服务器上：
```bash
cd /opt
tar xzf /tmp/knowledge_base.tar.gz
# 如果解压出来是 knowledge_base/ 子目录，移动内容到 /opt/knowledge_base/
mv knowledge_base/* /opt/knowledge_base/ 2>/dev/null || true
rm -f /tmp/knowledge_base.tar.gz
```

### 方式 B：Git 拉取（如果代码已入仓库）

```bash
cd /opt
git clone <仓库地址> knowledge_base
```

---

## 三、Python 虚拟环境 + 依赖

```bash
cd /opt/knowledge_base

# 创建虚拟环境
python3 -m venv .venv
source .venv/bin/activate

# 安装依赖
pip install -r server/requirements.txt

# 验证
python -c "import fastapi, jieba, httpx, yaml; print('依赖OK')"
```

requirements.txt 内容（供确认）：
```
fastapi>=0.104.0
uvicorn[standard]>=0.24.0
jieba>=0.42.1
pyyaml>=6.0
httpx>=0.25.0
pydantic>=2.0.0
```

---

## 四、配置

### 4.1 环境变量文件

```bash
cat > /opt/knowledge_base/.env << 'EOF'
# === 基础配置 ===
KB_ROOT=/opt/knowledge_base
KB_HOST=0.0.0.0
KB_PORT=8900

# === 写权限 Token ===
# 知识管家 Agent 写入时需要此 Token
# 请修改为随机强密码，例如: openssl rand -hex 32
KB_WRITE_TOKEN=请替换为你的Token

# === LLM 配置（/ask 接口用）===
# 方式 1: 本地 Ollama
KB_LLM_API_URL=http://localhost:11434/v1
KB_LLM_API_KEY=
KB_LLM_MODEL=qwen2.5:7b

# 方式 2: 远程 API（OpenRouter/OpenAI 等）
# KB_LLM_API_URL=https://openrouter.ai/api/v1
# KB_LLM_API_KEY=sk-xxx
# KB_LLM_MODEL=qwen/qwen-2.5-72b-instruct
EOF

chmod 600 /opt/knowledge_base/.env
```

### 4.2 验证配置

```bash
cd /opt/knowledge_base
source .venv/bin/activate
set -a; source .env; set +a

python -c "
from server.config import KB_ROOT, PORT, WRITE_TOKEN, LLM_API_BASE
print(f'KB_ROOT: {KB_ROOT}')
print(f'PORT: {PORT}')
print(f'TOKEN set: {WRITE_TOKEN != \"kb-default-token-change-me\"}')
print(f'LLM: {LLM_API_BASE}')
"
```

---

## 五、初始化索引

```bash
cd /opt/knowledge_base
source .venv/bin/activate
set -a; source .env; set +a

# 启动临时服务
python -m server.main &
SERVER_PID=$!
sleep 3

# 重建索引
curl -s -X POST "http://localhost:8900/api/reindex" \
  -H "X-KB-Token: $(grep KB_WRITE_TOKEN .env | cut -d= -f2)"

# 验证搜索
curl -s "http://localhost:8900/api/search?q=知识库" | python3 -m json.tool

# 验证 Web UI
curl -s http://localhost:8900/ | head -3

# 停掉临时服务
kill $SERVER_PID
```

---

## 六、Systemd 服务（生产部署）

```bash
sudo cat > /etc/systemd/system/kb-server.service << 'EOF'
[Unit]
Description=Leaptic Knowledge Base Server
After=network.target
Wants=network-online.target

[Service]
Type=simple
User=root
Group=root
WorkingDirectory=/opt/knowledge_base
EnvironmentFile=/opt/knowledge_base/.env
ExecStart=/opt/knowledge_base/.venv/bin/python -m server.main
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

# 安全加固
NoNewPrivileges=true
ProtectSystem=strict
ReadWritePaths=/opt/knowledge_base

[Install]
WantedBy=multi-user.target
EOF

# 启用并启动
sudo systemctl daemon-reload
sudo systemctl enable kb-server
sudo systemctl start kb-server

# 验证
sudo systemctl status kb-server
curl -s http://localhost:8900/api/list | python3 -m json.tool
```

### 常用运维命令

```bash
sudo systemctl start kb-server     # 启动
sudo systemctl stop kb-server      # 停止
sudo systemctl restart kb-server   # 重启
sudo systemctl status kb-server    # 状态
journalctl -u kb-server -f         # 实时日志
journalctl -u kb-server --since "1 hour ago"  # 最近日志
```

---

## 七、（可选）Nginx 反代

适用场景：需要域名访问、HTTPS、或统一网关。

```bash
sudo apt install -y nginx

sudo cat > /etc/nginx/sites-available/knowledge-base << 'EOF'
server {
    listen 80;
    server_name kb.example.com;  # 替换为实际域名或 IP

    # Web UI + API
    location / {
        proxy_pass http://127.0.0.1:8900;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        
        # 大文档上传
        client_max_body_size 10M;
    }
}
EOF

sudo ln -sf /etc/nginx/sites-available/knowledge-base /etc/nginx/sites-enabled/
sudo nginx -t
sudo systemctl reload nginx
```

如需 HTTPS，加 certbot：
```bash
sudo apt install -y certbot python3-certbot-nginx
sudo certbot --nginx -d kb.example.com
```

---

## 八、接入知识管家 Agent（Hermes + 飞书）

### 8.1 安装 Hermes Agent

```bash
curl -fsSL https://raw.githubusercontent.com/NousResearch/hermes-agent/main/scripts/install.sh | bash
```

### 8.2 创建知识管家 Profile

```bash
hermes profile create librarian
```

### 8.3 配置 LLM Provider

```bash
hermes -p librarian model
# 选择 provider（Copilot / OpenRouter / 本地等）
# 按提示配置 API key
```

### 8.4 注册 MCP Server

```bash
hermes -p librarian mcp add knowledge-base \
  --command "/opt/knowledge_base/.venv/bin/python" \
  --args "-m,server.mcp_server"
  
# 手动编辑 config 补充 cwd 和 env:
hermes -p librarian config edit
```

在 config.yaml 的 mcp_servers 段确认：
```yaml
mcp_servers:
  knowledge-base:
    command: /opt/knowledge_base/.venv/bin/python
    args: ["-m", "server.mcp_server"]
    cwd: /opt/knowledge_base
    env:
      KB_ROOT: /opt/knowledge_base
      KB_WRITE_TOKEN: 你的Token
```

### 8.5 配置飞书 Bot

1. 登录 [飞书开放平台](https://open.feishu.cn/)
2. 创建企业自建应用 → 命名"知识管家"
3. 添加机器人能力
4. 获取 App ID + App Secret
5. 配置事件订阅：
   - 请求地址: `http://服务器IP:飞书端口/webhook/feishu`
   - 订阅事件: `im.message.receive_v1`
6. 发布应用并审核

```bash
hermes -p librarian gateway setup
# 选择 feishu → 输入 App ID / Secret / 等
```

### 8.6 安装知识管家 Skill

将知识管家 skill 复制到 librarian profile：
```bash
cp -r ~/.hermes/skills/productivity/knowledge-librarian \
      ~/.hermes/profiles/librarian/skills/
```

### 8.7 启动 Gateway

```bash
hermes -p librarian gateway install
hermes -p librarian gateway start
hermes -p librarian gateway status
```

### 8.8 验证

在飞书里 @知识管家 bot 发消息：
> 搜索知识库：WebSocket

应返回搜索结果。

---

## 九、其他 Agent / 工具接入指南

### Hermes Agent（本机）

```yaml
# ~/.hermes/config.yaml (或对应 profile)
mcp_servers:
  knowledge-base:
    command: /opt/knowledge_base/.venv/bin/python
    args: ["-m", "server.mcp_server"]
    cwd: /opt/knowledge_base
    env:
      KB_ROOT: /opt/knowledge_base
```

### Hermes Agent（远程 — 通过 HTTP）

远程 Agent 无法用 stdio MCP，直接调 HTTP API：
```bash
# 搜索
curl http://kb-server:8900/api/search?q=关键词

# 读文档
curl http://kb-server:8900/api/doc/modules/xxx.md

# 问答
curl -X POST http://kb-server:8900/api/ask \
  -H 'Content-Type: application/json' \
  -d '{"question": "WebSocket 为什么断开？"}'
```

### Cursor / VS Code (MCP)

在 `.cursor/mcp.json` 或 VS Code MCP 配置中：
```json
{
  "mcpServers": {
    "knowledge-base": {
      "command": "/opt/knowledge_base/.venv/bin/python",
      "args": ["-m", "server.mcp_server"],
      "cwd": "/opt/knowledge_base",
      "env": {
        "KB_ROOT": "/opt/knowledge_base"
      }
    }
  }
}
```

### 浏览器用户

直接访问：`http://服务器IP:8900/`
或通过 Nginx 域名：`http://kb.example.com/`

### 脚本 / CI / 自动化

```bash
# 写入文档（需要 Token）
curl -X POST http://kb-server:8900/api/doc \
  -H 'Content-Type: application/json' \
  -H 'X-KB-Token: 你的Token' \
  -d '{
    "path": "bugs/new-bug.md",
    "title": "Bug 标题",
    "body": "## 现象\n...\n## 根因\n...",
    "tags": ["bug", "app"],
    "module": "app"
  }'

# 导入文档
curl -X POST http://kb-server:8900/api/import \
  -H 'Content-Type: application/json' \
  -H 'X-KB-Token: 你的Token' \
  -d '{
    "content": "文档内容...",
    "title": "文档标题",
    "target_dir": "modules",
    "tags": ["tag1", "tag2"],
    "module": "app"
  }'
```

---

## 十、数据备份与恢复

### 备份

```bash
# 备份知识文件（不含索引和临时文件）
tar czf kb_backup_$(date +%Y%m%d).tar.gz \
  --exclude='.search.db*' \
  --exclude='.server.*' \
  --exclude='server/__pycache__' \
  --exclude='.venv' \
  -C /opt knowledge_base/

# 建议 cron 每日备份
echo "0 2 * * * tar czf /backup/kb_\$(date +\%Y\%m\%d).tar.gz --exclude='.search.db*' --exclude='.venv' -C /opt knowledge_base/" | crontab -
```

### 恢复

```bash
# 解压备份
cd /opt && tar xzf /backup/kb_20260607.tar.gz

# 重建索引
curl -X POST http://localhost:8900/api/reindex -H 'X-KB-Token: 你的Token'
```

---

## 十一、故障排查

| 问题 | 检查方式 | 解决 |
|------|---------|------|
| 服务不响应 | `systemctl status kb-server` | `systemctl restart kb-server` |
| 端口被占 | `lsof -i:8900` | 杀掉占用进程 |
| 搜索无结果 | `curl /api/list` 看文档数 | 重新 reindex |
| 403 写入被拒 | 检查 X-KB-Token | 确认 .env 中的 Token |
| /ask 超时 | 检查 LLM 服务 | 确认 KB_LLM_API_URL 可达 |
| 索引损坏 | 删除 .search.db | 重新 reindex |
| 依赖缺失 | `pip list` | `pip install -r server/requirements.txt` |

### 查看日志

```bash
# Systemd 日志
journalctl -u kb-server -f

# 或直接看进程日志
tail -f /opt/knowledge_base/.server.log
```

---

## 十二、升级

```bash
# 1. 从开发机打包新代码（不含数据）
tar czf kb_code_update.tar.gz \
  knowledge_base/server/ \
  knowledge_base/scripts/ \
  knowledge_base/start_server.sh \
  knowledge_base/SCHEMA.md

# 2. 传到服务器并解压覆盖
scp kb_code_update.tar.gz user@server:/tmp/
ssh user@server "cd /opt && tar xzf /tmp/kb_code_update.tar.gz"

# 3. 重启服务
ssh user@server "sudo systemctl restart kb-server"

# 4. 验证
curl http://server:8900/api/list
```

---

## API 速查

| 接口 | 方法 | 认证 | 说明 |
|------|------|------|------|
| `/api/search?q=xxx&limit=20` | GET | 无 | 搜索 |
| `/api/doc/{path}` | GET | 无 | 读取文档 |
| `/api/list?directory=&module=` | GET | 无 | 列出文档 |
| `/api/ask` | POST | 无 | 智能问答 |
| `/api/doc` | POST | X-KB-Token | 写入/更新文档 |
| `/api/doc/{path}` | DELETE | X-KB-Token | 删除文档 |
| `/api/import` | POST | X-KB-Token | 导入文档 |
| `/api/reindex` | POST | X-KB-Token | 重建索引 |
| `/` | GET | 无 | Web 搜索页 |
| `/docs` | GET | 无 | Swagger API 文档 |

---

## MCP 工具速查

| 工具 | 权限 | 说明 |
|------|------|------|
| `kb_search` | 只读 | 搜索知识库 |
| `kb_read` | 只读 | 读取文档 |
| `kb_list` | 只读 | 列出文档 |
| `kb_ask` | 只读 | 智能问答 |
| `kb_write` | 写 | 写入/更新文档 |
| `kb_import` | 写 | 导入文档 |
| `kb_reindex` | 写 | 重建索引 |
