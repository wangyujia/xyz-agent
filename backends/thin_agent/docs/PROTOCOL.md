# thin_agent 协议草案（v0.6.2）

> 目标：定义 thin_agent 与上层调度/控制端通信的最小协议，先满足 MVP（心跳、拉任务、回传结果、可靠重试）。

---

## 1. 协议设计原则

1. **简单可落地**：HTTP/HTTPS + JSON，便于嵌入式快速实现
2. **幂等可恢复**：所有请求可重试，不因重复请求造成副作用
3. **可追踪**：全链路 `trace_id` + `request_id`
4. **安全可控**：设备身份校验、签名/Token、时间戳防重放

---

## 2. 通用字段约定

所有请求 body（或 header）建议包含：

- `device_id`：设备唯一标识
- `agent_id`：agent 实例标识（同设备可多实例时使用）
- `request_id`：请求唯一 ID（UUID）
- `trace_id`：任务链路追踪 ID
- `timestamp`：Unix 秒或毫秒时间戳
- `signature`：可选，HMAC 签名（后续增强）
- `version`：agent 版本号

---

## 3. API 草案

## 3.1 设备注册（可选）

- **Method**: `POST`
- **Path**: `/api/v1/agent/register`

### Request
```json
{
  "device_id": "dev-001",
  "agent_id": "thin-agent-001",
  "request_id": "uuid-xxx",
  "timestamp": 1760000000,
  "version": "0.1.0",
  "capabilities": ["health_report", "collect_logs", "switch_mode", "capture_photo", "start_recording", "stop_recording", "fetch_capture_results"],
  "meta": {
    "arch": "aarch64",
    "os": "leaptic-distro-perf",
    "ip": "10.0.0.12"
  }
}
```

### Response
```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "registered": true,
    "config": {
      "heartbeat_interval_sec": 30,
      "poll_interval_sec": 10
    }
  }
}
```

---

## 3.2 心跳上报

- **Method**: `POST`
- **Path**: `/api/v1/agent/heartbeat`

### Request
```json
{
  "device_id": "dev-001",
  "agent_id": "thin-agent-001",
  "request_id": "uuid-xxx",
  "timestamp": 1760000000,
  "status": "online",
  "runtime": {
    "uptime_sec": 3600,
    "mem_used_mb": 256,
    "cpu_load": 0.42,
    "task_running": 1,
    "task_queue": 3
  }
}
```

### Response
```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "next_heartbeat_sec": 30,
    "server_time": 1760000001
  }
}
```

---

## 3.3 拉取任务（Poll）

- **Method**: `POST`
- **Path**: `/api/v1/agent/tasks/poll`

### Request
```json
{
  "device_id": "dev-001",
  "agent_id": "thin-agent-001",
  "request_id": "uuid-xxx",
  "timestamp": 1760000000,
  "max_tasks": 1,
  "supported_actions": ["health_report", "collect_logs", "switch_mode", "capture_photo", "start_recording", "stop_recording", "fetch_capture_results"]
}
```

### Response（有任务）
```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "tasks": [
      {
        "task_id": "task-123",
        "trace_id": "trace-abc",
        "action": "collect_logs",
        "priority": "high",
        "timeout_sec": 60,
        "args": {
          "paths": ["/var/log/syslog"],
          "tail_lines": 200
        },
        "created_at": 1760000000,
        "idempotency_key": "task-123-v1"
      }
    ]
  }
}
```

### Response（无任务）
```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "tasks": []
  }
}
```

---

## 3.4 任务结果回传

- **Method**: `POST`
- **Path**: `/api/v1/agent/tasks/callback`

### Request
```json
{
  "device_id": "dev-001",
  "agent_id": "thin-agent-001",
  "request_id": "uuid-xxx",
  "timestamp": 1760000000,
  "task_id": "task-123",
  "trace_id": "trace-abc",
  "status": "completed",
  "duration_ms": 1520,
  "result": {
    "summary": "collected 200 lines",
    "artifact_refs": ["local://artifacts/task-123.log"]
  },
  "error": null
}
```

### 失败示例
```json
{
  "device_id": "dev-001",
  "agent_id": "thin-agent-001",
  "request_id": "uuid-xxx",
  "timestamp": 1760000000,
  "task_id": "task-123",
  "trace_id": "trace-abc",
  "status": "failed",
  "duration_ms": 800,
  "result": null,
  "error": {
    "code": "ACTION_TIMEOUT",
    "message": "collect_logs timeout"
  }
}
```

### Response
```json
{
  "code": 0,
  "message": "ack"
}
```

---

## 3.5 WebSocket 消息协议（当前实现，v0.2增强）

- **方向**：外部 -> agent（请求），agent -> 外部（响应）
- **传输**：`ws://<host>:<port>/ws`
- **鉴权**：当前 demo 阶段无 token（后续升级）

### 通用字段

请求与响应均建议带：
- `type`：消息类型
- `cmd_id`：命令 ID（客户端可传，服务端会透传；未传则自动补全）
- `trace_id`：链路追踪 ID（客户端可传，服务端会透传；未传则自动补全）

### 输入消息类型（外部 -> agent）

- `status`：读取 agent 状态
- `ping`：探活
- `chat`：对话输入（更新短期/长期记忆）
- `action`：即时动作执行
- `task_submit`：提交任务（支持 `idempotency_key`）
- `task_get`：查询单任务
- `task_list`：查询任务列表（支持过滤参数 `state` / `action`，响应回显 `filters`）
- `task_cancel`：取消任务
- `task_replay`：重放任务
- `task_audit`：查询任务状态迁移审计
- `memory_recent`：读取会话短期记忆
- `memory_history`：读取长期记忆历史（来自 `data/agent_memory.jsonl`）
- `memory_search`：按 query 检索长期记忆（支持 `limit`，返回命中列表）
- `memory_summary`：按 query 汇总长期记忆（返回命中统计、top sessions 与摘要）
- `event_recent`：读取近期事件窗口
- `metrics`：读取本地运行指标（请求量/类型分布/平均处理耗时）

### 输出消息类型（agent -> 外部）

- `hello`
- `status`
- `pong`
- `chat_result`
- `action_result`
- `task_submit_result`
- `task_get_result`
- `task_list_result`
- `task_cancel_result`
- `task_replay_result`
- `task_audit_result`
- `memory_recent_result`
- `memory_history_result`
- `memory_search_result`
- `memory_summary_result`
- `event_recent_result`
- `metrics_result`
- `error`

### 示例：task_submit（失败重试路径）

```json
{
  "type": "task_submit",
  "cmd_id": "cmd-001",
  "trace_id": "trace-001",
  "action": "not_allowed_action",
  "args": {},
  "idempotency_key": "idem-001"
}
```

```json
{
  "type": "task_submit_result",
  "cmd_id": "cmd-001",
  "trace_id": "trace-001",
  "task": {
    "task_id": "task-2",
    "state": "failed",
    "attempts": 4,
    "code": 24001,
    "message": "unsupported action"
  }
}
```

### 示例：task_audit

```json
{
  "type": "task_audit",
  "cmd_id": "cmd-010",
  "trace_id": "trace-001",
  "task_id": "task-2",
  "limit": 20
}
```

```json
{
  "type": "task_audit_result",
  "cmd_id": "cmd-010",
  "trace_id": "trace-001",
  "data": {
    "task_id": "task-2",
    "exists": true,
    "audit_has_entries": true,
    "limit_applied": 20,
    "audit_count_returned": 4,
    "audit_latest_audit_id": 12,
    "audit_latest_from_state": "retrying",
    "audit_latest_to_state": "failed",
    "audit_latest_transition": "retrying->failed",
    "audit_latest_is_terminal": true,
    "audit_latest_is_non_terminal": false,
    "audit_latest_is_active": false,
    "audit_latest_is_queued": false,
    "audit_latest_is_running": false,
    "audit_latest_is_retrying": false,
    "audit_latest_is_success": false,
    "audit_latest_is_failed": true,
    "audit_latest_is_cancelled": false,
    "audit_latest_is_done": true,
    "audit_latest_is_failure_terminal": true,
    "audit_latest_state_category": "terminal_failure",
    "audit_latest_has_started": true,
    "audit_latest_is_in_progress": false,
    "audit_latest_has_error": true,
    "audit_latest_attempts": 3,
    "audit_latest_code": 24001,
    "audit_latest_message": "unsupported action",
    "audit_latest_created_at": "2026-07-03T01:52:20Z",
    "audits": [
      {"from_state": "", "to_state": "queued"},
      {"from_state": "queued", "to_state": "running"},
      {"from_state": "running", "to_state": "retrying"},
      {"from_state": "retrying", "to_state": "failed"}
    ]
  }
}
```

