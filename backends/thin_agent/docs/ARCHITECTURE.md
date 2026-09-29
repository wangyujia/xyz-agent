# thin_agent 架构方案

> 版本: v1.0-draft | 日期: 2026-07 | 状态: 待评审

---

## 1. 总体架构

```text
                    ┌──── WS Protocol ────┐
                    │  (JSON over TCP)     │
                    │                      │
   Frontend          │                      │          Backend
   (任意平台)        │                      │          (任意平台)
                    │                      │
┌──────────────┐    │    ┌──────────────────────────────┐
│ Windows      │◄───┼───►│ thin_agentd (Windows)         │
│  - CLI       │    │    │  → thin_agent_core.dll       │
│  - Desktop   │    │    ├──────────────────────────────┤
│  - Web       │    │    │ thin_agentd (Linux)           │
├──────────────┤    │    │  → libthin_agent_core.so     │
│ Linux        │◄───┼───►├──────────────────────────────┤
│  - CLI       │    │    │ thin_agentd (macOS)           │
│  - Desktop   │    │    │  → libthin_agent_core.dylib  │
│  - Web       │    │    ├──────────────────────────────┤
├──────────────┤    │    │ thin_agentd (WSL)             │
│ macOS        │◄───┼───►│  → libthin_agent_core.so     │
│  - CLI       │    │    ├──────────────────────────────┤
│  - Desktop   │    │    │ Android App (本地模式)        │
│  - Web       │    │    │  → libthin_agent_core.so     │
├──────────────┤    │    ├──────────────────────────────┤
│ Android      │◄───┼───►│ iOS App (本地模式)            │
│  - App (本地)│    │    │  → ThinAgentCore.framework   │
│  - App (远程)│    │    └──────────────────────────────┘
├──────────────┤    │
│ iOS          │    │    ── Multicast Discovery ──
│  - App (本地)│    │    Client: "Anyone running thin_agent?"
│  - App (远程)│    │    Server: "192.168.1.100:8765 | dev-pc | Linux"
└──────────────┘    │

  5 平台前端 × 5 平台后端 = 25 种组合，任意搭配
```

### 核心原则

1. **前后端通过 WS 协议通信** — 这是唯一通道，无共享内存/直接调用
2. **Agent 核心是动态库** — `.so` / `.dll` / `.dylib`，被守护进程加载
3. **前端只管展示** — 不含 Agent 逻辑，纯 WS 客户端
4. **一个后端可接多个前端** — WS 天然多连接，每个连接独立 session
5. **一个前端可切换多个后端** — 断开 → 组播发现或手动输入 IP → 新连接

---

## 2. 组件分层

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                     │
│  thin_agentd    thin_agent_cli    Mobile App             │
├─────────────────────────────────────────────────────────┤
│                    Interface Layer                       │
│  C ABI (agent_api.h / gateway_api.h)                    │
├─────────────────────────────────────────────────────────┤
│                    Library Layer                         │
│  libthin_agent_core    libthin_agent_gateway             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐              │
│  │  core/   │  │  agent/  │  │  llm/    │              │
│  │  local/  │  │  fdbus/  │  │  gateway │              │
│  └──────────┘  └──────────┘  └──────────┘              │
├─────────────────────────────────────────────────────────┤
│                    Platform Layer                        │
│  Mongoose    SQLite    curl    ONNX Runtime    OpenSSL   │
└─────────────────────────────────────────────────────────┘
```

---

## 3. 动态库拆分

### 3.1 当前 vs 目标

```
当前:                              目标:
┌──────────────────────┐          ┌──────────────────────────┐
│ thin_agent_core.a    │          │ libthin_agent_core.so    │
│ (STATIC, 所有代码)   │   →      │ libthin_agent_core.dll   │
│                      │          │ libthin_agent_core.dylib │
│ thin_agent (WS入口)  │          │ ThinAgentCore.framework  │
│ thin_agent_gw (IM)   │          ├──────────────────────────┤
│ thin_agent_demo(CLI) │          │ libthin_agent_gateway.so │
└──────────────────────┘          │ (独立的 IM 网关库)       │
                                  └──────────────────────────┘
