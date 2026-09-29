# thin_agent 商用落地差距评估（R21 检视产出，2026-09-10）

对照可商用 Agent 标准（参考 OpenAI/Claude 桌面产品、LangServe、Dify 等行业实践），
对服务端+客户端的逐项检视。分四档：✅ 达标 / 🟡 单机场景达标 / 🟠 需补齐 / ❌ 缺失。

## 一、安全（Security）

| 项 | 现状 | 档位 | 说明 |
|---|---|---|---|
| WS/HTTP 鉴权 | 无——任意客户端连上即全功能 | ❌ | 单机 127.0.0.1 绑定下风险受控；**一旦 `--host 0.0.0.0`（zing 远程安装就这么做）= 局域网裸奔**（聊天、shell_exec、文件读写全开放）。商用必补：握手 token（如 `ws://host:port/ws?token=xxx`，服务端启动时生成/配置注入） |
| TLS/wss | 无（决策：B 前置代理） | 🟠 | 同上部署边界下明文。**已拍板 B 方案**——正式部署时按 [DEPLOY-TLS.md](DEPLOY-TLS.md) 前置 Caddy/nginx 终结（证书后补，服务端零改动） |
| 工具执行防护 | CommandValidator 危险命令检测+HITL 审批（v0.49）+沙箱（code_exec 子进程组/深度限制） | ✅ | 分层防护完善 |
| API key 管理 | credential pool 轮换+env 注入 | ✅ | — |
| 客户端凭据 | ssh key 路径存 state.json（非密钥本体） | 🟡 | 单用户桌面设计内 |

## 二、可靠性（Reliability）

| 项 | 现状 | 档位 |
|---|---|---|
| LLM 容错 | 重试+jitter+熔断+多 provider fallback（v0.52 系） | ✅ |
| 优雅停机 | SIGTERM→worker drain（g_stop） | ✅ |
| 进程守护 | systemd unit（非特权用户+Restart=on-failure） | ✅ |
| 会话持久化 | jsonl 落盘+重启回灌（v0.53.28） | ✅ |
| 流式中断 | 迭代+回放双检查点（v0.53.30/31） | ✅ |
| **客户端离线队列** | 断线时发送直接失败，用户输入**不排队不保留** | ❌ | 商用标准：断线期间输入排队，重连后自动重发+输入框内容不丢 |
| 客户端崩溃恢复 | v7 C++ 落盘后会话/消息/主题全恢复 | ✅ |

## 三、可观测（Observability）

| 项 | 现状 | 档位 |
|---|---|---|
| 结构化日志 | log_event（event+level+kv） | ✅ |
| 运行指标 | /stats+metrics_payload（uptime/usage/cache/circuit/sessions/队列深度） | ✅ |
| 决策审计 | append_decision_audit（每次 chat 的路由/耗时） | ✅ |
| 外部导出 | 无 Prometheus/OTel 集成 | 🟡 | /stats JSON 轮询可补；商用 SRE 需标准导出器 |

## 四、产品能力（Product）

| 项 | 现状 | 档位 |
|---|---|---|
| 流式输出 | 最终轮回放（v0.53.30）；真逐 token=SSE 回调化待改造 | 🟡 |
| 多会话并发 | worker 池+cmd_id 分流（v0.53.30） | ✅ |
| 版本协商 | hello 帧带 version | 🟡（无 protocol_version 协商） |
| Provider 矩阵 | copilot/deepseek/openai-compatible 三种 | 🟠 | LM Studio 等本地推理应一等公民 |
| 多租户/配额 | 无（单用户模型） | 🟡 | 设计定位=个人 agent；商用多租户需重构 |

## 结论（按优先级）

1. **P0（上生产前必须）**：WS 鉴权 token——远程安装（0.0.0.0）场景下当前是裸奔
2. **P1**：客户端离线队列（断线输入不丢）
3. **P1**：真逐 token 流式（首字延迟）
4. **P2**：TLS 终结 → **已决策 B 前置代理**（见 DEPLOY-TLS.md，正式部署时落地）；Prometheus 导出器 → ✅ v0.53.35
5. **P2**：LM Studio 本地 provider 一等公民