### task_list limit 约束（v0.3.1）

- `limit <= 0` 时，服务端按默认 `20` 处理。
- `limit > 200` 时，服务端会钳制为 `200`，防止单次查询过大。
- 该约束与实现 `TaskEngine::list_tasks`、`AgentService(task_list)` 保持一致。

### task_audit limit 约束（v0.3.2）

- `limit <= 0` 时，服务端按默认 `50` 处理。
- `limit > 200` 时，服务端会钳制为 `200`，防止单次查询过大。
- 该约束与实现 `TaskEngine::list_task_audits`、`AgentService(task_audit)` 保持一致。

### memory_search / memory_summary（v0.6.39）

- `memory_search` 请求参数：
  - `query`（string，可空）：检索关键词；为空表示返回最近长期记忆。
  - `limit`（number，可选）：命中上限；`<=0` 默认 `20`，`>200` 钳制为 `200`。
- `memory_search_result` 返回：
  - `query`：回显查询词
  - `limit_applied`：生效 limit
  - `results`：命中记忆数组（v0.7.0 起含 `data/agent_memory.jsonl` 与 task/audit sqlite 聚合结果）
  - `result_size`：命中条数（等于 `results.size()`）
  - `results[*].source`：来源标记（`memory_jsonl` / `task_sqlite`）
  - `results[*].kind`（可选）：当 `source=task_sqlite` 时区分 `task` / `task_audit`

- `memory_summary` 请求参数：
  - `query`（string，可空）
  - `limit`（number，可选）：同 `memory_search`
- `memory_summary_result` 返回：
  - `query`：回显查询词
  - `limit_applied`：生效 limit
  - `hit_count`：命中条数
  - `top_sessions`：按命中次数聚合的会话列表（最多 3 个）
  - `source_counts`（v0.7.0）：按来源聚合的命中统计（如 `{"memory_jsonl":2,"task_sqlite":3}`）
  - `summary`：规则摘要文本（空命中时返回“未检索到匹配记忆。”）
  - `evidence_latest_basis`（v0.8.1）：当命中 `task_sqlite` 维护证据时返回 latest 依据对象，字段与 `chat->observation.evidence_latest_basis` 对齐（`task_id/kind/source/row_ref/priority/created_at/audit_id/reason/selected_at`）。
  - `memory_replay_hint`（v0.8.5）：latest 证据回放提示对象，结构为 `{row_ref,selected_at,reason,payload{type,query,limit}}`，用于前端一键回放。

### 服务层 limit 转发一致性（v0.3.4）

- `task_list` 与 `task_audit` 的 `limit` 参数由 `AgentService` 透传至 `TaskEngine`。
- 新增服务层单元测试覆盖 `limit=0`、负数 `limit`（`task_list=-5` / `task_audit=-7`）与 `limit=1000` 的边界输入，验证响应条目数与默认/钳制语义一致。

### 核心层 limit 负数边界一致性（v0.3.5）

- `TaskEngine::list_tasks` 新增单测覆盖 `limit=-3`，验证与 `limit=0` 的默认语义一致（默认按 `20` 条处理）。
- `TaskEngine::list_task_audits` 新增单测覆盖 `limit=-9`，验证与 `limit=0` 的默认语义一致（默认按 `50` 条处理）。
- 结合既有 `limit=1000` 钳制测试，确认核心层边界策略为：`limit <= 0` 使用默认值，`limit > 200` 钳制为 `200`。

### task_audit 缺失任务返回约束（v0.3.7）

- `task_audit` 返回结构固定为：`{"task_id":"<请求值>","exists":<bool>,"audits":[...]}`。
- 当 `task_id` 存在时：`exists=true`，`audits` 为该任务审计列表（可为空数组）。
- 当 `task_id` 不存在时：`exists=false`，`audits` 必须为空数组（不返回 `null`）。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在/缺失任务场景，确保核心实现与协议约束一致。

### task_audit 生效 limit 回显约束（v0.3.8）

- `task_audit` 返回结构新增 `limit_applied` 字段：`{"task_id":"<请求值>","exists":<bool>,"limit_applied":<int>,"audits":[...]}`。
- 当请求 `limit <= 0` 时：`limit_applied=50`（默认值）。
- 当请求 `limit > 200` 时：`limit_applied=200`（钳制值）。
- 其他合法范围请求保持透传（例如请求 `limit=10` 时，`limit_applied=10`）。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖 `0/负数/超大值/缺失任务` 场景，确保协议与实现一致。

### task_audit 返回条数回显约束（v0.3.9）

- `task_audit` 返回结构新增 `audit_count_returned` 字段：`{"task_id":"<请求值>","exists":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audits":[...]}`。
- `audit_count_returned` 必须等于当前响应中 `audits` 数组实际条数（即 `audits.size()`）。
- 当任务不存在时：`exists=false`、`audits=[]`，且 `audit_count_returned=0`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖默认/钳制/缺失任务场景，确保协议与实现一致。

### task_audit 最新状态回显约束（v0.3.10）

