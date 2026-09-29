# thin_agent vs 业界 Agent 框架对比

> **对比基准版本**: thin_agent **v0.38.2**
> 
> **对比日期**: 2026-07-29（v0.38.x 更新）
> 
> **目的**: 追踪 thin_agent 与主流 Agent 框架的能力差距，指导版本演进方向。
> 
> 📖 **新**: 与 Hermes Agent 的详细维度对比见 [`AGENT_COMP_HERMES.md`](./AGENT_COMP_HERMES.md) — 工具系统、自主性、平台、Provider、技能系统逐项分析。
> 
> ---
> 
> **更新记录**:
> 
| 日期 | thin_agent 版本 | 主要变化 | 更新人 |
|------|-----------------|----------|--------|
| 2026-09-01 | v0.53.10/11 | web_search 真实数据源（Bing 适配器）+ 运行时配置（WS web_search_config 可改可存）+ FC 工具化（LLM 自主搜索决策）；智谱 zai 源协议验证留作配置项 | 助手 |
| 2026-08-31 | v0.53.8 | MCP 外接兼容包收官+多语言执行+工具链多源 fallback：市面 Python/Node MCP server 全量可接（npx 真端到端实证，修 2 存量缺陷=配置从未生效根因）；code_exec 语言类白名单（词表驱动，新语言零代码）；FC 工具链 cloud_providers 冗余（GLM 熔断不再瘫复杂任务）；子代理钩子闸闭环；Release 产物 6.32MB | Hermes |
| 2026-07-29 | v0.38.2 | v0.38.0-0.38.2 三轮断路修复（语义记忆+技能自进化+目标纠错主路径注入）；自主性 4.5→5⭐ 对齐 Hermes；新增 3 个测试套件（15 项断言）；Memory 维度 5⭐ | Hermes |
| 2026-07-29 | v0.37.4 | 审计更新：修正对比文档误标（Session Search/Webhook/Curator 实际已实现）；自主性 4→4.5⭐；剩余 12 项差距重新排序 | Hermes |
| 2026-07-27 | v0.36.2 | v0.30~v0.36 补齐 7 项 P0：PTY+BG/Sandbox/Security/MCP/CLI/Cron/Tele+Discord；重新评分自主性 3→4⭐；识别 15 项剩余差距 | Hermes |
| 2026-07-20 | v0.27.3 | KB系统（FTS5+插件）上线 + 插件系统 (.so) + 测试 14/14 + 接口抽象 | Hermes |
| 2026-07-12 | v0.10.5 | v0.10.5 全面复审：16/16 补齐，识别 8 项新差距 + 3 项非硬差距 + 优先级建议 | Hermes |
| 2026-07-12 | v0.10.5 | +技能生命周期 +自适应路由 +跨会话学习 +目标驱动 +主动监控 +子Agent；自主性 ⭐⭐⭐⭐⭐ | Hermes |
| 2026-07-12 | v0.9.4 | +Provider多源 +模型热切换 +自我纠错 +长期目标管理；补齐测试缺口 | Hermes |
| 2026-07-12 | v0.9.0-2 | 修复行级不一致，重新评分；+4 bugfix (递归锁×3 + response_format) | Hermes |
| 2026-07-12 | v0.9.0 | +Summarizer +Cron +Checkpoint +SkillManager +MCP +Orchestrator | Hermes |
| 2026-07-06 | v0.8.53 | 初始版本，P0+P1 全覆盖 | Hermes |

---

## 对标对象

| 框架 | 定位 | 语言 | 官网/仓库 |
|------|------|------|-----------|
| **thin_agent** (我们) | 全场景轻量 Agent 平台 | C++17 | `~/code/thin_agent` |
| **Hermes Agent** (Nous Research) | 通用自主 Agent 平台 | Python | https://github.com/NousResearch/hermes-agent |
| **Claude Code** (Anthropic) | 编码 Agent | TypeScript | https://github.com/anthropics/claude-code |
| **LangChain / LangGraph** | Agent 编排框架 | Python/JS | https://github.com/langchain-ai/langgraph |
| **OpenAI Agents SDK** | 轻量 Agent 框架 | Python | https://github.com/openai/openai-agents-python |
| **CrewAI** | 多 Agent 编排 | Python | https://github.com/crewAIInc/crewAI |
| **AutoGPT** | 自主 Agent | Python | https://github.com/Significant-Gravitas/AutoGPT |