```

### 3.2 库边界

| 库 | 包含模块 | 依赖 | 体积估算 |
|----|---------|------|---------|
| `libthin_agent_core` | core/, agent/, llm/, local/, fdbus/ | sqlite3, curl, onnxruntime | ~6MB |
| `libthin_agent_gateway` | IM 协议（飞书/微信） | mongoose, openssl, curl | ~1MB |
| 移动端裁剪版 | 去掉 llama.cpp, ONNX 本地推理 | sqlite3, curl | ~3MB |

### 3.3 C ABI 接口设计

```c
// include/thin_agent/api/agent_api.h
#ifndef THIN_AGENT_API_H
#define THIN_AGENT_API_H

#ifdef __cplusplus
extern "C" {
#endif

// ── 导出宏 ──
#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef THIN_AGENT_BUILDING_DLL
    #define THIN_API __declspec(dllexport)
  #else
    #define THIN_API __declspec(dllimport)
  #endif
#else
  #define THIN_API __attribute__((visibility("default")))
#endif

// ── 不透明句柄 ──
typedef struct thin_agent_t thin_agent_t;
typedef struct thin_agent_config_t thin_agent_config_t;

// ── 配置 ──
typedef struct {
    const char* config_yaml;          // demo.model.yaml 路径
    const char* profile;              // default / zai_main_demo
    int         ws_port;              // WebSocket 端口 (默认 8765)
    const char* ws_bind_addr;         // 绑定地址 (默认 "127.0.0.1")
    int         multicast_port;       // 组播端口 (默认 9876, 0=禁用)
    const char* multicast_group;      // 组播地址 (默认 "224.0.0.199")
    const char* data_dir;             // 数据目录 (默认 ~/.thin_agent)
    int         log_level;            // 0=quiet, 1=info, 2=debug
} thin_agent_config_t;

// ── 生命周期 ──
THIN_API thin_agent_t* thin_agent_create(const thin_agent_config_t* config);
THIN_API int            thin_agent_start(thin_agent_t* agent);
THIN_API int            thin_agent_stop(thin_agent_t* agent);
THIN_API void           thin_agent_destroy(thin_agent_t* agent);

// ── 查询 ──
THIN_API const char*    thin_agent_version(void);
THIN_API int            thin_agent_is_running(thin_agent_t* agent);
THIN_API const char*    thin_agent_get_info_json(thin_agent_t* agent);
// 返回: {"hostname":"dev-pc","os":"Linux 6.8","ip":"192.168.1.100",
//        "ws_port":8765,"version":"v0.12.0","profiles":["default"],...}

// ── 回调 ──
typedef void (*thin_agent_log_callback)(int level, const char* message);
THIN_API void thin_agent_set_log_callback(thin_agent_t* agent,
                                           thin_agent_log_callback cb);

#ifdef __cplusplus
}
#endif

#endif // THIN_AGENT_API_H
```

---

## 4. 组播发现协议

### 4.1 协议规格

```
传输层:    UDP
组播地址:  224.0.0.199 (IANA 未分配段，可配置)
端口:      9876 (可配置)
消息格式:  JSON，单行，以 \n 结尾
TTL:       3 (局域网内)
```

### 4.2 探针消息（Client → Multicast Group）

```json
{
  "type": "thin_agent.discover",
  "version": "1",
  "client_id": "frontend-abc123",
  "client_type": "cli"
}
```

### 4.3 响应消息（Server → Client Unicast）

```json
{
  "type": "thin_agent.announce",
  "version": "1",
  "server_id": "thin-agentd-xyz789",
  "hostname": "dev-server",
  "os": "Linux 6.8.0-52-generic",
  "arch": "x86_64",
  "ip": "192.168.1.100",
  "ws_port": 8765,
  "agent_version": "v0.12.0",
  "profiles": ["default", "feishu-deepseek"],
  "capabilities": ["agent", "gateway", "local_models"],
  "load": 0.35,
  "uptime_seconds": 86400,
  "signature": "hmac-sha256..."
}
```

### 4.4 流程

```
Client                          Multicast Group           Server A        Server B
  │                                  │                       │               │
  │──── probe (UDP) ────────────────►│                       │               │
  │                                  │── probe ────────────►│               │
  │                                  │── probe ────────────────────────────►│
  │                                  │                       │               │
  │◄─── announce (unicast) ──────────│───────────────────────│               │
  │◄─── announce (unicast) ──────────│───────────────────────────────────────│
  │                                  │                       │               │
  │  收集回复列表 → 用户选择 → WS 连接                      │               │