- `task_audit` 返回结构新增 `audit_latest_to_state` 字段：`{"task_id":"<请求值>","exists":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_to_state":"<string>","audits":[...]}`。
- 当 `audits` 非空时：`audit_latest_to_state` 必须等于 `audits[0].to_state`（按 `audit_id DESC` 返回时的最新状态）。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_to_state` 必须为 `""`（空字符串）。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。
- **v0.3.58 补充**：在 `TaskEngine::list_task_audits` 成功任务 + 显式 `limit=1` 场景新增核心层断言，验证 `audit_latest_to_state=success` 且与 `audits[0].to_state` 严格一致，确保最新状态回显字段在成功终态路径稳定可判定。
- **v0.3.59 补充**：在 `AgentService(task_audit)` 成功任务 + 显式 `limit=1` 场景新增服务层断言，验证协议层 `audit_latest_to_state=success` 且与 `audits[0].to_state` 严格一致，确保服务层回包与核心层审计记录一致。

### 云模式 chat 行为约束（v0.3.62，历史）

- `mode=cloud|auto` 且密钥可用时，`chat_result` 允许走真实云端调用链路：
  - 成功：`mode_used=cloud`，并返回 `cloud_http_status`（2xx）与云端文本（`text`）。
  - 失败且 `fallback=offline`：`mode_used=offline-fallback`，`fallback_reason=cloud_call_failed`，并回显 `cloud_error`。
  - 失败且 `fallback!=offline`（如 `fallback=none`）：返回 `mode_used=cloud-error`，并回显 `cloud_http_status` 与 `cloud_error`，不进入离线兜底。
- 当配置 `api_key_env` 但环境变量缺失时，必须离线回退：
  - `mode_used=offline-fallback`
  - `fallback_reason=missing_api_key`
- 当前实现兼容 OpenAI Chat Completions 形态（`POST /chat/completions`），可通过 `api_base` 指向兼容网关或本地 mock。
- 本仓已新增 `tests/demo/mock_openai_server.py` + `tests/demo/e2e_cloud_chat.py` 用于“真实 HTTP 调用闭环”验证。
- **v0.3.62 补充（历史）**：新增 `AgentService` 单测覆盖 `fallback=none` 的 `cloud-error` 路径，确保 `mode_used/cloud_http_status/cloud_error` 三字段在“禁用离线回退”的失败场景可稳定判定。

### chat 文案配置策略升级（v0.6.13）

为落实“中文关键词与回复文案不硬编码 C++、需可配置”的约束，`chat_policy.json` 在 cloud/offline 回退文案上升级为**新命名优先 + 旧命名兼容**。

#### 1) 关键策略变化点

- offline 本地回显文案已配置化（不再固定硬编码）：
  - 新键：`cloud.offline_local_fallback`
- cloud 缺密钥回退文案统一为新命名：
  - 新键：`cloud.fallback_missing_key`
  - 兼容旧键：`cloud.missing_key_fallback`
- cloud 调用失败且允许离线回退文案统一为新命名：
  - 新键：`cloud.fallback_call_failed`
  - 兼容旧键：`cloud.call_failed_fallback`
- cloud 调用失败且不允许离线回退文案统一为新命名：
  - 新键：`cloud.error_call_failed_no_fallback`
  - 兼容旧键：`cloud.call_failed_no_fallback`

#### 2) 取值优先级（必须遵守）

- `offline` 分支回显：
  1. `cloud.offline_local_fallback`
  2. `cloud.local_offline_fallback`（历史兼容）
  3. 内置默认文案
- `missing_api_key` 分支：
  1. `cloud.fallback_missing_key`
  2. `cloud.missing_key_fallback`（历史兼容）
  3. 内置默认文案
- `cloud_call_failed + fallback=offline` 分支：
  1. `cloud.fallback_call_failed`
  2. `cloud.call_failed_fallback`（历史兼容）
  3. 内置默认文案
- `cloud_call_failed + fallback!=offline` 分支：
  1. `cloud.error_call_failed_no_fallback`
  2. `cloud.call_failed_no_fallback`（历史兼容）
  3. 内置默认文案

#### 3) 配置与生效说明（使用手册）

1. 准备 policy 文件（可复用 `config/chat_policy.json`）：

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

2. 通过环境变量指定策略文件路径：

```bash
export THIN_AGENT_CHAT_POLICY_PATH=/absolute/path/to/chat_policy.json
```

3. 重启服务进程后生效（进程启动时读取配置路径）。

4. 若需灰度兼容旧版本，可同时保留旧键（`cloud.missing_key_fallback` 等）；新版本会优先读取新键。

#### 4) 模板变量约束

- 以上 fallback 文案中，除 `cloud.error_call_failed_no_fallback` 外，均建议包含 `{text}` 占位符用于输入回显。
- 缺失模板键时，系统按“新键 -> 兼容旧键 -> 默认文案”兜底，不会因配置缺项导致崩溃。

### chat_result 分层决策字段约束（v0.3.80）

为对齐“分层智能体决策引擎”（规则层 + 轻量语义路由层 + 策略层），`chat_result` 新增/约束如下：

- `decision`（object，推荐）：
  - `route`（string）：最终路由，如 `local_profile/local_status/local_memory_recent/local_memory_history/local_memory_search/local_memory_summary/local_task_inline/local_action/cloud_llm/local_reject`。
  - `reason`（string）：路由原因，如 `status_intent/task_expression_capture_photo/user_asks_agent_identity`。
  - `intent`（string，可选）：语义路由意图标签（如 `profile/status/task_capture`）。
  - `confidence`（number，可选）：意图置信度，范围 `[0,1]`。
  - `policy`（string，可选）：策略层决策结果（如 `execute/clarify/fallback_cloud/reject`）。
  - `slots`（object，可选）：结构化补充参数（如 `action/task_id`）。
- `intent_backend`（string，可选）：意图推理后端标识（如 `onnx`/`tflite`/`cloud`/`cloud-strategy`），建议默认本地 ONNX/TFLite。
- `observation.cloud_policy`（object，可选）：当 `intent_backend=cloud-strategy` 时，回显云端策略建议（本地已解析后的结构化结果，含 `strategy/intent/confidence/risk/response_draft/parser_mode`）。
- `observation.cloud_task_pipeline_contract`（object，可选，v0.6.22）：云策略中 `task_pipeline` 契约校验观测。
  - `valid`（bool）
  - `step_count`（number）
  - `errors`（array[string]）
- `observation.pipeline_execution`（object，可选，v0.6.24）：当 `task_pipeline` 执行时返回执行明细。
  - `step_count`（number）
  - `completed`（number）
  - `failed`（bool）
  - `failed_at`（number；未失败时为 `-1`）
  - `steps`（array）每步包含 `index/action/submitted/success`，若已提交可含 `task_id/state/code/message`，若参数校验失败可含 `error`。
- `observation.pipeline_error`（object，可选，v0.6.31，主字段）：pipeline 统一错误对象（建议前端优先消费）。
  - `class`（string）：`engine|param|task`（无失败时为空串）
  - `code`（string）：`PIPELINE_ENGINE_UNAVAILABLE|PIPELINE_PARAM_INVALID|PIPELINE_TASK_FAILED`（无失败时为空串）
  - `failed_at`（number）：失败步序号；无失败时 `-1`
  - v0.6.34：success 场景也强制返回空对象（`class="", code="", failed_at=-1`）以稳定消费端结构。
- `observation.pipeline_error_class`（string，可选，兼容字段，v0.6.30）：pipeline 总体错误分类（`engine|param|task`）。
  - `engine`：task_engine 不可用（拒绝执行）
  - `param`：步骤参数校验失败
  - `task`：步骤执行失败（`state!=success`)
- `observation.pipeline_error_code`（string，可选，v0.6.28）：pipeline 统一错误码。
  - `PIPELINE_ENGINE_UNAVAILABLE`：task_engine 不可用
  - `PIPELINE_PARAM_INVALID`：步骤参数校验失败（`error_class=param`）
  - `PIPELINE_TASK_FAILED`：步骤执行失败（`error_class=task`）
- `decision.reason`（v0.6.35）：当 `decision.route=local_task_pipeline` 时统一为稳定枚举。
  - 失败：使用与 `observation.pipeline_error_code` 一致的标准错误码。
  - 成功：`PIPELINE_EXECUTED`。
- `decision_trace` 的 `policy.input`（v0.6.32）：当 `route=local_task_pipeline` 时，新增 `pipeline_error_code` 字段（失败时为标准错误码，成功时为空串），用于链路审计与前端追踪。
  - v0.6.33：覆盖 execute/reject 全分支（含 no-engine 拒绝分支）。
- v0.6.36 实现约束：`pipeline_error_code_from_class` + `pipeline_decision_reason` + `pipeline_error_object` 三个辅助函数统一映射来源，避免分支硬编码漂移。
- v0.6.37：`decision_trace.policy.input` 在 pipeline execute/reject 分支统一输出 `{route,policy,failed,completed,pipeline_error_code}`。
- v0.6.38：新增 `observation.pipeline_outcome={failed,reason,error}` 统一结论对象，`decision.reason` 与 `pipeline_error` 从该对象派生；保持 `pipeline_error/pipeline_error_class/pipeline_error_code` 兼容字段不变。

### chat 记忆检索/摘要本地路由（v0.6.40）

- 当 `chat` 文本命中 `memory_search` 关键词（默认：`记忆检索`/`memory_search`）时：
  - `decision.route=local_memory_search`
  - `decision.intent=memory_search`
  - `decision.policy=execute`
  - `observation` 直接回显 `memory_search_result` 结构（含 `query/limit_applied/results/result_size`）
- 当 `chat` 文本命中 `memory_summary` 关键词（默认：`记忆摘要`/`memory_summary`）时：
  - `decision.route=local_memory_summary`
  - `decision.intent=memory_summary`
  - `decision.policy=execute`
  - `observation` 直接回显 `memory_summary_result` 结构（含 `query/limit_applied/hit_count/top_sessions/summary`）
- query 提取：默认从关键词后的尾部文本提取，去除常见口语前缀（如 `请/帮我/做/一下`），空 query 允许并按全量检索语义执行。
- limit 提取：若 query 尾部存在整数（如 `记忆检索 xxx 5`），将解析为 `limit`（范围 `1..200`，超界钳制；缺省为 `20`）。
- `decision_trace.policy.input`：在上述两类路由中补充 `query/limit` 字段，便于前端与审计链路观测。
- v0.6.42：ws_agent 前端在“可读轨迹块（非 JSON 区）”新增 memory 行：`[memory] query=... · limit=...`；仅在显示决策轨迹时渲染，保持默认隐藏/可切换语义不变。
- v0.8.4：在上述可读轨迹块继续补充 latest 依据行：当 `observation.evidence_latest_basis` 存在时，新增 `[memory-latest] row_ref=... · selected_at=... · reason=...`，用于快速回放定位与判定依据审阅。
- v0.7.1：当 chat 路由到 `local_memory_search/local_memory_summary` 且命中 `task_sqlite` 证据时，`text` 优先返回“维护证据（task/audit）”摘要（包含 query/limit 与关键样本），用于维护专家风格输出；`observation` 契约保持不变。
- v0.7.2：在 v0.7.1 基础上，`text` 统一为两行模板：`维护结论：...` + `证据点：...`，优先突出可审阅结论与关键证据点；不新增协议字段。
- v0.7.3：`证据点` 样本排序固定为状态优先（failed > cancelled > other），并限制 Top2 展示，减少输出抖动；不改变 `observation`/`decision_trace` 字段。
- v0.7.4：`证据点` 样本按 `task_id` 去重；同 task 多命中时优先保留失败/取消态并保留最新高优条目，进一步增强维护诊断可读性；协议字段不变。
- v0.7.5：在 v0.7.4 基础上将“最新”收敛为显式时间语义：同优先级下按 `created_at` 降序优先，若时间并列再按 `audit_id` 降序；`证据点 #1` 追加 `[latest]` 标记以显式可见，协议字段不变。
- v0.7.6：当 chat 路由到 `local_memory_search` 且命中维护证据时，在 `observation` 增加 `evidence_latest_basis={task_id,kind,priority,created_at,audit_id}`，用于 latest 判定依据可审计化。
- v0.7.7：latest 比较规则单点化（统一 helper）；`证据点 #1 [latest]` 与 `observation.evidence_latest_basis` 共用同一判定逻辑，降低分支漂移风险。
- v0.7.8：`observation.evidence_latest_basis` 增加 `source/row_ref`；同时在 `memory_search_result.results[*]` 增加 `row_ref`，支持 latest 证据可回放与唯一定位。
- v0.7.9：latest 排序增加固定 tie-break（`kind` + `task_id`），并新增重复请求稳定性断言，确保同输入多次请求 `#1 [latest]` 与 `evidence_latest_basis` 完全一致。
- v0.8.0：`evidence_latest_basis` 契约增强，新增 `reason=priority+recency+tie_break` 与 `selected_at` 字段，明确 latest 判定来源并提升审计可读性。
- v0.8.1：`memory_summary_result` 新增 `evidence_latest_basis`，使 `memory_summary` 与 `memory_search` 在 latest 判定依据上完全对齐。
- v0.8.2：新增配置模板键 `memory.maintenance_conclusion / memory.maintenance_evidence / memory.maintenance_summary_evidence`，维护两行文案可配置（缺省仍走原有内置文案，保持兼容）。
- v0.8.3：补强 `memory_summary_result.evidence_latest_basis` 契约回归断言，强制校验 `reason=priority+recency+tie_break` 与非空 `selected_at`，确保 summary 路径 latest 判定稳定可审计。
- v0.8.4：ws 可读轨迹在 memory 路由下新增 latest 行：`[memory-latest] row_ref=... · selected_at=... · reason=...`（仅在显示决策轨迹时渲染）；并同步 smoke 用例校验前端钩子存在性。
- v0.8.5：`chat` 的 memory 本地路由在 `observation` 增加 `memory_replay_hint={row_ref,selected_at,reason,payload{type,query,limit}}`，其中 `payload` 直接可用于回发 `memory_search/memory_summary` 请求，实现 latest 证据可回放契约。
- v0.8.6：ws 前端新增 `buildMemoryReplayPayloadFromLatest(...)` 并在 memory 路由消息操作区插入“回放该条记忆”按钮；优先复用 `observation.memory_replay_hint.payload`，缺失时回退到 `evidence_latest_basis.row_ref` 构造安全默认 payload，并由 smoke 用例校验钩子存在。
- v0.8.7：ws 前端新增 `detectMemoryReplayHit(...)` 与 `pendingMemoryReplayRowRef`，在回放请求后基于 `observation.evidence_latest_basis`/`observation.results[*].row_ref` 做命中判定；命中时增加可读轨迹行 `[memory-replay] hit row_ref=...`，并在消息气泡显示“已回放命中：row_ref=...”高亮提示。
- v0.8.13：结构化回包（`memory_summary_result/memory_search_result`）在开启决策轨迹时补齐可读轨迹行：`[memory] query=... · limit=...` 与回放命中行 `[memory-replay] hit row_ref=...`，使回放后的 B 用例观测形态与 chat_result 路径一致。
- v0.8.11：修复 memory_summary_result 回放命中判定缺口：`detectMemoryReplayHit(...)` 增加 `observation.query == pendingReplayQuery` 兜底规则，在摘要结果未返回 `results/evidence_latest_basis` 时仍可触发回放命中可见性。
- v0.8.10：修复“回放该条记忆”命中可见性在 query 兜底场景失效的问题：动作项新增 `replayRowRef`（来自 payload.query）并在点击时写入 `pendingMemoryReplayRowRef`，确保回放返回后稳定触发 `[memory-replay] hit row_ref=...` 与“已回放命中”提示。
- v0.8.9：ws 前端在 memory 路由消息操作区补充 replay payload 兜底构造：当 `observation.memory_replay_hint` 缺失时，基于可读轨迹中的 `memory.query/limit` 自动生成 `{type,query,limit}` 并仍展示“回放该条记忆”按钮，保证 memory_summary/memory_search 场景下回放入口稳定可见。
- v0.8.8：ws 前端新增证据列表提取与定位能力：`extractMemoryEvidenceRows(...)`、`scrollToEvidenceRow(...)`、`flashEvidenceRow(...)`；支持点击证据行定位，且回放命中后自动滚动并闪烁高亮命中行，形成“回放→命中→定位”闭环。
- `observation.pipeline_execution.rollback`（object，可选，v0.6.26）：rollback hook 观测。
  - `enabled`（bool）：配置开关 `pipeline_enable_rollback_hook`
  - `triggered`（bool）：失败且存在已完成步骤时是否触发回滚
  - `attempted_steps`（number）：可回滚步数
  - `executed_steps`（number）：实际执行的回滚步数
  - `status`（string）：`done|failed|skipped`
  - `steps`（array）：回滚执行明细（action/task_id/state/success/code/message）
  - v0.6.27 白名单补偿映射：`start_recording->stop_recording`、`stop_recording->start_recording`