---

## 一、架构对比

| 维度 | thin_agent | Hermes Agent | Claude Code | LangGraph | OpenAI SDK | CrewAI |
|------|-----------|-------------|-------------|-----------|------------|--------|
| **语言** | C++17 ✅ | Python | TypeScript | Python | Python | Python |
| **二进制体积** | 6.1MB ✅ | ~200MB venv | ~300MB node | ~500MB deps | ~200MB | ~300MB |
| **启动时间** | <100ms ✅ | 2-5s | 1-3s | 3-8s | 1-3s | 3-5s |
| **内存占用** | ~50MB (纯推理) | ~500MB+ | ~400MB+ | ~1GB+ | ~300MB+ | ~400MB+ |
| **嵌入式部署** | ✅ 直接交叉编译 | ❌ | ❌ | ❌ | ❌ | ❌ |
| **推理循环** | AgentLoop (多步) | Tool-calling loop | Agent loop | Graph state machine | Runner loop | Task-based |
| **多 Agent** | ✅ 角色化+DAG+辩论+看板 | ✅ (spawn/delegate/kanban) | ❌ | ✅ | ❌ | ✅ (核心) |
| **混合推理** | ✅ 本地 + 云端 | ✅ 多 provider | ✅ (API only) | ✅ 多 provider | ✅ OpenAI only | ✅ 多 provider |
| **True Sandbox** | ✅ clone+ns+cgroups (v0.36) | ✅ Docker/SSH/Modal | ✅ 子进程 | ❌ | ❌ | ❌ |
| **Filesystem Checkpoint** | ✅ content-addressed (v0.31) | ✅ /rollback | ❌ | ❌ | ❌ | ❌ |

> **thin_agent 评价**: 6.1MB 二进制 + <100ms 启动 + 交叉编译能力，使其成为从嵌入式 MCU 到云端服务器的全场景唯一选择。v0.36 补齐 True Sandbox 后，安全执行能力对齐 Hermes 和 Claude Code。多 Agent 维度保持 ⭐⭐⭐⭐⭐——角色系统（9 角色+工具白名单）、DAG 依赖编排（Kahn 拓扑+失败级联传播）、动态任务分解（LLM 自动拆目标）、Agent 间直接通信（SubAgentBus+@mention）、共享上下文黑板（Blackboard）、Kanban 看板（自主抢单）、协商辩论（多角色多轮 debate→共识）。

---

## 二、模型 & Provider

| 维度 | thin_agent | Hermes Agent | Claude Code | OpenAI SDK |
|------|-----------|-------------|-------------|------------|
| **Provider 数量** | 3 (deepseek/compat/copilot) | 21+ ✅ | 1 (Anthropic) | 1 (OpenAI) |
| **本地模型** | ✅ GGUF/ONNX/Template/ModelPool | ✅ (via openai-compat) | ❌ | ❌ |
| **模型热切换** | ✅ WS switch_model | ✅ /model 即时切 | ❌ | ❌ |
| **级联降级** | ✅ Template→Qwen→Gemma→Cloud | ✅ credential pools | ❌ | ❌ |
| **流式推理** | ✅ token 级 (WS) | ✅ SSE | ✅ | ✅ |
| **ONNX 推理** | ✅ 意图分类 (3.3KB) | ❌ | ❌ | ❌ |
| **Anthropic** | ✅ anthropic_messages 格式（v0.39 前后） | ✅ | ✅ (原生) | ❌ |
| **Google Gemini** | ❌ | ✅ | ❌ | ❌ |
| **Grok/xAI** | ❌ | ✅ | ❌ | ❌ |

