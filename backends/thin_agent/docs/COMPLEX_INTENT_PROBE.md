# thin_agent complex_intent 规则探针说明（v0.1）

> 目标：在不改变现有路由策略的前提下，为复合/多意图输入建立低成本、可观测的检测信号，支撑后续“复杂意图强制上云拆解”决策。

---

## 1. 范围与原则

- v0.6.21 版本定位：**Probe only（只观测）**。
- v0.6.23 起：支持开关式强制上云拆解（`complex_intent_force_cloud=true`）。
- 不变更：默认配置下（开关关闭）既有本地路由行为保持不变。
- 变更点：开启开关且命中复杂意图时，优先进入 cloud strategy 路径。
  - `observation.complex_intent_probe`
  - `decision_trace` 中的 `probe` 层。

---

## 2. 探针触发规则（v0.6.21）

当 chat 输入命中以下连词标记之一时，判定 `detected=true`：

- 中文：`如果`、`先`、`然后`、`否则`
- 英文：`if`、`then`、`else`

输出：

```json
"complex_intent_probe": {
  "detected": true,
  "markers": ["cn_if", "cn_then"]
}
```

未命中时：

```json
"complex_intent_probe": {
  "detected": false,
  "markers": []
}
```

---

## 3. 回包字段约束

### 3.1 observation

- `observation.complex_intent_probe.detected`：bool，必有
- `observation.complex_intent_probe.markers`：array[string]，必有（可空）
- `observation.cloud_complex_intent_gate.enabled`：bool（v0.6.23）
- `observation.cloud_complex_intent_gate.detected`：bool（v0.6.23）
- `observation.cloud_complex_intent_gate.forced`：bool（v0.6.23）

### 3.2 decision_trace

新增一层：

- `layer = "probe"`
- `input = 原始 text`
- `output = {"complex_intent_probe": {...}}`
- `ts` 由服务端统一补全

---

## 4. 与后续策略关系

v0.6.23 前：
- Probe 仅观测，不直接改变路由。

v0.6.23 起：
- 当 `complex_intent_force_cloud=true` 且 `detected=true`，会跳过本地短路规则，进入 cloud strategy。
- 该路径仍受 budget gate + policy contract gate + task pipeline contract gate 三层约束。

---

## 5. 测试要点

- 默认关（`complex_intent_force_cloud=false`）：
  - 普通句：`detected=false`
  - 复合句：`detected=true` 且 `markers` 非空
  - 路由保持本地短路逻辑（不强制上云）
- 开关开（`complex_intent_force_cloud=true`）+ 复合句：
  - 进入 cloud strategy 路径（`intent_backend=cloud-strategy`）
  - `observation.cloud_complex_intent_gate.forced=true`
- `decision_trace` 始终包含 `probe` 层

---

## 6. 版本信息

- 首次引入版本：`v0.6.21`
- 开关升级版本：`v0.6.23`（`complex_intent_force_cloud`）
- 相关模块：
  - `src/core/AgentService.cpp`
  - `tests/unit/test_agent_service.cpp`
  - `PROTOCOL.md`
  - `PROJECT_STRUCTURE.md`

## 7. 下一步（与 cloud task pipeline 契约衔接）

- `complex_intent_probe` 将作为“是否触发云端拆解”的观测输入之一。
- v0.6.22 起，云侧可携带 `task_pipeline[]`，端侧执行前先做契约校验：
  - `step` 为 object
  - `action` 为白名单（`capture_photo/start_recording/stop_recording`）
  - `params` 如存在必须为 object
- 校验结果在 `observation.cloud_task_pipeline_contract` 回显；非法 pipeline 统一降级为 `local_clarify` + `cloud_policy_contract_violation`。
- v0.6.23 起可通过 `complex_intent_force_cloud=true` 强制复杂意图进入 cloud strategy，并保留三层门控。