- `steps[*].error_class`（string，可选，v0.6.25）：步骤级错误分类（`param|task`）
- `observation.risk_gate`（object，可选）：高风险拒绝门控观测（如 `{"risk":"high","blocked":true}`）。
- `tool_calls`（array，推荐）：本次执行过的本地工具链轨迹（如 `cloud_llm -> task_submit -> task_get`）。
- `observation`（object，可选）：本地观测快照（status/task/memory/event 等）。
- `decision_trace`（array，可选）：策略可观测轨迹，每项建议包含：
  - `layer`（`rules|intent|policy|llm|probe`）
  - `input`（string/object）
  - `output`（string/object）
  - `ts`（string，毫秒时间戳字符串；v0.3.80 起由服务端统一补全，不能为空）
- `observation.complex_intent_probe`（object，可选，v0.6.23）：复杂意图规则探针。
  - `detected`（bool）
  - `markers`（array）
- `observation.cloud_complex_intent_gate`（object，可选，v0.6.23）：复杂意图强制上云开关观测。
  - `enabled`（bool）：配置开关 `complex_intent_force_cloud`
  - `detected`（bool）：探针是否命中
  - `forced`（bool）：是否触发强制上云路由

兼容性说明：
- 历史 `chat_result` 未携带 `intent/confidence/decision_trace` 时视为兼容旧版本，不应作为解析失败。
- 新版本前端应优先渲染 `text`，并在调试区展示 `decision/tool_calls/decision_trace`。

### chat_result 示例：本地路由（v0.3.80）