> **thin_agent 评价**: 3 层降级链（Template 0MB → Qwen 395MB → Gemma 769MB → Cloud）是独特的边缘计算设计。但 Provider 生态是最大工程欠账——3 vs Hermes 21+。`openai-compatible` 可覆盖一部分，但 Anthropic Messages API 等非 OpenAI 格式后端需要专用适配。

---

## 三、工具系统

| 维度 | thin_agent | Hermes Agent | Claude Code | LangChain | CrewAI |
|------|-----------|-------------|-------------|-----------|--------|
| **Function Calling** | ✅ AgentLoop FC | ✅ 原生 FC | ✅ | ✅ | ✅ |
| **MCP Server** | ✅ stdio JSON-RPC (v0.34) | ✅ hermes mcp serve | ❌ (Consumer) | ✅ | ❌ |
| **MCP Client** | ✅ McpClient (v0.29；v0.53.7 市面全量可接：args[]/env{} 标准格式+真 e2e npx filesystem 实证) | ✅ hermes mcp add | ✅ | ✅ | ❌ |
| **工具注册** | ✅ SkillRegistry (C++) | ✅ registry.py | ✅ MCP tools | ✅ @tool | ✅ @tool |
| **危险工具审批 (HITL)** | ✅ 审批按钮 | ✅ approvals.mode | ✅ permission | ❌ | ✅ |
| **True Sandbox** | ✅ clone+ns+cgroups (v0.36) | ✅ Docker/Modal | ✅ 子进程 | ❌ | ❌ |
| **PTY 交互** | ✅ (v0.30) | ✅ | ✅ | ❌ | ❌ |
| **后台进程管理** | ✅ poll/wait/kill/log (v0.30) | ✅ | ✅ | ❌ | ❌ |
| **浏览器工具** | ❌ | ✅ CDP/Browserbase | ❌ | ✅ | ❌ |
| **文件工具** | ✅ code_read/write/patch/search | ✅ | ✅ | ✅ | ✅ |
| **Web 搜索** | ✅ Bing 真源+FC 工具+意图层双层（v0.53.10/11，运行时可切源） | ✅ web_search + browser | ✅ | ✅ | ✅ |
| **Vision/Image** | ✅ 多模态消息体（v0.39.0，cfg.vision） | ✅ vision_analyze | ✅ 截图分析 | ❌ | ❌ |
| **TTS/Voice** | ❌ | ✅ 6 provider | ❌ | ❌ | ❌ |
| **Security Redaction** | ✅ API Key+PII (v0.33) | ✅ redact_secrets | ❌ | ❌ | ❌ |
| **KB 全文搜索** | ✅ C++ FTS5 (164K 文档, 41ms) | ❌ | ❌ | ❌ | ❌ |
| **任务引擎** | ✅ SQLite TaskEngine | ✅ Cron job | ❌ | ❌ | ❌ |
| **插件系统** | ✅ .so dlopen (4 插件) | ✅ Python 插件 | ❌ | ✅ | ❌ |
| **Patch 编辑** | ⚠️ 精确字符串替换 | ✅ 9 种模糊策略 | ✅ | ✅ | ❌ |
| **终端执行** | shell_exec（dev:黑名单 / sandbox:clone+ns） | terminal（完整 shell） | ✅ | ✅ | ❌ |

> **thin_agent 评价**: v0.30~v0.36 补齐了 PTY、后台进程、True Sandbox、Security Redaction、MCP Server/Client 6 项关键工具差距。v0.53.10/11 补齐 Web 搜索（Bing 真源、FC 工具自主调用、运行时切源）。剩余差距集中在浏览器、TTS 等外延能力（Vision 消息体已有，截图→分析链路待接）。KB 全文搜索、设备控制、ONNX 推理、SKILL.md 生态、MCP client 为 thin_agent 优势项。

---

## 四、Memory & RAG