```

### 4.5 安全考虑

- 组播仅局域网可达（路由器默认不转发组播）
- 可选 HMAC-SHA256 签名（需要前后端共享 secret）
- 生产环境建议禁组播，手动配置 IP

---

## 5. WS 协议（现有，扩展）

### 5.1 连接流程

```
Client                                    Server (thin_agentd)
  │                                         │
  │──── ws://192.168.1.100:8765/ws ────────►│
  │                                         │── 分配 session_id
  │◄─── hello {version, session_id} ────────│
  │                                         │
  │──── {type:"chat", text:"你好"} ────────►│
  │◄─── chat_chunk {chunk:"你", done:false}─│  (流式)
  │◄─── chat_chunk {chunk:"好", done:true}──│
  │◄─── chat_result {text, trace, ...} ─────│
```

### 5.2 现有消息类型（保持不变）

| type | 方向 | 说明 |
|------|:--:|------|
| `chat` | C→S | 对话消息 |
| `chat_chunk` | S→C | 流式 token |
| `chat_result` | S→C | 最终回复 |
| `spawn_agent` | C→S | 创建子 Agent |
| `role_list` / `role_register` / `role_remove` | C→S | 角色管理 |
| `agent_dag` | C→S | DAG 编排 |
| `agent_synthesize` | C→S | LLM 合成 |
| `agent_decompose` | C→S | 动态分解 |
| `agent_message` / `agent_inbox` | C→S | Agent 间通信 |
| `bb_write` / `bb_read` / `bb_clear` | C→S | 黑板操作 |
| `kanban_*` | C→S | Kanban 看板 |
| `agent_debate` | C→S | 协商辩论 |

### 5.3 新增消息类型

```json
// 查询后端信息
{"type": "server_info"}
→ {"type":"server_info_resp","hostname":"dev-pc","os":"Linux","version":"v0.12.0",
   "profiles":["default"],"uptime":86400,"connections":3}

// 切换 profile
{"type": "switch_profile", "profile": "gpu-server"}
→ {"type":"profile_switched","ok":true,"profile":"gpu-server","model":"qwen-72b"}

// 列出活跃 sessions
{"type": "list_sessions"}
→ {"type":"sessions_list","sessions":[
     {"id":"ws-1","created":"2026-07-01T10:00:00Z","messages":42},
     {"id":"ws-2","created":"2026-07-01T11:00:00Z","messages":15}
   ]}

// 心跳
{"type": "ping"}
→ {"type":"pong","ts":1719900000}

// 系统通知
S→C: {"type":"notice","severity":"info","message":"Model loaded: qwen-72b"}
S→C: {"type":"notice","severity":"warn","message":"Memory usage > 80%"}
```

---

## 6. 前端架构

### 6.1 CLI 前端（thin_agent_cli）

```
┌────────────────────────────────────────────┐
│ thin_agent_cli                              │
│                                             │
│  ┌─────────────┐   ┌──────────────────┐     │
│  │ Discovery    │   │  WS Client       │     │
│  │ (UDP mcast)  │   │  (reconnect,     │     │
│  │              │   │   heartbeat)     │     │
│  └──────┬───────┘   └────────┬─────────┘     │
│         │                    │               │
│  ┌──────▼────────────────────▼─────────┐     │
│  │         Chat Loop (REPL)             │     │
│  │  readline → WS send → stream render  │     │
│  └──────────────────────────────────────┘     │
│                                             │
│  命令:                                       │
│    /connect 192.168.1.100:8765              │
│    /discover  (组播扫描)                     │
│    /backends  (列出已知后端)                 │
│    /switch <n> (切换后端)                   │
│    /profile <name> (切换 profile)          │
│    /info      (后端信息)                    │
│    /quit                                    │
└────────────────────────────────────────────┘
```

### 6.2 Web 前端（ws_agent.html 增强）

现有 1426 行，基于浏览器 WebSocket。增强点：

- 连接面板：手动输入 `ws://IP:PORT/ws` 或"扫描局域网"
- 多后端管理：收藏后端列表，一键切换
- 后端断开自动重连（指数退避）
- Tab 页：多 session 并行

