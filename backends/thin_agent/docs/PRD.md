# thin_agent 跨平台多端架构 PRD

> 版本: v1.0-draft | 日期: 2026-07 | 状态: 待评审

---

## 1. 产品愿景

将 thin_agent 从一个「单机 C++ 可执行文件」升级为**跨平台、多端、前后端分离的 Agent 服务框架**。

### 1.1 平台覆盖矩阵

**后端和前端各自独立覆盖全部 5 个平台**，任意组合均可工作：

| 后端 ↓ \ 前端 → | Windows | Linux | macOS | Android | iOS |
|:---:|:---:|:---:|:---:|:---:|:---:|
| **Windows** | ✅ 本地 | ✅ 远程 | ✅ 远程 | ✅ 远程 | ✅ 远程 |
| **Linux** | ✅ 远程 | ✅ 本地 | ✅ 远程 | ✅ 远程 | ✅ 远程 |
| **macOS** | ✅ 远程 | ✅ 远程 | ✅ 本地 | ✅ 远程 | ✅ 远程 |
| **Android** | — | — | — | ✅ 本地* | — |
| **iOS** | — | — | — | — | ✅ 本地* |

> \* 手机端「本地」= App 内 `loadLibrary()` 直接跑 agent .so/framework，不依赖网络。
> 「远程」= 前端通过 WS 连接另一台设备的 agent 后端。
> 手机作为后端服务其他前端理论上可行但不推荐（功耗/网络限制），优先用于本地模式。

**典型组合：**

| 后端在哪 | 前端在哪 | 场景 |
|----------|---------|------|
| Linux 服务器 | Windows 桌面客户端 | 主力开发机 + GPU 服务器推理 |
| macOS 笔记本 | iPhone App | 出门在外，连回家里的 Mac |
| WSL (Windows) | 同一台 Windows 的 CLI | 本地开发调试 |
| Android 手机 | 同一台 Android App | 移动离线，本地小模型 |
| Linux 服务器 | 3 个前端同时连 | 团队共享同一个 Agent 后端 |

### 1.2 核心原则

- **前后端通过 WS 协议通信** — 唯一通道，无共享内存/直接函数调用
- **前端只管展示** — 不含 Agent 逻辑，纯 WS 客户端
- **后端可服务多个前端** — WS 天然多连接，每连接独立 session
- **前端可切换多个后端** — 断开 → 组播发现或手动 IP → 新连接
- **零配置发现**：局域网内组播自动发现 agent 服务

---

## 2. 用户场景

| 场景 | 描述 |
|------|------|
| **桌面日常** | 开发机上跑 `thin_agentd` 后台服务，用 CLI 或桌面客户端连接 |
| **远程协作** | 笔记本前端连接机房服务器的 agent 后端，利用服务器 GPU 推理 |
| **移动离线** | 手机 App 内置本地 agent（小模型），无网也能对话 |
| **移动联网** | 手机 App 切换为远程模式，连回家里的 agent 服务器 |
| **多前端协作** | 同一后端同时服务桌面客户端 + 手机 App + CLI，共享会话上下文 |
| **后端切换** | 前端下拉选择"家里的服务器" / "公司的开发机" / "本机" |

---

## 3. 功能需求

### 3.1 Agent 后端（thin_agentd）

| P | 需求 | 说明 |
|:--:|------|------|
| P0 | 后台守护进程 | systemd (Linux) / launchd (macOS) / Windows Service |
| P0 | WS 服务 | 端口可配，默认 8765，现有 JSON 协议不变 |
| P0 | 组播发现 | mDNS 或 UDP 组播响应，宣告 IP + 端口 + 设备信息 |
| P1 | 多 Profile | 同一后端支持多个 profile 热切换 |
| P1 | 多前端并发 | 每个 WS 连接独立 session，互不干扰 |
| P2 | 资源监控 | CPU/内存/模型加载状态可查询 |
| P2 | 优雅启停 | SIGTERM 保存状态后退出，systemd notify |

### 3.2 Agent 核心库（libthin_agent_core）

| P | 需求 | 说明 |
|:--:|------|------|
| P0 | 动态库 | Linux `.so`，macOS `.dylib`，Windows `.dll`，Android `.so`，iOS `.framework` |
| P0 | C ABI | `extern "C"` 稳定接口，init/start/stop/destroy/version |
| P0 | 编译兼容 | 支持 GCC/Clang/MSVC/Android NDK |
| P1 | 体积控制 | 移动端裁剪选项（关闭 llama.cpp / ONNX 大模型） |

### 3.3 前端（Desktop）

| P | 需求 | 说明 |
|:--:|------|------|
| P0 | CLI 前端 | `thin_agent_cli`，终端交互，支持管道/重定向 |
| P1 | Web 前端 | 复用现有 `ws_agent.html`（1426 行），可直接浏览器打开 |
| P2 | 桌面客户端 | Electron / Tauri 或 Qt，系统托盘常驻 |

