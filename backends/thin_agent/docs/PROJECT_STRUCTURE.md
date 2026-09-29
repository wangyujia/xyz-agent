# thin_agent 目录结构与模块边界（v0.1）

> 本文档定义 C++17 thin_agent 的推荐目录结构、模块职责与边界，作为后续编码与代码评审基线。

---

## 1. 推荐目录结构

```text
thin_agent/
├── CMakeLists.txt
├── README.md
├── configs/
│   ├── agent.dev.json
│   └── agent.prod.json
├── docs/
│   ├── ARCHITECTURE.md
│   ├── PROTOCOL.md
│   ├── PROJECT_STRUCTURE.md
│   ├── DB_SCHEMA.md
│   └── STATE_MACHINE.md
├── scripts/
│   ├── run_local.sh
│   ├── package.sh
│   └── health_check.sh
├── third_party/
│   └── (按需：mongoose/sqlite/json/gtest 来源管理)
├── src/
│   ├── main.cpp
│   ├── app/
│   │   ├── AgentApp.h
│   │   └── AgentApp.cpp
│   ├── core/
│   │   ├── Types.h
│   │   ├── ErrorCode.h
│   │   ├── Status.h
│   │   ├── Clock.h
│   │   └── Utils.h
│   ├── config/
│   │   ├── Config.h
│   │   ├── ConfigLoader.h
│   │   └── ConfigLoader.cpp
│   ├── logging/
│   │   ├── Logger.h
│   │   └── Logger.cpp
│   ├── transport/
│   │   ├── HttpClient.h
│   │   ├── MongooseHttpClient.cpp
│   │   ├── ApiClient.h
│   │   └── ApiClient.cpp
│   ├── protocol/
│   │   ├── Dto.h
│   │   ├── JsonCodec.h
│   │   └── JsonCodec.cpp
│   ├── runtime/
│   │   ├── EventLoop.h
│   │   ├── Scheduler.h
│   │   ├── WorkerPool.h
│   │   └── WorkerPool.cpp
│   ├── task/
│   │   ├── Task.h
│   │   ├── TaskStateMachine.h
│   │   ├── TaskManager.h
│   │   └── TaskManager.cpp
│   ├── policy/
│   │   ├── PolicyGate.h
│   │   ├── ActionRegistry.h
│   │   └── PolicyGate.cpp
│   ├── tools/
│   │   ├── ITool.h
│   │   ├── HealthReportTool.cpp
│   │   ├── CollectLogsTool.cpp
│   │   └── RestartServiceTool.cpp
│   ├── storage/
│   │   ├── SqliteDB.h
│   │   ├── SqliteDB.cpp
│   │   ├── TaskRepository.h
│   │   ├── TaskRepository.cpp
│   │   ├── AuditRepository.h
│   │   └── AuditRepository.cpp
│   ├── security/
│   │   ├── TokenAuth.h
│   │   ├── SignatureVerifier.h
│   │   └── SignatureVerifier.cpp
│   ├── fdbus/
│   │   ├── IDeviceControl.h
│   │   ├── FdbusDeviceControl.h
│   │   ├── FdbusDeviceControl.cpp
│   │   ├── FakeDeviceControl.h
│   │   ├── FakeDeviceControl.cpp
│   │   ├── MockDeviceControl.h        # 可选：单测桩
│   │   ├── MockDeviceControl.cpp      # 可选：单测桩
│   │   ├── FdbusCodec.h
│   │   └── FdbusCodec.cpp
│   ├── heartbeat/
│   │   ├── HeartbeatService.h
│   │   └── HeartbeatService.cpp
│   └── llm/
│       ├── LlmClient.h
│       ├── CloudLlmClient.cpp
│       ├── PromptBuilder.h
│       └── PromptBuilder.cpp
└── tests/
    ├── CMakeLists.txt
    ├── unit/
    │   ├── test_config_loader.cpp
    │   ├── test_policy_gate.cpp
    │   ├── test_task_state_machine.cpp
    │   └── test_json_codec.cpp
    └── integration/
        ├── test_poll_execute_callback.cpp
        └── test_retry_and_recover.cpp
```

---

## 2. 模块边界（职责定义）

## 2.1 `app/`（应用编排层）

**职责**：
- 组装所有模块（DI/依赖注入）
- 管理启动顺序与关闭顺序
- 控制主流程（heartbeat、poll、execute、callback）

**边界**：
- 不包含业务细节逻辑
- 不直接操作 SQL 和网络底层

---

## 2.2 `core/`（基础公共层）

**职责**：
- 公共类型、错误码、状态枚举、工具函数
- 时间与平台相关抽象

**边界**：
- 不依赖上层业务模块
- 仅提供基础设施

---

## 2.3 `config/`（配置层）

