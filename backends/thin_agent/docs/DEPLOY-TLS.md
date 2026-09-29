# TLS 终结部署指南（B 方案：前置代理）

> 决策（2026-09-11 拍板）：**选 B 前置代理终结**。当前无证书，本机 127.0.0.1
> 明文无实质暴露；正式部署申请域名+证书后按本文档落地。服务端**零改动**。

## 何时需要这份文档

- thin_agent 绑定 `--host 0.0.0.0` 对公网/局域网暴露时
- zing_agent 客户端从外部网络连回服务端时

本机使用（`ws://127.0.0.1:PORT/ws`）**不需要** TLS，鉴权 token（v0.53.32）
已是第一道防线。

## 架构

```
zing/外部客户端 ──wss:443──▶ Caddy/nginx ──ws:127.0.0.1:8765──▶ thin_agent
                              （证书终结点）
```

thin_agent 只听 127.0.0.1，代理负责 TLS。证书申请/续期全自动。

## 方案一：Caddy（推荐——证书全自动）

`/etc/caddy/Caddyfile`：

```
agent.example.com {
    reverse_proxy 127.0.0.1:8765
}
```

完成。Caddy 自动从 Let's Encrypt 申请证书、到期前自动续期。
前提：域名 DNS A 记录指向服务器、80/443 端口可达。

安装（Debian/Ubuntu）：

```bash
sudo apt install -y caddy
sudo systemctl enable --now caddy
```

## 方案二：nginx + certbot

```nginx
# /etc/nginx/sites-available/thin-agent
server {
    listen 443 ssl;
    server_name agent.example.com;

    ssl_certificate     /etc/letsencrypt/live/agent.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/agent.example.com/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:8765;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;      # WebSocket 升级头
        proxy_set_header Connection "upgrade";
        proxy_read_timeout 3600s;                     # 长连接（心跳 30s/判死 60s）
        proxy_send_timeout 3600s;
    }
}
server {
    listen 80;
    server_name agent.example.com;
    return 301 https://$host$request_uri;
}
```

证书申请（首次）：

```bash
sudo apt install -y nginx certbot python3-certbot-nginx
sudo certbot --nginx -d agent.example.com   # 自动改 nginx 配置 + 定时续期
sudo systemctl reload nginx
```

## 服务端启动（两种方案同）

```bash
# 只听本机——TLS 边界完全交给代理
thin_agent --host 127.0.0.1 --port 8765 --auth-token <你的token> ...
```

## 客户端（zing）

后端地址填：`wss://agent.example.com/ws?token=<你的token>`

（v0.53.32 鉴权：token 走 URL query，手动地址栏直接支持。）

## 验证清单

```bash
# 1. 证书生效
curl -sI https://agent.example.com/health | head -1        # 200（探针豁免鉴权）

# 2. WS over TLS + 鉴权
#    zing 连 wss://agent.example.com/ws?token=... 能收 hello 帧

# 3. 指标抓取（如接了 Prometheus）
curl -s -H "Authorization: Bearer <token>" https://agent.example.com/metrics | head -3
```

## 常见坑

| 现象 | 原因 |
|---|---|
| WS 连上即断 | nginx 没配 `Upgrade/Connection` 头（上面配置已含） |
| 60s 断线 | 代理 read_timeout < 心跳周期；本服务心跳 30s/判死 60s，代理超时须 ≥3600s 量级 |
| 证书续期失败 | 80 端口不可达（ACME HTTP-01 挑战）——检查防火墙/安全组 |
| 想省 443 冲突 | Caddy 与 nginx 二选一，别同时起 |