### 6.3 手机端

```
┌──────────────────────────────────────┐
│         Mobile App (Android/iOS)      │
│                                       │
│  ┌─────────────┐  ┌────────────────┐  │
│  │ 本地模式     │  │  远程模式       │  │
│  │             │  │                │  │
│  │ System.     │  │  WS Client     │  │
│  │ loadLibrary │  │  → 组播发现    │  │
│  │ ("thin_     │  │  → 手动IP      │  │
│  │  agent_     │  │                │  │
│  │  core")     │  │                │  │
│  └──────┬──────┘  └───────┬────────┘  │
│         │                 │           │
│  ┌──────▼─────────────────▼────────┐  │
│  │       WebView 渲染前端 UI        │  │
│  │       (复用 ws_agent.html)       │  │
│  └──────────────────────────────────┘  │
└──────────────────────────────────────┘
```

---

## 7. thin_agentd 守护进程

### 7.1 职责

- 加载 `libthin_agent_core.so`
- 启动 WS 服务器 + 组播响应器
- 管理配置（config.yaml → C ABI config）
- 信号处理（SIGTERM 优雅退出）
- 日志输出（stdout 或 syslog）

### 7.2 平台服务化

| 平台 | 服务管理 |
|------|---------|
| Linux | systemd unit (`thin-agentd.service`) |
| macOS | launchd plist (`com.thinagent.daemon.plist`) |
| Windows | Windows Service (`sc create thin_agentd`) |
| WSL | 同 Linux systemd |

### 7.3 伪代码

```cpp
#include <dlfcn.h>
#include "thin_agent/api/agent_api.h"

int main(int argc, char* argv[]) {
    // 1. dlopen 核心库
    void* lib = dlopen("libthin_agent_core.so", RTLD_NOW);
    auto create = (thin_agent_t*(*)(const thin_agent_config_t*))
                  dlsym(lib, "thin_agent_create");
    auto start  = (int(*)(thin_agent_t*))
                  dlsym(lib, "thin_agent_start");
    // ...

    // 2. 创建并启动
    thin_agent_config_t cfg = {
        .config_yaml = "config/demo.model.yaml",
        .profile = "default",
        .ws_port = 8765,
        .multicast_port = 9876,
    };
    thin_agent_t* agent = create(&cfg);
    start(agent);

    // 3. 等待信号
    wait_for_signal(SIGTERM, SIGINT);

    // 4. 清理
    stop(agent);
    destroy(agent);
    dlclose(lib);
}
```

---

## 8. 项目目录结构变更

```
thin_agent/
├── include/thin_agent/
│   ├── api/                        ★ 新增
│   │   ├── agent_api.h             C ABI 公共接口
│   │   ├── gateway_api.h           IM 网关 C ABI
│   │   └── discovery.h             组播发现 C ABI
│   ├── agent/                      (不变)
│   ├── core/                       (不变)
│   ├── fdbus/                      (不变)
│   ├── llm/                        (不变)
│   └── local/                      (不变)
├── src/
│   ├── api/                        ★ 新增
│   │   ├── agent_api.cpp           C ABI 实现（包装 AgentService）
│   │   ├── gateway_api.cpp         C ABI 实现（包装 IM gateway）
│   │   └── discovery.cpp           组播发现实现
│   ├── daemon/                     ★ 新增
│   │   └── thin_agentd_main.cpp    dlopen 守护进程入口
│   ├── cli/                        ★ 新增
│   │   └── thin_agent_cli.cpp      WS 前端 CLI
│   ├── agent/                      (不变)
│   ├── core/                       (不变, AgentService 内部重构)
│   ├── demo/                       → 合并到 daemon/ + cli/
│   ├── llm/                        (不变)
│   └── local/                      (不变)
├── mobile/                         ★ 新增
│   ├── android/
│   │   ├── app/                    Android App (Kotlin + WebView)
│   │   └── jni/                    JNI 胶水层
│   └── ios/
│       ├── ThinAgent/              iOS App (Swift)
│       └── framework/              .framework 构建
├── cmake/
│   ├── toolchain-android.cmake     ★ 新增
│   ├── toolchain-ios.cmake         ★ 新增
│   ├── toolchain-windows-msvc.cmake★ 新增
│   └── ...                         (现有保持不变)
├── packaging/                      ★ 新增
│   ├── linux/                      systemd unit, .deb spec
│   ├── macos/                      launchd plist, .pkg
│   └── windows/                    .msi installer, Service config
└── docs/
    ├── PRD.md                      ★ 本 PRD
    └── ARCHITECTURE.md             ★ 本文档
```