| 维度 | thin_agent | Hermes Agent | Claude Code | LangChain | CrewAI |
|------|-----------|-------------|-------------|-----------|--------|
| **短期记忆** | ✅ session_chat_memory_ | ✅ 会话窗口 | ✅ 会话窗口 | ✅ | ✅ |
| **长期记忆** | ✅ JSONL + SQLite (SessionStore) | ✅ state.db FTS5 | ❌ | ✅ VectorStore | ✅ |
| **语义搜索** | ⚠️ FTS5（无 embedding） | ✅ FTS5 + embedding | ❌ | ✅ | ✅ |
| **Session Search** | ✅ FTS5 全文检索（v0.39 前后，2026-08-27 真网实测通过） | ✅ FTS5 全文检索 | ❌ | ❌ | ❌ |
| **自动记忆抽取** | ⚠️ FactStore 热快照注入（角色域） | ✅ MemoryManager | ❌ | ❌ | ❌ |
| **跨会话记忆注入** | ✅ hot_snapshot 注入 system prompt（v0.27.5） | ✅ 自动注入 system prompt | ❌ | ✅ | ✅ |
| **上下文压缩** | ✅ 摘要压缩 (阈值触发) | ✅ 摘要压缩 | ✅ | ✅ | ✅ |

> **thin_agent 评价**: thin_agent 有基础的 SessionStore + FactStore，但缺失 Hermes 级别的语义记忆和跨会话自动注入。Session Search（FTS5 全文检索历史）是最大短板——用户无法跨会话召回上下文。

---

## 五、部署 & 平台

| 维度 | thin_agent | Hermes Agent | Claude Code | 其他 |
|------|-----------|-------------|-------------|------|
| **WebSocket 服务** | ✅ thin_agent WS | ✅ API Server | ❌ | ✅ |
| **HTTP API** | ✅ (via WS JSON) | ✅ REST API | ❌ | ✅ |
| **CLI** | ✅ thin_agent_cli -q (v0.29) | ✅ 完整 CLI | ✅ | ✅ |
| **飞书** | ✅ IM Gateway | ✅ | ❌ | 少数 |
| **微信** | ✅ (网关架构) | ✅ (Weixin adapter) | ❌ | ❌ |
| **Telegram** | ✅ (v0.32) | ✅ | ❌ | 少数 |
| **Discord** | ✅ (v0.35) | ✅ | ❌ | 少数 |
| **Slack/WhatsApp/Signal 等** | ❌ (差 13 平台) | ✅ 17+ 平台 | ❌ | ❌ |
| **Docker** | ✅ 轻量 | ✅ | ❌ | ✅ |
| **嵌入式 MCU** | ✅ 设计目标 | ❌ | ❌ | ❌ |
| **IDE 集成** | ❌ | ✅ VSCode/JetBrains (ACP) | ✅ 原生 | ✅ |
| **Cron 调度** | ✅ CronScheduler (v0.29) | ✅ cronjob | ❌ | ✅ |
| **Webhook** | ✅ WebhookClient+HMAC 投递（deliver_to=webhook:） | ✅ webhook subscriptions | ❌ | ✅ |

> **thin_agent 评价**: v0.32~v0.35 补齐 Telegram + Discord，平台从 2→4。CLI 和 Cron 调度已对齐。仍缺 13 个平台适配器和 Webhook/IDE 集成。

---

## 六、可观测性

| 维度 | thin_agent | Hermes Agent | Claude Code | LangSmith |
|------|-----------|-------------|-------------|-----------|
| **链路追踪** | ✅ AgentTracer SQLite | ✅ state.db | ❌ | ✅ LangSmith |
| **Tracing UI** | ✅ ws_agent.html 按钮 | ✅ /debug | ❌ | ✅ 完整 UI |
| **决策审计** | ✅ decision_audit jsonl | ❌ | ❌ | ✅ |
| **指标收集** | ✅ metrics + latency | ✅ /insights | ❌ | ✅ |
| **日志** | RotatingLogBuf (20MB/10MB) | ✅ 结构化日志 | ❌ | ✅ |
| **分布式追踪** | ❌ | ❌ | ❌ | ✅ |