### 3.4 前端（Mobile）

| P | 需求 | 说明 |
|:--:|------|------|
| P0 | Android App | Kotlin/Swift 原生壳 + WebView 加载 WS 前端 |
| P1 | iOS App | 同上 |
| P1 | 本地模式 | App 内加载 .so/.framework，不依赖网络 |
| P1 | 远程模式 | 连接远程 agent 后端，手动输入 IP 或组播发现 |
| P2 | iPadOS 适配 | 分屏、台前调度、键盘快捷键 |

### 3.5 组播发现

| P | 需求 | 说明 |
|:--:|------|------|
| P0 | 后端响应组播 | 监听 UDP 组播端口，收到 probe 后回复设备信息 JSON |
| P0 | 前端发现 | 发送组播 probe → 收集回复列表 → 用户选择 → WS 连接 |
| P1 | 安全 | 组播回复可加 shared secret 签名防伪造 |
| P2 | mDNS 兼容 | 注册 `_thin-agent._tcp` 服务类型，兼容 Bonjour/Avahi |

### 3.6 网关库（libthin_agent_gateway）

| P | 需求 | 说明 |
|:--:|------|------|
| P0 | 动态库化 | 抽取 `im_gateway_main.cpp`（790 行）为独立 .so |
| P1 | 多平台 IM | 飞书长连接 + 微信 iLink，跨平台可编译 |
| P2 | 独立部署 | 网关可独立于 agent 部署在不同机器 |

---

## 4. 非功能需求

- **二进制体积**：移动端 < 15MB（裁剪后），桌面端 < 30MB
- **内存占用**：空闲 < 50MB，活跃 < 500MB（不含模型权重）
- **延迟**：组播发现 < 3s，WS 连接 < 1s
- **兼容性**：Windows 10+，macOS 13+，Android 8+，iOS 15+，Linux kernel 5.4+

---

## 5. 实施计划（18 步 / 4 Phase）

### Phase 1：核心库化（基礎，所有平台依赖）

| 步 | 内容 | 产出 | 预估 |
|:--:|------|------|:--:|
| 1.1 | 定义 C ABI 接口 | `agent_api.h` / `gateway_api.h` / `discovery.h` | 0.5d |
| 1.2 | 实现 C ABI 包装层 | `agent_api.cpp`: 不透明指针包裹 AgentService | 1d |
| 1.3 | CMake STATIC → SHARED | `libthin_agent_core.{so,dll,dylib}` + visibility 控制 | 0.5d |
| 1.4 | 抽取网关为 SHARED | `libthin_agent_gateway.{so,dll,dylib}` | 1d |
| 1.5 | 实现组播发现 | `discovery.cpp`: UDP multicast probe/response | 1d |
| 1.6 | thin_agentd 守护进程 | dlopen 加载库 + 启 WS + 启组播 + 信号处理 | 1d |
| 1.7 | 回归测试 | 编译 0e0w、13/13 全绿、现有功能不受影响 | 0.5d |

> Phase 1 做完 = 最小可用闭环：`thin_agentd` 后台 + `thin_agent_cli` 前端通过 WS 通信。

### Phase 2：PC 前端（1-2 周）

| 步 | 内容 | 产出 |
|:--:|------|------|
| 2.1 | thin_agent_cli | WS 客户端 + 组播发现 + `/connect /discover /backends` 命令 |
| 2.2 | ws_agent.html 增强 | 连接面板（手动 IP / 扫描局域网）+ 后端切换 + 自动重连 |
| 2.3 | PC 端服务化 | Linux systemd unit、macOS launchd plist、Windows Service |

### Phase 3：移动端（2-3 周）

| 步 | 内容 | 产出 |
|:--:|------|------|
| 3.1 | Android NDK 编译 | arm64 `libthin_agent_core.so`（裁剪版，无 llama.cpp） |
| 3.2 | JNI 胶水层 | Java/Kotlin ↔ native C ABI 桥接 |
| 3.3 | Android App 壳 | WebView + 本地/远程双模式切换 |
| 3.4 | iOS framework 编译 | `ThinAgentCore.framework` (arm64) |
| 3.5 | iOS App 壳 | Swift + WKWebView + 本地/远程双模式 |

### Phase 4：收尾（2 周）

| 步 | 内容 | 产出 |
|:--:|------|------|
| 4.1 | Windows DLL + MSVC | VS cmake toolchain 适配 |
| 4.2 | CI/CD 多平台矩阵 | GitHub Actions / Jenkins 自动构建 |
| 4.3 | 安装包 | .deb / .pkg / .msi / .apk / .ipa |
| 4.4 | 文档 | 部署指南 + 用户手册

---

## 6. 术语表