```json
{
  "type": "chat_result",
  "cmd_id": "cmd-201",
  "trace_id": "trace-201",
  "mode_used": "local-agent",
  "text": "已通过 Agent 任务引擎执行拍照任务。task_id=task-2，当前状态=success。",
  "decision": {
    "route": "local_task_inline",
    "reason": "task_expression_capture_photo",
    "intent": "task_capture",
    "confidence": 0.96,
    "policy": "execute"
  },
  "tool_calls": [
    {"tool": "task_submit", "action": "capture_photo", "idempotency_key": "chat-inline-ws-1-5-capture_photo"},
    {"tool": "task_get", "task_id": "task-2"}
  ],
  "observation": {
    "inline_task": {"action": "capture_photo", "task_id": "task-2", "state": "success"}
  },
  "decision_trace": [
    {"layer": "rules", "input": "帮我拍一张照并告诉我结果", "output": "match:task_expression_capture_photo", "ts": "2026-07-03T08:00:00Z"},
    {"layer": "policy", "input": {"risk": "medium", "tool": "task_engine"}, "output": "execute", "ts": "2026-07-03T08:00:00Z"}
  ]
}
```

### chat_result 示例：云回退（v0.3.80）

```json
{
  "type": "chat_result",
  "cmd_id": "cmd-202",
  "trace_id": "trace-202",
  "mode_used": "local-agent",
  "intent_backend": "cloud-strategy",
  "cloud_http_status": 200,
  "text": "[端云协同-本地裁决] 已按本地策略返回状态信息。",
  "decision": {
    "route": "local_status",
    "reason": "cloud_strategy_match_local_status",
    "intent": "status",
    "confidence": 0.91,
    "policy": "execute"
  },
  "observation": {
    "cloud_policy": {
      "strategy": "answer_direct",
      "intent": "status",
      "confidence": 0.91,
      "local_route_hint": "local_status",
      "risk": "low",
      "parser_mode": "json"
    }
  },
  "tool_calls": [
    {"tool": "cloud_llm", "provider": "openai-compatible", "model": "mock-model"},
    {"tool": "status"}
  ],
  "decision_trace": [
    {"layer": "intent", "input": "route-s-demo", "output": {"intent": "general_query", "confidence": 0.40}, "ts": "2026-07-03T08:01:00Z"},
    {"layer": "llm", "input": "cloud_strategy", "output": {"strategy": "answer_direct", "local_route_hint": "local_status"}, "ts": "2026-07-03T08:01:01Z"},
    {"layer": "policy", "input": {"route_hint": "local_status", "risk": "low"}, "output": "execute_local_status", "ts": "2026-07-03T08:01:01Z"}
  ]
}
```

### chat_result 示例：高风险拒绝（v0.3.80）

```json
{
  "type": "chat_result",
  "mode_used": "local-agent",
  "intent_backend": "cloud-strategy",
  "cloud_http_status": 200,
  "text": "[端云协同-本地裁决] 当前请求被本地策略拒绝执行：存在高风险/敏感操作。请改为更具体且低风险的目标。",
  "decision": {
    "route": "local_reject",
    "reason": "mock_policy_high_risk_reject",
    "intent": "dangerous_action",
    "confidence": 0.95,
    "policy": "reject"
  },
  "observation": {
    "cloud_policy": {
      "strategy": "reject",
      "risk": "high",
      "parser_mode": "json"
    },
    "risk_gate": {
      "risk": "high",
      "blocked": true
    }
  },
  "decision_trace": [
    {"layer": "intent", "input": "reject-high-risk-demo", "output": {"intent": "general_query", "confidence": 0.40}},
    {"layer": "llm", "input": "cloud_strategy", "output": {"strategy": "reject", "risk": "high"}},
    {"layer": "policy", "input": {"risk": "high", "strategy": "reject"}, "output": "reject"}
  ]
}
```

### metrics_result 延迟字段约束（v0.3.63）

- `metrics_result.data` 新增 `last_latency_ms` 字段，表示最近一次请求处理耗时（毫秒，浮点）。
- 该字段必须为非负数（`>=0`），并与现有 `total_requests/by_type/avg_latency_ms` 同时返回。
- 字段语义：
  - `last_latency_ms`：最近一次请求耗时
  - `avg_latency_ms`：启动以来平均请求耗时
- 已由 `AgentService(metrics)` 服务层单测覆盖，确保协议与实现一致。

### metrics_result 类型计数字段约束（v0.3.64，历史）

- `metrics_result.data` 新增 `type_count` 字段，表示 `by_type` 对象中的请求类型键数量（整型）。
- 该字段必须为正整数（`>=1`，当至少有一种请求被记录时），并与现有 `total_requests/by_type/last_latency_ms/avg_latency_ms` 同时返回。
- 字段语义：
  - `type_count`：当前 `by_type` 中的类型数（例如存在 `chat/ping/metrics` 三类时为 `3`）。
- 已由 `AgentService(metrics)` 服务层单测覆盖，确保协议与实现一致。

### metrics_result 事件窗口计数字段约束（v0.3.65）

- `metrics_result.data` 新增 `event_count` 字段，表示 `event_recent` 当前窗口中的事件条目数（整型）。
- 该字段必须为非负整数（`>=0`），并与现有 `total_requests/by_type/type_count/last_latency_ms/avg_latency_ms` 同时返回。
- 字段语义：
  - `event_count`：当前事件窗口中保留的事件数量（与 `event_recent_result.event_size` 统计口径一致）。
- 已由 `AgentService(metrics)` 服务层单测覆盖，确保协议与实现一致。

### metrics_result 事件窗口容量字段约束（v0.3.66）

- `metrics_result.data` 新增 `event_window_limit` 字段，表示事件窗口上限容量（整型，当前实现固定为 `32`）。
- 该字段必须为正整数（`>=1`），并与现有 `total_requests/by_type/type_count/event_count/last_latency_ms/avg_latency_ms` 同时返回。
- 字段语义：
  - `event_window_limit`：`event_recent` 保留窗口最大容量。
  - `event_count`：当前窗口占用条目数，必须满足 `event_count <= event_window_limit`。
- 已由 `AgentService(metrics)` 服务层单测覆盖，确保协议与实现一致。

### metrics_result 类型自包含约束（v0.3.67）

- 当收到 `{"type":"metrics"}` 请求时，`metrics_result.data.by_type` 必须包含 `metrics` 键。
- `metrics_result.data.by_type.metrics` 必须为正整数（`>=1`），用于证明当前请求类型已计入类型统计字典。
- 该约束需与既有 `total_requests/by_type/type_count/event_count/event_window_limit/last_latency_ms/avg_latency_ms` 字段同时满足。
- 已由 `AgentService(metrics)` 服务层单测覆盖，确保协议与实现一致。

### metrics_result 记忆窗口容量字段约束（v0.3.68）

- `metrics_result.data` 新增 `memory_window_limit` 字段，表示会话短期记忆窗口上限容量（整型，当前实现固定为 `6`）。
- 该字段必须为正整数（`>=1`），并与既有 `total_requests/by_type/type_count/event_count/event_window_limit/last_latency_ms/avg_latency_ms` 同时返回。
- 字段语义：
  - `memory_window_limit`：`memory_recent` 会话短期记忆最大保留容量。
  - 对任意会话，`memory_recent_result.memory_size` 必须满足 `memory_size <= memory_window_limit`。
- 已由 `AgentService(metrics)` 服务层单测覆盖，确保协议与实现一致。

### metrics_result 活跃会话数字段约束（v0.3.69）

- `metrics_result.data` 新增 `active_session_count` 字段，表示当前活跃会话数量（整型，统计口径为 `session_chat_memory_` 映射中的会话数）。
- 该字段必须为非负整数（`>=0`），并与既有 `total_requests/by_type/type_count/event_count/event_window_limit/memory_window_limit/last_latency_ms/avg_latency_ms` 同时返回。
- 字段语义：
  - `active_session_count`：当前已打开且未关闭的会话数。
- 已由 `AgentService(metrics)` 服务层单测覆盖，确保协议与实现一致。

### task_list 过滤参数约束（v0.3.13）