> **thin_agent 评价**: Tracing 对嵌入式场景出人意料地完整。SQLite 持久化 + Web UI 可用。日志系统有 RotatingLogBuf 自动轮转。

---

## 七、自主性

| 维度 | thin_agent | Hermes Agent | Claude Code | AutoGPT |
|------|-----------|-------------|-------------|---------|
| **Skill 系统** | ✅ .so 插件 (4 个) | ✅ YAML (30+) + 自进化 | ❌ | ❌ |
| **Skill 自进化** | ✅ v0.38.1 复杂任务后自动提取 | ✅ 从经验学习 | ❌ | ❌ |
| **Curator** | ✅ SkillManager.auto_maintain（stale→归档→清理） | ✅ 技能生命周期管理 | ❌ | ❌ |
| **自适应路由** | ✅ 复杂度感知选模型 | ✅ credential pools | ❌ | ❌ |
| **跨会话学习** | ✅ ErrorCorrectionStore | ✅ state.db + session_search | ❌ | ❌ |
| **自我纠错** | ✅ 规则+LLM双重检测 | ✅ 迭代修复 | ✅ | ✅ |
| **长期目标** | ✅ GoalManager SQLite | ✅ /goal 机制 | ❌ | ✅ |
| **主动监控** | ✅ 文件+HTTP 告警 | ✅ cron+webhook | ❌ | ❌ |
| **子Agent生成** | ✅ spawn_agent 隔离推理 | ✅ delegate_task | ❌ | ❌ |
| **子任务委托** | ✅ DAG 拓扑排序+并行 | ✅ delegate_task (parallel leaf) | ❌ | ❌ |
| **检查点回滚** | ✅ 快照+恢复 | ✅ /rollback | ❌ | ❌ |
| **Webhook** | ✅ 事件驱动触发（v0.44.0 投递） | ✅ 事件驱动触发 | ❌ | ❌ |
| **Profiles** | ✅ v0.43.0 多实例隔离 | ✅ 多实例隔离 | ❌ | ❌ |

> **thin_agent 评价**: v0.36.2 自主性保持在 ⭐⭐⭐⭐。与 Hermes 的核心差距已从 6 项缩小到 4 项：Skill 自进化、Curator、Webhook、Profiles。加上 Memory 维度的 Session Search 和语义记忆，共 5 项关键差距。

---

## 📊 总体评分

| 维度 | thin_agent | Hermes | Claude Code | LangGraph | OpenAI SDK | CrewAI |
|------|:---------:|:------:|:-----------:|:---------:|:----------:|:------:|
| 轻量高性能 | ⭐⭐⭐⭐⭐ | ⭐⭐ | ⭐⭐ | ⭐ | ⭐ | ⭐ |
| 本地推理 | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ | ⭐ | ⭐⭐ | ⭐ | ⭐⭐ |
| 多 Agent | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐ | ⭐⭐⭐⭐ | ⭐ | ⭐⭐⭐⭐⭐ |
| 工具生态 | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐ |
| Memory | ⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐ | ⭐⭐⭐ |
| IM 平台 | ⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐ | ⭐ | ⭐ | ⭐ |
| 可观测性 | ⭐⭐⭐ | ⭐⭐⭐ | ⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐ | ⭐⭐ |
| **自主性** | ⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐ | ⭐⭐ | ⭐ | ⭐⭐⭐ |
| **综合定位** | **全场景轻量王者** | **通用最强** | **编码最强** | **编排最强** | **OpenAI 首选** | **多 Agent 首选** |

> **评分变化（vs v0.27.3）**:
> - 工具生态: ⭐⭐ → ⭐⭐⭐（+1 — v0.30~v0.36 补齐 PTY/BG/Sandbox/Security/MCP/CLI 共 7 项）
> - IM 平台: ⭐ → ⭐⭐（+1 — v0.32/v0.35 补齐 Telegram + Discord）
> - 自主性: 保持 ⭐⭐⭐⭐（v0.36.2 未涉及自进化/Curator/Webhook/Profiles）