**职责**：
- 使用 `nlohmann::json` 加载/校验配置
- 提供默认值与配置快照

**边界**：
- 只负责配置，不做网络/任务逻辑

---

## 2.4 `logging/`（日志层）

**职责**：
- 统一日志接口
- 结构化日志与日志级别控制

**边界**：
- 不能反向依赖业务模块

---

## 2.5 `transport/`（传输层）

**职责**：
- 基于 Mongoose 实现 HTTP/HTTPS 客户端
- 封装 API 请求（register/heartbeat/poll/callback）

**边界**：
- 不处理任务执行逻辑
- 只返回传输结果与错误

---

## 2.6 `protocol/`（协议编解码层）

**职责**：
- DTO 定义
- JSON <-> C++ struct 转换

**边界**：
- 不包含网络和数据库逻辑

---

## 2.7 `runtime/`（运行时层）

**职责**：
- 事件循环
- 定时调度（heartbeat/poll/retry）
- 工作线程池管理

**边界**：
- 不决定任务是否可执行（交给 policy）

---

## 2.8 `task/`（任务编排层）

**职责**：
- 任务状态机
- 任务队列管理
- 协调 policy/tools/storage

**边界**：
- 不直接做 HTTP 底层调用

---

## 2.9 `policy/`（策略安全层）

**职责**：
- action 白名单
- 参数校验
- 超时/权限/风险校验

**边界**：
- 不能直接执行工具，只做准入决策

---

## 2.10 `tools/`（设备能力层）

**职责**：
- 具体动作实现（采集日志、状态上报、服务重启等）

**边界**：
- 通过统一接口暴露，不直接感知外部协议

说明：
- 若动作需调用设备原生能力，`tools` 应通过 `fdbus/` 适配层访问，而非直接耦合 FDBus C API。

---

## 2.10.1 `fdbus/`（设备原生能力适配层）

**职责**：
- 定义抽象接口（`IDeviceControl`）供上层依赖
- 基于 `fdbus_clib` 提供真实实现（`FdbusDeviceControl`）
- 提供测试/演示使用的 Mock 实现（`MockDeviceControl`）
- 处理 FDBus 消息编解码、调用超时、错误转换
- 对接设备原生服务（参考 `~/code/leaptic_app`）

**边界**：
- 不包含业务决策
- 不直接操作任务状态机，仅提供“可调用能力”

参考：
- `~/code/leaptic_app/wifi_manager/include/WifiFdbusComms.hpp`

---

## 2.11 `storage/`（持久化层）

**职责**：
- SQLite 连接与事务封装
- 任务、重试、审计数据读写

**边界**：
- 不包含业务决策

---

## 2.12 `security/`（安全层）

**职责**：
- Token 校验
- 时间窗校验
- 签名校验（后续增强）

**边界**：
- 不做业务动作执行

---

## 2.13 `heartbeat/`（心跳服务）

**职责**：
- 聚合运行状态指标
- 触发心跳上报

**边界**：
- 不负责任务调度

---

## 2.14 `llm/`（在线增强层，可选）

**职责**：
- 联网时调用云端 LLM
- 构造 prompt 与上下文（本地记忆摘要）
- 读取并落实 `PRD.md` 中的模型配置与fallback约束

**边界**：
- 输出建议，不直接绕过 policy 执行

---

## 3. 依赖方向约束（必须遵守）

建议依赖方向：

`app -> (runtime, task, heartbeat, transport, storage, policy, tools, fdbus, security, config, logging, protocol)`

在设备控制场景下补充：

`tools -> fdbus(IDeviceControl) -> core/logging`

核心约束：
1. `tools` 不依赖 `transport`
2. `policy` 不依赖 `tools` 实现细节（只依赖抽象）
3. `storage` 不依赖 `task`
4. `protocol` 不依赖 `transport/storage`
5. `llm` 不得直接执行动作，必须通过 `task + policy`
6. `fdbus` 不依赖 `task/policy/storage`（保持适配层纯净）
7. `tools` 不直接 include `fdbus_clib.h`（统一经 `fdbus/` 封装）
8. `x86_64` 构建默认链接 `MockDeviceControl`，`aarch64` 构建可链接 `FdbusDeviceControl`

---

## 4. MVP 模块最小落地建议

MVP 实现优先顺序：
1. `config + logging + core`
2. `storage(sqlite) + protocol`
3. `transport(api client)`
4. `fdbus(adapter interface + mock)`
5. `policy + tools(3个最小 action，先接 mock)`
6. `task + runtime + heartbeat`
7. `aarch64` 接入 `FdbusDeviceControl` 真实现并联调
8. `tests(unit + integration + demo)`

### 4.1 当前代码落地（2026-07-02）