- `task_list` 请求支持可选过滤参数：`state` 与 `action`。
- 当请求带 `state` 时，仅返回状态匹配的任务（例如 `failed` / `success`）。
- 当请求带 `action` 时，仅返回动作名匹配的任务（例如 `capture_photo`）。
- 当同时带 `state + action` 时，按 AND 语义过滤（两个条件同时满足）。
- `task_list_result` 响应新增 `filters` 字段回显本次实际使用过滤条件：
  - `filters.state`
  - `filters.action`
- 已由 `TaskEngine` / `AgentService` / `e2e_ws_v02.py` 三层测试覆盖，确保协议与实现一致。

### task_list 返回条数与生效 limit 回显约束（v0.3.57，历史）

- `task_list_result` 返回结构新增：
  - `limit_applied`：服务端实际生效的 limit（`<=0 -> 20`，`>200 -> 200`）。
  - `returned_count`：本次实际返回任务条数，必须等于 `tasks.size()`。
- 当请求 `limit=0` 或负数时：`limit_applied` 必须为 `20`。
- 当请求 `limit>200`（如 `1000`）时：`limit_applied` 必须为 `200`。
- 当请求携带过滤参数（`state` / `action`）时：`limit_applied` 与 `returned_count` 语义不变，`returned_count` 仍必须等于过滤后 `tasks.size()`。
- 已由 `AgentService` 单测与 `e2e_ws_v02.py` 联调覆盖，并在 `TaskEngine::list_tasks` 过滤路径补充 `limit=0/-5/1000` 边界单测，确保协议与核心实现一致。

### task_audit 最新错误码回显约束（v0.3.11）