---

## 🎯 thin_agent 的独特优势

1. **C++17 全场景部署** — 6.1MB 二进制，从嵌入式 MCU 到云端服务器一套代码
2. **3 层降级推理链** — Template → Qwen → Gemma → Cloud，边缘到云端全覆盖
3. **ONNX 意图分类** — 3.3KB 模型，12 类意图分类
4. **KB 全文搜索** — C++ FTS5 引擎，164K 文档 41ms 响应，插件热写
5. **True Sandbox** — clone+namespaces+cgroups，Linux/macOS/Windows 三平台
6. **Security Redaction** — API Key 红标 + 危险命令硬阻断
7. **多 Agent 满分** — ⭐⭐⭐⭐⭐，9 角色 + DAG + 辩论 + 看板 + 黑板
8. **飞书/微信原生集成** — IM 网关架构，双向 WebSocket 透传
9. **自动化测试覆盖** — 32 组测试 100% 通过
10. **MCP 双向支持** — Server (stdio) + Client，兼容 Claude Desktop

---

## ⚠️ 关键差距（按优先级）

### 🔴 硬差距（2026-08-29 对账：15→5→4 项）

> **对账方法**：逐项代码核实（实现+调用点）+真网验证（session_search 经 chat 通道实测返回 FTS5 结果）。
> **已实现但表格长期漏记的 10 项**：Session Search（FTS5）/ Webhook（WebhookClient 318 行 HMAC）/ Curator（SkillManager.auto_maintain）/ Skill 自进化（v0.38.1）/ Vision 多模态（v0.39.0）/ Anthropic（anthropic_messages 全链）/ Credential Pool（v0.43.0 轮转）/ Kanban（push/status handler）/ Delegation 增强（v0.52.20-24 嵌套+委托+双向回流）/ **Profiles（v0.43.0——~/.thin_agent/profiles/ 下 7 实例目录实证，data/config 隔离）**。

| # | 差距项 | thin_agent 现状 | 业界最佳 | 差距程度 | 优先级 |
|---|--------|-----------------|----------|:--:|:--:|
> **v0.52.29 更新**：Hook System 已补齐（8 事件+C++/shell 双源+tool_pre 否决）——差距 5→4 项，全部为外延生态类。

| 1 | **Browser** | ❌ 零 | Hermes CDP/Browserbase/Camofox | 🔴大 | P2 |
| 2 | **Voice/TTS/STT** | ❌ 零 | Hermes 6 provider | 🟡中 | P2 |
| 3 | **Provider 补齐** | 4（openai-compat/anthropic/GLM/本地） vs 21+ | Google/Grok/Kimi 等 | 🟡中 | P3 |
| 4 | **平台补齐** | 4（飞书/微信/TG/Discord） vs 17+ | Slack/WhatsApp/Signal 等 13 个 | 🟡中 | P3 |

---

### 🟢 不可撼动的护城河（业界追不上）

| 能力 | thin_agent | 业界最佳 | 差距 |
|------|:---:|:---:|------|
| C++ 全场景部署 | ✅ 6.1MB（嵌入式→服务器→桌面） | ❌ | **独占** |
| 交叉编译到 MCU | ✅ | ❌ | **独占** |
| 3 层本地推理降级 | ✅ 0→395→769MB | ❌ | **独占** |
| ONNX 意图分类 | ✅ 3.3KB | ❌ | **独占** |
| KB 全文搜索 | ✅ C++ FTS5 164K 文档 41ms | ❌ | **独占** |
| True Sandbox (C++ native) | ✅ clone+ns+cgroups 跨平台 | 部分（Docker/Python） | **独占**（C++ 原生） |
| 飞书/微信原生 | ✅ 网关架构 | ❌ 非中国市场 | **独占** |
| 启动时间 | <100ms | 1-8s | **10-80x** |
| 内存占用 | ~50MB | 200MB-1GB | **4-20x** |