| 术语 | 定义 |
|------|------|
| **Agent 后端** | 运行 `thin_agentd` 的进程/机器，加载 `libthin_agent_core.so` |
| **前端** | CLI / 桌面 GUI / 移动 App，通过 WS 连接后端 |
| **组播发现** | UDP multicast probe-response 协议，局域网零配置发现 |
| **Profile** | Agent 的配置上下文（模型/密钥/工具集），可热切换 |
| **Session** | 一个前端 ↔ 后端的 WS 连接，含独立对话上下文 |

---

## 附录：v0.8 功能规格参考

> 来源：`PRD.md`（v0.8.13，早期功能 PRD）。本章节保留详细功能定义，供后续实现参考。

### 1. Agent 主体能力模型

1. **感知（Perception）** — 采集设备状态（health/status）、日志与运行指标、接收事件输入
2. **决策（Decision）** — 规则层 + 轻量语义路由层 + 策略层，分层智能体决策引擎
3. **执行（Action）** — 通过 ActionExecutor 与策略门控，设备能力通过 IDeviceControl 抽象
4. **记忆（Memory）** — 短期记忆（session memory）+ 长期记忆（SQLite/audit/task history）
5. **可观测（Observability）** — 运行指标查询（request 总量、按类型计数、平均耗时）

### 2. 技术栈基线（强约束）

- **C++17（必须）**
- **Mongoose（必须）**：WebSocket/HTTP 服务
- **nlohmann::json（必须）**：JSON 序列化
- **CMake（必须）**：统一构建
- Mongoose 来源：`~/code/leaptic_app/mobile/src/mongoose.c/.h`
- 不允许 Python 作为主服务实现路径

### 3. 消息协议参考

| type | 方向 | 说明 |
|------|:--:|------|
| `chat` | C→S | 对话消息 |
| `chat_chunk` | S→C | 流式 token |
| `chat_result` | S→C | 最终回复（含 decision_trace） |
| `status` | C→S | 状态查询 |
| `action` | C→S | 本地动作执行 |
| `task_submit` / `task_get` / `task_list` | C→S | 任务编排 |
| `spawn_agent` / `agent_message` | C→S | 子 Agent 间通信 |
| `kanban_*` | C→S | Kanban 看板 |
| `agent_debate` | C→S | 多 Agent 协商辩论 |

### 4. 模型策略规则

| 模式 | 说明 |
|------|------|
| `offline` | 禁用外部模型，仅规则引擎 + 本地策略 |
| `cloud` | 强制云模型调用，网络不可用时按 fallback 处理 |
| `auto` | 优先 cloud，失败降级 offline |
| Provider | github-copilot / deepseek / openai-compatible |

### 5. 端云协同策略闭环

1. **本地主控（Controller）** — 本地负责最终路由与最终回复生成
2. **云端顾问（Strategist）** — 返回结构化策略 JSON（strategy/intent/confidence/risk/local_route_hint/response_draft）
3. **本地二次裁决（Policy）** — 根据风险与路由 hint 选择 execute/clarify/fallback_cloud/reject
4. **非照搬约束** — 最终用户可见文本必须由本地输出

### 6. 端云协同五层决策漏斗（v0.16）

```
第 0 层 — 本地意图秒回 (0ms)
  关键词命中本地 handler → 模板回复

第 1 层 — 离线降级 (0ms)
  无网络 / 缺 API key → 规则引擎兜底

第 2 层 — auto 先遣 (0-300ms)
  HybridRouter 级联（Template → Qwen 0.5B → Gemma 1B）

第 3 层 — Flash 快通道 (2-5s)
  简单文件操作（list/read/write/search）→ Flash FC
  非简体中文/非英文输入自动翻译后匹配

第 4 层 — Pro 深度推理 (5-8s)
  Native Function Calling + streaming
  LLM 直接调用工具（read_file/write_file/list_dir/search_code）
```

- **Tier 1 本地**：TemplateModel (0ms) → ONNX intent (50ms) → Qwen 0.5B (300ms) → Gemma 1B (800ms)
- **Tier 2 云端意图分类**：Flash 模型返回结构化 intent JSON，命中本地 handler 则回到本地模板回复
- **Flash 快通道**（v0.16 新增）：Tier 2 与 Tier 3 之间，简单文件操作用 Flash FC 加速
- **Tier 3 云端 AgentLoop**：Pro 模型完整 ReAct 循环，工具调用、多步推理
- **输入翻译**（v0.16 新增）：非简体中文/非英文 → 先翻译成英文再匹配关键词

### 7. 离线/在线双模式

- **离线模式**：规则/状态机自治，不依赖外部 LLM，可作为"窄域完整 agent"
- **在线模式**：调用云端大模型 API，LLM 输出仅作为建议，最终执行走 Policy Gate
- 任何模式下，危险动作都必须通过 Policy Gate
