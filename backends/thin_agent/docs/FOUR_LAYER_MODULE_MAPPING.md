# THIN_AGENT_FOUR_LAYER_MODULE_MAPPING_V0_1

- 当前版本：v0.6.2
- 目标：把 `LLM + Prompt/Policy + Context + Harness Loop` 四层抽象映射到当前可运行代码文件，作为团队对齐与评审基线。

---

## 1. 四层模块映射总表（代码文件 → 四层归属）

| 四层 | 模块/职责 | 代码文件（当前落地） | 关键接口/字段（示例） |
|---|---|---|---|
| LLM 层 | 云端推理调用 | `src/llm/CloudLlmClient.cpp`<br>`include/thin_agent/llm/CloudLlmClient.h` | `chat_completion(...)`、云端返回文本/错误、HTTP 状态 |
| LLM 层 | 本地轻量语义（intent） | `src/core/IntentOnnx.cpp`<br>`include/thin_agent/core/IntentOnnx.h` | `infer_profile_confidence_onnx(...)`、`intent_backend=onnx/local-model/rules` |
| LLM 层 | 模型/Provider 配置归一化 | `src/llm/ProviderFactory.cpp`<br>`src/llm/ModelConfig.cpp`<br>`src/llm/DemoConfigCompat.cpp`<br>`include/thin_agent/llm/*.h` | `provider/model/api_base/fallback` 解析与兼容 |
| Prompt/Policy 层 | 规则路由 + 阈值决策（execute/clarify/fallback/reject） | `src/core/AgentService.cpp`<br>`include/thin_agent/core/AgentService.h` | `decision.route/policy/intent/confidence`、`kExecuteThreshold/kClarifyThreshold` |
| Prompt/Policy 层 | 风险门控与云策略二次裁决 | `src/core/AgentService.cpp` | `risk=high -> local_reject`、`cloud_strategy -> local_route_hint` |
| Prompt/Policy 层 | 动作准入执行（受策略控制） | `src/core/ActionExecutor.cpp`<br>`include/thin_agent/core/ActionExecutor.h` | `handle_action(...)`、动作白名单路径 |
| Context 层 | 会话短期记忆/历史/事件窗口 | `src/core/AgentService.cpp` | `memory_recent`、`memory_history`、`event_recent` |
| Context 层 | 任务状态与审计持久化（SQLite） | `src/core/TaskEngine.cpp`<br>`include/thin_agent/core/TaskEngine.h` | `task_submit/get/list/cancel/replay/audit`、`task_audits` |
| Context 层 | 外部查询结果回流上下文 | `src/core/ExternalInfoClient.cpp`<br>`include/thin_agent/core/ExternalInfoClient.h` | `observation.external_result/source/http_status` |
| Harness Loop 层 | 主闭环编排（接收→路由→执行→回流→输出） | `src/core/AgentService.cpp` | `handle_request(...)`、`tool_calls`、`decision_trace` |
| Harness Loop 层 | 任务调度执行闭环 | `src/core/TaskEngine.cpp` | 状态机：`queued/running/retrying/success/failed/cancelled` |
| Harness Loop 层 | WS 运行入口与服务生命周期 | `src/demo/ws_agent_main.cpp`<br>`src/demo/main.cpp` | 启动日志 `[ws_agent_cpp] version=...`、端口监听 |
| Harness Loop 层 | 设备能力适配（x86 fake / 设备侧 fdbus） | `src/fdbus/FakeDeviceControl.cpp`<br>`src/fdbus/FdbusDeviceControl.cpp`<br>`include/thin_agent/fdbus/*.h` | `IDeviceControl` 抽象，动作/事件落地 |

---

## 2. 主链路（与四层对应）

`用户输入(chat/action/task) -> Harness(AgentService) -> Policy(规则+阈值+风险门控) -> LLM(本地/云) -> Tool/Task 执行 -> Context 回流(memory/event/observation) -> 输出 chat_result`

对齐要点：
- **最终裁决在端侧**：云返回是策略输入，不是最终输出。
- **策略可观测**：必须可见 `decision` 与 `decision_trace`。
- **上下文可回放**：关键结果进入 `memory/history/event/task_audit`。

---

## 3. 场景追踪表（功能场景 → 四层 → 代码锚点）

| 场景 | Harness Loop（编排入口） | Prompt/Policy（裁决） | LLM（语义/推理） | Context（状态回流） |
|---|---|---|---|---|
| chat 主体问答（你是谁/能力介绍） | `AgentService::handle_request(type=chat)` | `decision.route=local_profile/local_status` | `classify_local_intent(...)`（rules/onnx） | `memory_recent/history` + `decision_trace` |
| task 任务执行（拍照/录制） | `task_submit -> TaskEngine` | `local_task_inline` + 风险门控 | 云策略可给 `local_route_hint`，本地二次裁决 | `task/task_audits` + `observation.inline_task` |
| action 直达动作（capture/start_recording） | `handle_action(...)` | allowlist + 参数约束 | 可选语义识别后路由到 action | `observation.result` + event/audit |
| external 外部查询（weather/news/general） | `local_external_*` 分支 | execute/clarify 阈值（0.75/0.45） | rules 槽位 + 可选云策略辅助 | `observation.external_result/source/http_status` |

---

## 4. 嵌入式轻量级约束（必须长期满足）

1. **进程轻量**：`thin_agent` 单体可执行保持小体积（当前 x86 约 2.5MB，动态链接）。
2. **依赖克制**：主链路仅 C++17 + Mongoose + SQLite + nlohmann::json；不引入 Python 运行时依赖。
3. **端侧主裁决**：云仅建议，本地最终决策，弱网/断网可降级工作。
4. **可恢复**：任务状态机与审计持久化，进程重启后可追溯。
5. **可观测**：关键路径必须含 `decision` + `decision_trace` + version 启动日志。

轻量化回归建议（每次迭代至少执行）：
- 二进制体积检查：`file build/thin_agent` + 大小对比
- 全量回归：`unit + ctest`
- 启动版本验活：`[ws_agent_cpp] version=...`

---

## 5. 评审检查清单（建议）

1. 新增能力是否明确归属到四层之一（避免“功能漂浮”）？
2. 是否补齐可观测字段（至少 `decision.route/policy`）？
3. 是否补齐回归测试（unit + ctest）并避免破坏现有主体路由？
4. 是否同步版本号与 PRD/协议/架构/测试策略文档？
5. 是否满足嵌入式轻量约束（体积、依赖、断网降级）？

---

## 6. 与其他文档关系

- 架构总览：`ARCHITECTURE.md`
- PRD 基线：`PRD.md`
- 协议草案：`PROTOCOL.md`
- 测试策略：`TESTING_STRATEGY.md`
- external 路由：`EXTERNAL_QUERY_ROUTING_SPEC.md`
- 目录边界：`PROJECT_STRUCTURE.md`