### 🎯 优先级路线图

| 优先级 | 差距项 | 理由 | 预估 |
|:--:|--------|------|:--:|
| **P0** | Session Search (#1) | 无法跨会话检索——用户重复交代 | 2-3d |
| **P0** | Vision/Image (#2) | 无法看图/调试截图——阻碍远程设备调试 | 2-3d |
| **P1** | Persistent Memory (#3) | FactStore→语义记忆+自动注入 | 3-5d |
| **P1** | Webhook (#4) | 从定时到事件驱动 | 1-2d |
| **P1** | Delegation 增强 (#5) | 单层→嵌套+批处理 | 3-5d |
| **P1** | Hook System (#6) | 工具生命周期钩子 | 3-5d |
| **P2** | Skill 自进化 (#7) | 从经验中学习创建 skill | 5-7d |
| **P2** | Curator (#8) | 技能生命周期管理 | 2-3d |
| **P2** | Profiles (#9) | 多实例隔离 | 2-3d |
| **P2** | Browser (#10) | CDP 浏览器自动化 | 3-5d |
| **P3** | Voice/TTS (#11) | 语音交互 | 3-5d |
| **P3** | Credential Pool (#12) | Key 轮转+OAuth | 2-3d |
| **P3** | Provider 补齐 (#13) | Anthropic/Google/Grok 等 | 5-7d |
| **P3** | 平台补齐 (#14) | Slack/WhatsApp/Signal 等 | 7-10d |
| **P3** | Kanban 集成 (#15) | dispatcher+worker+claim | 3-5d |

---

## 📝 版本演进追踪

| thin_agent 版本 | 日期 | 完成度 | 对标排名变化 | 备注 |
|----------------|------|--------|-------------|------|
| **v0.52.24** | 2026-08-27 | 差距表归真：15→6 项 | 质量线大收官 | 全量实测对账：Session Search/Webhook/Curator/Skill自进化/Vision(多模态)/Anthropic/Credential轮转/Kanban/Delegation 均已实现（表格长期漏记）；v0.52.10-24 质量线：进程组收割/线程雪崩/假绿/崩溃守卫/零工具割裂/WS误踢/嵌套双闸/令牌桶/协作取消/注册表双向 |
| **v0.36.2** | 2026-07-27 | 工具生态 ⭐⭐→⭐⭐⭐ / 平台 ⭐→⭐⭐ | 补齐 7 项 P0 差距 | v0.30~v0.36: PTY+BG/Sandbox/Security/MCP/CLI/Cron/Tele+Discord；识别 15 项剩余差距 |
| v0.27.3 | 2026-07-20 | 18/27 | 定位拓宽：全场景轻量 Agent 平台 | 插件系统 (✅) + 测试扩展 + KB 系统上线 |
| v0.11.7 | 2026-07-12 | 17/27 | 多 Agent ⭐⭐⭐⭐→⭐⭐⭐⭐⭐ | Agent协商辩论：多角色多轮debate→黑板共享→@mention→共识收敛 |
| v0.11.6 | 2026-07-12 | — | 多 Agent ⭐⭐⭐⭐ | Kanban看板：任务池+Worker自主抢单+状态追踪 |
| v0.11.0 | 2026-07-12 | — | 多 Agent ⭐⭐⭐→⭐⭐⭐⭐ | AgentRole + 5 内置角色 + 工具白名单 + 角色化 spawn_agent |
| v0.10.5 | 2026-07-12 | 16/16 补齐 ✅ | 自主性 ⭐⭐⭐⭐⭐ | +6项自主性能力：技能生命周期/自适应路由/跨会话学习/目标驱动/主动监控/子Agent |
| v0.9.0 | 2026-07-12 | 6/6 差距全部补齐 ✅ | 综合评分 +4 | Summarizer+Cron+Checkpoint+SkillManager+MCP+Orchestrator |
| v0.8.53 | 2026-07-06 | P0✅ P1✅ P2❌ | 嵌入式赛道 No.1 | 初始对比基准 |