已落地到 C++ 主体：
- `src/core/AgentService.cpp`：WS 消息路由、会话管理、短期记忆、事件窗口、trace/cmd 元信息、任务取消/重放/审计查询路由
- `src/core/TaskEngine.cpp`：SQLite 任务状态机（queued/running/retrying/success/failed/cancelled）、状态迁移审计表 `task_audits`、指数退避+jitter 重试、统一错误码映射、幂等
- `src/core/ActionExecutor.cpp`：统一动作执行入口（受 allowlist 约束）
- `src/fdbus/FakeDeviceControl.cpp`：x86 阶段设备控制假实现（可替换为真实适配器）

Demo 可体验接口：
- `status` / `ping` / `chat` / `action`
- `task_submit` / `task_get` / `task_list` / `task_cancel` / `task_replay` / `task_audit`
- `memory_recent` / `event_recent`

前端体验页：
- `ws_agent.html`（覆盖主体能力演示）

---

## 4.2 chat_policy 配置策略（v0.6.13）

在当前代码中，chat 关键词/文案由 `config/chat_policy.json`（或 `THIN_AGENT_CHAT_POLICY_PATH` 指定路径）驱动。

### 4.2.1 生效路径

- 默认读取顺序（进程内）：
  1. `THIN_AGENT_CHAT_POLICY_PATH`（若设置）
  2. `config/chat_policy.json`
  3. `../config/chat_policy.json`
  4. `/root/code/thin_agent/config/chat_policy.json`

> 注意：策略文件在服务进程内加载。修改后请重启对应进程再验收。

### 4.2.2 v0.6.13 关键变化（cloud/offline fallback 文案）

- 新增并优先使用：
  - `cloud.offline_local_fallback`
  - `cloud.fallback_missing_key`
  - `cloud.fallback_call_failed`
  - `cloud.error_call_failed_no_fallback`
- 兼容旧键（仍可保留）：
  - `cloud.missing_key_fallback`
  - `cloud.call_failed_fallback`
  - `cloud.call_failed_no_fallback`

### 4.2.3 优先级规则

- offline 回显：
  - `cloud.offline_local_fallback` -> `cloud.local_offline_fallback` -> 内置默认
- missing_api_key：
  - `cloud.fallback_missing_key` -> `cloud.missing_key_fallback` -> 内置默认
- cloud_call_failed（fallback=offline）：
  - `cloud.fallback_call_failed` -> `cloud.call_failed_fallback` -> 内置默认
- cloud_call_failed（fallback!=offline）：
  - `cloud.error_call_failed_no_fallback` -> `cloud.call_failed_no_fallback` -> 内置默认

### 4.2.4 最小配置示例

```json
{
  "templates": {
    "cloud.offline_local_fallback": "[offline-fallback] 当前离线模式。输入回显：{text}",
    "cloud.fallback_missing_key": "[offline-fallback] 未检测到云模型密钥。输入回显：{text}",
    "cloud.fallback_call_failed": "[offline-fallback] 云调用失败，已离线回退。输入回显：{text}",
    "cloud.error_call_failed_no_fallback": "[cloud-error] 云调用失败且未允许离线回退。"
  }
}
```

### 4.2.5 运维操作示例

```bash
export THIN_AGENT_CHAT_POLICY_PATH=/absolute/path/to/chat_policy.json
# 重启 thin_agent 进程
# 再执行 chat smoke 验证文案是否命中新模板
```

### 4.2.6 模板占位符建议

- `cloud.error_call_failed_no_fallback`：固定错误提示，可不带变量。
- 其余 fallback 文案建议包含 `{text}`，用于输入回显与问题定位。

### 4.2.7 complex_intent 规则探针（v0.6.21）

- 在 `AgentService(chat)` 路径新增**复杂意图规则探针**，当前仅观测，不改变路由策略。
- 触发方式：检测输入中的复合连词/条件标记（如 `如果/先/然后/否则/if/then/else`）。
- 在 `AgentService(chat)` 路径新增**复杂意图规则探针**，当输入命中连词模式（如“如果/先/然后/否则/if/then/else”）时：
  - `observation.complex_intent_probe.detected`（bool）
  - `observation.complex_intent_probe.markers`（array）
  - `decision_trace` 追加 `layer=probe`
- 开关：`DemoConfigCompat.complex_intent_force_cloud`（默认 `false`）
  - `false`：仅观测，不改路由
  - `true` 且命中复杂意图：跳过本地短路规则，进入 cloud strategy 路径（强制上云拆解）
- 回包可增加：`observation.cloud_complex_intent_gate = {enabled, detected, forced}`

---

## 5. 与现有文档关系

- 架构总览：`ARCHITECTURE.md`
- 协议细节：`PROTOCOL.md`
- 接口与错误码：`INTERFACE_AND_ERRORCODE.md`
- 本文档：目录结构与模块边界（工程落地视图）