- `task_audit` 返回结构新增 `audit_latest_code` 字段：`{"task_id":"<请求值>","exists":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_to_state":"<string>","audit_latest_code":<int>,"audits":[...]}`。
- 当 `audits` 非空时：`audit_latest_code` 必须等于 `audits[0].code`（按 `audit_id DESC` 返回时的最新审计错误码）。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_code` 必须为 `-1`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新消息回显约束（v0.3.12）

- `task_audit` 返回结构新增 `audit_latest_message` 字段：`{"task_id":"<请求值>","exists":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_to_state":"<string>","audit_latest_code":<int>,"audit_latest_message":"<string>","audits":[...]}`。
- 当 `audits` 非空时：`audit_latest_message` 必须等于 `audits[0].message`（按 `audit_id DESC` 返回时的最新审计消息）。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_message` 必须为 `""`（空字符串）。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新重试次数回显约束（v0.3.14）

- `task_audit` 返回结构新增 `audit_latest_attempts` 字段：`{"task_id":"<请求值>","exists":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_to_state":"<string>","audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audits":[...]}`。
- 当 `audits` 非空时：`audit_latest_attempts` 必须等于 `audits[0].attempts`（按 `audit_id DESC` 返回时的最新审计重试次数）。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_attempts` 必须为 `-1`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新审计时间回显约束（v0.3.15）

- `task_audit` 返回结构新增 `audit_latest_created_at` 字段：`{"task_id":"<请求值>","exists":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_to_state":"<string>","audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空时：`audit_latest_created_at` 必须等于 `audits[0].created_at`（按 `audit_id DESC` 返回时的最新审计时间）。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_created_at` 必须为 `""`（空字符串）。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新来源状态回显约束（v0.3.16）

- `task_audit` 返回结构新增 `audit_latest_from_state` 字段：`{"task_id":"<请求值>","exists":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空时：`audit_latest_from_state` 必须等于 `audits[0].from_state`（按 `audit_id DESC` 返回时的最新审计来源状态）。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_from_state` 必须为 `""`（空字符串）。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新审计ID回显约束（v0.3.17）

- `task_audit` 返回结构新增 `audit_latest_audit_id` 字段：`{"task_id":"<请求值>","exists":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空时：`audit_latest_audit_id` 必须等于 `audits[0].audit_id`（按 `audit_id DESC` 返回时的最新审计ID）。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_audit_id` 必须为 `-1`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 审计存在性回显约束（v0.3.18）

- `task_audit` 返回结构新增 `audit_has_entries` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空时：`audit_has_entries` 必须为 `true`。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_has_entries` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新状态迁移回显约束（v0.3.19）

- `task_audit` 返回结构新增 `audit_latest_transition` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空时：`audit_latest_transition` 必须等于 `audits[0].from_state + "->" + audits[0].to_state`（按 `audit_id DESC` 返回时的最新状态迁移）。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_transition` 必须为 `""`（空字符串）。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新终态判定回显约束（v0.3.20）

- `task_audit` 返回结构新增 `audit_latest_is_terminal` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且 `audit_latest_to_state` 属于终态集合 `{success, failed, cancelled}` 时：`audit_latest_is_terminal` 必须为 `true`。
- 当 `audits` 非空且 `audit_latest_to_state` 不属于终态集合时：`audit_latest_is_terminal` 必须为 `false`。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_terminal` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新重试态判定回显约束（v0.3.21）

- `task_audit` 返回结构新增 `audit_latest_is_retrying` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且 `audit_latest_to_state == "retrying"` 时：`audit_latest_is_retrying` 必须为 `true`。
- 当 `audits` 非空且 `audit_latest_to_state != "retrying"` 时：`audit_latest_is_retrying` 必须为 `false`。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_retrying` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新成功态判定回显约束（v0.3.22）

- `task_audit` 返回结构新增 `audit_latest_is_success` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且 `audit_latest_to_state == "success"` 时：`audit_latest_is_success` 必须为 `true`。
- 当 `audits` 非空且 `audit_latest_to_state != "success"` 时：`audit_latest_is_success` 必须为 `false`。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_success` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新失败态判定回显约束（v0.3.23）

- `task_audit` 返回结构新增 `audit_latest_is_failed` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_is_failed":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且 `audit_latest_to_state == "failed"` 时：`audit_latest_is_failed` 必须为 `true`。
- 当 `audits` 非空且 `audit_latest_to_state != "failed"` 时：`audit_latest_is_failed` 必须为 `false`。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_failed` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新取消态判定回显约束（v0.3.24）

- `task_audit` 返回结构新增 `audit_latest_is_cancelled` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_queued":<bool>,"audit_latest_is_running":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_is_failed":<bool>,"audit_latest_is_cancelled":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且 `audit_latest_to_state == "cancelled"` 时：`audit_latest_is_cancelled` 必须为 `true`。
- 当 `audits` 非空且 `audit_latest_to_state != "cancelled"` 时：`audit_latest_is_cancelled` 必须为 `false`。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_cancelled` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新运行态判定回显约束（v0.3.25）

- `task_audit` 返回结构新增 `audit_latest_is_running` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_queued":<bool>,"audit_latest_is_running":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_is_failed":<bool>,"audit_latest_is_cancelled":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且 `audit_latest_to_state == "running"` 时：`audit_latest_is_running` 必须为 `true`。
- 当 `audits` 非空且 `audit_latest_to_state != "running"` 时：`audit_latest_is_running` 必须为 `false`。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_running` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新排队态判定回显约束（v0.3.26）

- `task_audit` 返回结构新增 `audit_latest_is_queued` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_queued":<bool>,"audit_latest_is_running":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_is_failed":<bool>,"audit_latest_is_cancelled":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且 `audit_latest_to_state == "queued"` 时：`audit_latest_is_queued` 必须为 `true`。
- 当 `audits` 非空且 `audit_latest_to_state != "queued"` 时：`audit_latest_is_queued` 必须为 `false`。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_queued` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新错误存在性判定回显约束（v0.3.27）

- `task_audit` 返回结构新增 `audit_latest_has_error` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_queued":<bool>,"audit_latest_is_running":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_is_failed":<bool>,"audit_latest_is_cancelled":<bool>,"audit_latest_has_error":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且 `audit_latest_code > 0` 时：`audit_latest_has_error` 必须为 `true`。
- 当 `audits` 非空且 `audit_latest_code <= 0` 时：`audit_latest_has_error` 必须为 `false`。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_has_error` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新“已启动”判定回显约束（v0.3.28）

- `task_audit` 返回结构新增 `audit_latest_has_started` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_queued":<bool>,"audit_latest_is_running":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_is_failed":<bool>,"audit_latest_is_cancelled":<bool>,"audit_latest_has_started":<bool>,"audit_latest_has_error":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且最新审计 `to_state` 属于 `running/retrying/success/failed/cancelled` 时：`audit_latest_has_started` 必须为 `true`。
- 当最新审计 `to_state` 为 `queued`，或 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_has_started` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新“进行中”判定回显约束（v0.3.29）

- `task_audit` 返回结构新增 `audit_latest_is_in_progress` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_queued":<bool>,"audit_latest_is_running":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_is_failed":<bool>,"audit_latest_is_cancelled":<bool>,"audit_latest_has_started":<bool>,"audit_latest_is_in_progress":<bool>,"audit_latest_has_error":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且最新审计 `to_state` 属于 `running/retrying` 时：`audit_latest_is_in_progress` 必须为 `true`。
- 当最新审计 `to_state` 不属于 `running/retrying`，或 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_in_progress` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新“完成态”判定回显约束（v0.3.30）

- `task_audit` 返回结构新增 `audit_latest_is_done` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_queued":<bool>,"audit_latest_is_running":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_is_failed":<bool>,"audit_latest_is_cancelled":<bool>,"audit_latest_is_done":<bool>,"audit_latest_has_started":<bool>,"audit_latest_is_in_progress":<bool>,"audit_latest_has_error":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且最新审计 `to_state` 属于 `success/failed/cancelled` 时：`audit_latest_is_done` 必须为 `true`。
- 当最新审计 `to_state` 不属于 `success/failed/cancelled`，或 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_done` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新“失败终态”判定回显约束（v0.3.31）

- `task_audit` 返回结构新增 `audit_latest_is_failure_terminal` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_non_terminal":<bool>,"audit_latest_is_queued":<bool>,"audit_latest_is_running":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_is_failed":<bool>,"audit_latest_is_cancelled":<bool>,"audit_latest_is_done":<bool>,"audit_latest_is_failure_terminal":<bool>,"audit_latest_has_started":<bool>,"audit_latest_is_in_progress":<bool>,"audit_latest_has_error":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且最新审计 `to_state` 属于 `failed/cancelled` 时：`audit_latest_is_failure_terminal` 必须为 `true`。
- 当最新审计 `to_state` 不属于 `failed/cancelled`，或 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_failure_terminal` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新“非终态”判定回显约束（v0.3.32）

- `task_audit` 返回结构新增 `audit_latest_is_non_terminal` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_non_terminal":<bool>,"audit_latest_is_queued":<bool>,"audit_latest_is_running":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_is_failed":<bool>,"audit_latest_is_cancelled":<bool>,"audit_latest_is_done":<bool>,"audit_latest_is_failure_terminal":<bool>,"audit_latest_has_started":<bool>,"audit_latest_is_in_progress":<bool>,"audit_latest_has_error":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且最新审计 `to_state` 属于 `queued/running/retrying` 时：`audit_latest_is_non_terminal` 必须为 `true`。
- 当最新审计 `to_state` 不属于 `queued/running/retrying`，或 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_non_terminal` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖存在任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新“活跃态”判定回显约束（v0.3.72）

- `task_audit` 返回结构新增 `audit_latest_is_active` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_non_terminal":<bool>,"audit_latest_is_active":<bool>,"audit_latest_is_queued":<bool>,"audit_latest_is_running":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_is_failed":<bool>,"audit_latest_is_cancelled":<bool>,"audit_latest_is_done":<bool>,"audit_latest_is_failure_terminal":<bool>,"audit_latest_has_started":<bool>,"audit_latest_is_in_progress":<bool>,"audit_latest_has_error":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 非空且最新审计 `to_state` 属于 `queued/running/retrying` 时：`audit_latest_is_active` 必须为 `true`。
- 当最新审计 `to_state` 不属于 `queued/running/retrying`，或 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_active` 必须为 `false`。
- 当前实现中，`audit_latest_is_active` 与 `audit_latest_is_non_terminal` 在语义上等价（用于提升协议可读性），即两字段必须始终相等。
- `audit_latest_is_active` 必须严格等价于布尔公式：`audit_latest_is_queued || audit_latest_is_running || audit_latest_is_retrying`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖成功任务、失败任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新“进行中态公式”回显约束（v0.3.73）

- `task_audit` 返回结构中的 `audit_latest_is_in_progress` 必须严格等价于布尔公式：`audit_latest_is_running || audit_latest_is_retrying`。
- 当最新审计 `to_state` 属于 `running/retrying` 时：`audit_latest_is_in_progress` 必须为 `true`；其余状态（含 `queued/success/failed/cancelled`）必须为 `false`。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_in_progress` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖成功任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新“完成态公式”回显约束（v0.3.74）

- `task_audit` 返回结构中的 `audit_latest_is_done` 必须严格等价于布尔公式：`audit_latest_is_success || audit_latest_is_failed || audit_latest_is_cancelled`。
- 当最新审计 `to_state` 属于 `success/failed/cancelled` 时：`audit_latest_is_done` 必须为 `true`；其余状态（含 `queued/running/retrying`）必须为 `false`。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_done` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖成功任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新“失败终态公式”回显约束（v0.3.75）

- `task_audit` 返回结构中的 `audit_latest_is_failure_terminal` 必须严格等价于布尔公式：`audit_latest_is_failed || audit_latest_is_cancelled`。
- 当最新审计 `to_state` 属于 `failed/cancelled` 时：`audit_latest_is_failure_terminal` 必须为 `true`；其余状态（含 `queued/running/retrying/success`）必须为 `false`。
- 当 `audits` 为空时（例如任务不存在或该任务尚无审计记录）：`audit_latest_is_failure_terminal` 必须为 `false`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖成功任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新状态分类回显约束（v0.3.33）

- `task_audit` 返回结构新增 `audit_latest_state_category` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_non_terminal":<bool>,"audit_latest_is_queued":<bool>,"audit_latest_is_running":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_is_failed":<bool>,"audit_latest_is_cancelled":<bool>,"audit_latest_is_done":<bool>,"audit_latest_is_failure_terminal":<bool>,"audit_latest_state_category":"<string>","audit_latest_has_started":<bool>,"audit_latest_is_in_progress":<bool>,"audit_latest_has_error":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audits` 为空时：`audit_latest_state_category` 必须为 `"none"`。
- 当 `audits` 非空且最新审计 `to_state` 属于 `queued/running/retrying` 时：`audit_latest_state_category` 必须为 `"non_terminal"`。
- 当 `audits` 非空且最新审计 `to_state == "success"` 时：`audit_latest_state_category` 必须为 `"terminal_success"`。
- 当 `audits` 非空且最新审计 `to_state` 属于 `failed/cancelled` 时：`audit_latest_state_category` 必须为 `"terminal_failure"`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖成功任务、失败任务与缺失任务场景，确保协议与实现一致。

### task_audit 最新状态等级回显约束（v0.3.53）

- `task_audit` 返回结构新增 `audit_latest_state_rank` 字段：`{"task_id":"<请求值>","exists":<bool>,"audit_has_entries":<bool>,"limit_applied":<int>,"audit_count_returned":<int>,"audit_latest_audit_id":<int>,"audit_latest_from_state":"<string>","audit_latest_to_state":"<string>","audit_latest_transition":"<string>","audit_latest_is_terminal":<bool>,"audit_latest_is_non_terminal":<bool>,"audit_latest_is_queued":<bool>,"audit_latest_is_running":<bool>,"audit_latest_is_retrying":<bool>,"audit_latest_is_success":<bool>,"audit_latest_is_failed":<bool>,"audit_latest_is_cancelled":<bool>,"audit_latest_is_done":<bool>,"audit_latest_is_failure_terminal":<bool>,"audit_latest_state_category":"<string>","audit_latest_state_rank":<int>,"audit_latest_has_started":<bool>,"audit_latest_is_in_progress":<bool>,"audit_latest_has_error":<bool>,"audit_latest_attempts":<int>,"audit_latest_code":<int>,"audit_latest_message":"<string>","audit_latest_created_at":"<string>","audits":[...]}`。
- 当 `audit_latest_state_category == "none"` 时：`audit_latest_state_rank` 必须为 `0`。
- 当 `audit_latest_state_category == "non_terminal"` 时：`audit_latest_state_rank` 必须为 `1`。
- 当 `audit_latest_state_category == "terminal_success"` 时：`audit_latest_state_rank` 必须为 `2`。
- 当 `audit_latest_state_category == "terminal_failure"` 时：`audit_latest_state_rank` 必须为 `3`。
- 已由 `TaskEngine` 与 `AgentService` 单测覆盖成功任务、失败任务与缺失任务场景，确保该字段与 `audit_latest_state_category` 映射语义一致。
- 服务层在显式 `limit=20` 的 `task_audit` 请求下，`limit_applied` 必须保持为 `20`（不回落为默认 `50`），并同时满足 `audit_latest_state_category="terminal_success"` 与 `audit_latest_state_rank=2` 的一致性约束。
- 服务层在显式 `limit=1` 的 `task_audit` 请求下，`limit_applied` 必须保持为 `1`、`audit_count_returned` 必须与 `audits.size()` 一致，且在成功任务场景继续满足 `audit_latest_state_category="terminal_success"` 与 `audit_latest_state_rank=2` 的一致性约束。
- 核心层在成功任务 + 显式 `limit=1` 的 `list_task_audits` 调用下，`limit_applied` 必须保持为 `1`、`audit_count_returned` 必须与 `audits.size()` 一致，且继续满足 `audit_latest_state_category="terminal_success"` 与 `audit_latest_state_rank=2` 的一致性约束。
- 核心层在成功任务 + 显式 `limit=1` 的 `list_task_audits` 调用下，必须满足 `audit_latest_is_done=true` 且 `audit_latest_is_failure_terminal=false`，确保终态布尔判定与 `terminal_success` 分类一致。
- 服务层在成功任务 + 显式 `limit=1` 的 `task_audit` 请求下，必须满足 `audit_latest_is_done=true` 且 `audit_latest_is_failure_terminal=false`，确保协议层返回的终态布尔判定与 `terminal_success` 分类一致。
- 核心层在成功任务 + 显式 `limit=1` 的 `list_task_audits` 调用下，必须满足 `audit_latest_is_terminal=true` 且 `audit_latest_is_non_terminal=false`，确保终态/非终态布尔判定与 `terminal_success` 分类一致。
- 服务层在成功任务 + 显式 `limit=1` 的 `task_audit` 请求下，必须满足 `audit_latest_is_terminal=true` 且 `audit_latest_is_non_terminal=false`，确保协议层回显与核心层终态/非终态布尔判定一致。
- 服务层在成功任务 + 显式 `limit=1` 的 `task_audit` 请求下，必须满足 `audit_latest_is_success=true` 且 `audit_latest_is_failed=false`，确保协议层成功/失败布尔判定与 `terminal_success` 分类一致。
- 核心层在成功任务 + 显式 `limit=1` 的 `list_task_audits` 调用下，必须满足 `audit_latest_is_success=true` 且 `audit_latest_is_failed=false`，确保核心层成功/失败布尔判定与 `terminal_success` 分类一致。
- 服务层在成功任务 + 显式 `limit=1` 的 `task_audit` 请求下，必须满足 `audit_latest_has_started=true` 且 `audit_latest_has_error=false`，确保协议层“已启动/有错误”布尔判定与 `terminal_success` 分类一致。
- 核心层在成功任务 + 显式 `limit=1` 的 `list_task_audits` 调用下，必须满足 `audit_latest_has_started=true` 且 `audit_latest_has_error=false`，确保核心层“已启动/有错误”布尔判定与 `terminal_success` 分类一致。
- 核心层在成功任务 + 显式 `limit=1` 的 `list_task_audits` 调用下，必须满足 `audit_latest_code=0`，确保 `terminal_success` 分类与 `audit_latest_has_error=false` 的错误码语义一致。
- 服务层在成功任务 + 显式 `limit=1` 的 `task_audit` 请求下，必须满足 `audit_latest_code=0`，确保协议层 `terminal_success` 返回与 `audit_latest_has_error=false` 的错误码语义一致。
- 服务层在成功任务 + 显式 `limit=1` 的 `task_audit` 请求下，必须满足 `audit_latest_to_state="success"`，确保协议层 `audit_latest_to_state` 与 `audit_latest_state_category="terminal_success"` 的映射语义一致。
- 服务层在成功任务 + 显式 `limit=1` 的 `task_audit` 请求下，必须满足 `audit_latest_transition == audits[0].from_state + "->" + audits[0].to_state`，确保协议层 `audit_latest_transition` 与最新审计迁移记录严格一致。
- 核心层在成功任务 + 显式 `limit=1` 的 `list_task_audits` 调用下，必须满足 `audit_latest_transition == audits[0].from_state + "->" + audits[0].to_state`，确保核心层 `audit_latest_transition` 与最新审计迁移记录严格一致。
- 服务层在成功任务 + 显式 `limit=1` 的 `task_audit` 请求下，必须满足 `audit_latest_from_state == audits[0].from_state`，确保协议层最新来源状态回显与最新审计记录严格一致。
- 服务层在成功任务 + 显式 `limit=1` 的 `task_audit` 请求下，必须满足 `audit_latest_audit_id == audits[0].audit_id`，确保协议层最新审计ID回显与最新审计记录严格一致。

### 示例：metrics

```json
{
  "type": "metrics",
  "cmd_id": "cmd-020",
  "trace_id": "trace-001"
}
```

```json
{
  "type": "metrics_result",
  "cmd_id": "cmd-020",
  "trace_id": "trace-001",
  "data": {
    "total_requests": 16,
    "by_type": {
      "status": 2,
      "chat": 4,
      "task_submit": 2,
      "metrics": 1
    },
    "last_latency_ms": 0.72,
    "avg_latency_ms": 0.84
  }
}
```

---

## 4. 错误码建议（MVP）

- `0`：成功
- `1001`：鉴权失败
- `1002`：签名错误
- `1003`：请求过期（timestamp 超窗）
- `2001`：参数错误
- `2002`：设备未注册
- `3001`：任务不存在
- `3002`：任务状态非法
- `5000`：服务端内部错误

Agent 本地执行错误（建议本地与回传统一）：
- `ACTION_NOT_ALLOWED`
- `ACTION_INVALID_ARGS`
- `ACTION_TIMEOUT`
- `ACTION_RUNTIME_ERROR`
- `NETWORK_UNREACHABLE`
- `PERSIST_FAILED`

---

## 5. 状态机建议（任务级）

`PENDING -> RUNNING -> COMPLETED`

异常分支：
- `RUNNING -> FAILED`
- `RUNNING -> TIMEOUT`
- `FAILED/TIMEOUT -> RETRY_WAIT -> RUNNING`
- 重试达到上限 -> `DEAD_LETTER`

---

## 6. 幂等与重试策略

1. `request_id` 全局唯一，服务端可用于去重
2. `task_id + idempotency_key` 保证同任务重复下发不重复执行副作用
3. 回传失败时本地落 SQLite，按指数退避重试
4. 退避建议：`base=1s, factor=2, jitter=20%, max=60s`

---

## 7. 安全建议（分阶段）

### MVP
- Header 携带 `Authorization: Bearer [token]`（HTTP 通道）
- 所有 HTTP 接口走 HTTPS
- `timestamp` 超时窗口校验（如 ±300s）
- 交互 WebSocket 通道不启用 token/WSS（与现网控制链路保持一致）

### 增强
- HMAC 签名（body + timestamp + nonce）
- Token 轮换
- 双向 TLS（mTLS）
- WebSocket 鉴权与 WSS 加密

---

## 8. 版本策略

- URL 显式版本：`/api/v1/...`
- 协议字段新增遵循“向后兼容”
- 不兼容变更走 `/api/v2/...`

---

## 9. 与架构文档关系

本文件为协议层草案，对应：
- `ARCHITECTURE.md` 中 Transport/Task Engine/Policy/Store 模块
- 后续实现前需补充：
  - Header 规范
  - 签名算法细节
  - 各 action 的参数 schema
  - 回传 artifact 存储策略