---

## 9. 实施计划（18 步 / 4 Phase）

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

> Phase 1 做完 = 最小可用闭环：`thin_agentd` 后台 + 前端通过 WS 通信。

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

## 10. 风险与对策

| 风险 | 影响 | 对策 |
|------|:--:|------|
| C ABI 限制 C++ 特性 | 接口设计 | 不透明句柄 + JSON 序列化，核心内仍用 C++ |
| ONNX Runtime 移动端编译 | Android/iOS | 使用 ONNX Runtime Mobile 预编译包 |
| 组播在部分网络被禁用 | 发现失效 | 回退手动输入 IP，组播是可选功能 |
| 多平台构建复杂度 | 维护成本 | CMake toolchain 统一管理，CI 矩阵验证 |

---

## 附录 A：嵌入式设备（aarch64）部署参考

> 来源：`ARCHITECTURE.md`（v0.2，嵌入式早期架构）。本章节保留嵌入式特定约束。

### A.1 设备背景

- ARM64（aarch64）嵌入式 Linux，相机/座舱设备
- 资源受限（根分区空间紧张），稳定性与可预测性优先
- 具备原生 FDBus 服务生态
- 设备控制能力通过 FDBus 调用原生服务，非直接侵入业务模块
- 依赖接入：先 x86 fake 单测与 demo，再在 aarch64 从 `leaptic_app` 工程对齐 FDBus

### A.2 四层核心抽象

thin_agent 明确采用四层抽象作为主线：

1. **LLM 层（大脑）** — 云端 LLM 调用 + 本地轻量语义能力（intent）
2. **Prompt/Policy 层（指令与边界）** — 规则路由、阈值策略、Policy Gate
3. **Context 层（记忆与状态）** — memory_recent / memory_history / event_recent / observation
4. **Harness Loop 层（调度闭环）** — AgentService + TaskEngine + Tool Layer + Observability

标准闭环：`用户目标 → Harness 判断与拆解 → Context 拼装 → LLM/规则推理 → 工具执行 → 结果回流 Context → 输出/继续迭代`

### A.3 分层智能体决策引擎

1. **规则层（Deterministic Hard Routing）** — 安全/设备控制/关键动作必须命中确定性规则
2. **轻量语义路由层（Intent Classifier）** — ONNX/TFLite 小模型输出 intent/confidence/slots
3. **策略层（Planner/Policy）** — 综合运行状态、历史记忆、风险等级进行路径选择

### A.4 FDBus 设备控制适配

- 抽象接口 `IDeviceControl`：`get` / `set` / `act` / `evt`
- 真实实现 `FdbusDeviceControl`（仅 aarch64），测试用 `FakeDeviceControl`（x86）
- 首批联调目标服务：`camera`
- 构建开关：`THIN_AGENT_WITH_FDBUS=ON/OFF`，`THIN_AGENT_TARGET_ARCH=aarch64|x86_64`

### A.5 测试隔离

- x86 CI：全部单元测试 + FakeDeviceControl demo
- aarch64：设备联调时开启真实 FDBus
- 验收：x86 CI 核心闭环通过 + aarch64 FDBus 真实调用闭环

### A.6 安全与可靠性基线

- 默认拒绝未知 action，白名单放行
- 所有任务带 request_id / trace_id / timestamp
- 幂等处理（重复任务不重复执行副作用）
- 网络重试采用指数退避 + 抖动
- 崩溃恢复后从 SQLite 继续未完成任务
- MVP 阶段 WS 不使用 token/WSS，但保留后续开启鉴权与加密的配置开关

### A.7 版本里程碑（历史）

- v0.1 骨架版 → v0.2 x86 WS Agent 交互闭环 → v0.3 稳定版（重试/幂等/审计）
- v0.4 在线 LLM 真实调用 → v0.5 端云协同增强 → v0.6 分层决策引擎三层深化
- v0.7 aarch64 + FDBus 真实能力接入
