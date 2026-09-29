# thin_agent cloud task_pipeline 执行说明（v0.1）

## 1. 目标

在 `cloud-strategy` 路径下，将已通过契约校验的 `task_pipeline[]` 从“只校验”升级为“按序执行”，并保持嵌入式优先的安全边界（白名单 action + 参数白名单 + fail-fast）。

---

## 2. 版本

- 契约校验引入：`v0.6.22`
- 执行引入：`v0.6.24`

---

## 3. 执行前提

仅当以下条件同时成立时执行 pipeline：

1. `intent_backend = cloud-strategy`
2. `task_pipeline` 为非空数组
3. `cloud_task_pipeline_contract.valid = true`
4. `task_engine` 可用

否则按既有策略降级（`local_clarify` 或 `local_task_pipeline` + reject 文案）。

---

## 4. 执行语义

### 4.1 顺序执行

按 step 顺序执行，每步映射到 `task_engine->submit_task(action, params, idem)`，随后 `task_get` 拉取状态。

### 4.2 fail-fast

任一步失败后立即停止后续步骤：

- 任务执行失败（`state != success`）
- 参数白名单校验失败（提交前）

### 4.3 action 白名单

- `capture_photo`
- `start_recording`
- `stop_recording`

### 4.4 params 白名单

- `capture_photo`: 无参数
- `start_recording`: 仅允许 `mode`，且取值仅 `normal|video|audio`
- `stop_recording`: 无参数

---

## 5. 回包字段

`chat_result.observation.pipeline_execution`:

```json
{
  "step_count": 2,
  "completed": 1,
  "failed": true,
  "failed_at": 2,
  "steps": [
    {
      "index": 1,
      "action": "capture_photo",
      "submitted": true,
      "success": true,
      "task_id": "task-12",
      "state": "success",
      "code": 0,
      "message": "ok"
    },
    {
      "index": 2,
      "action": "start_recording",
      "submitted": false,
      "success": false,
      "error": "invalid_mode:invalid"
    }
  ]
}
```

并保留：
- `observation.cloud_task_pipeline_contract`
- `observation.cloud_complex_intent_gate`

---

## 6. 决策输出

- 执行 route：`local_task_pipeline`
- 执行 reason：`cloud_strategy_match_task_pipeline`
- policy：`execute`

错误分类（v0.6.31）：
- `pipeline_error` 统一对象（推荐优先消费，主字段）：
  - `class`: `engine|param|task`
  - `code`: `PIPELINE_ENGINE_UNAVAILABLE|PIPELINE_PARAM_INVALID|PIPELINE_TASK_FAILED`
  - `failed_at`: 失败步号（无失败 `-1`）
  - v0.6.34：success 场景也固定返回空对象（`class="",code="",failed_at=-1`）
- 兼容字段保留：
  - `pipeline_error_class`：总体分类
  - `pipeline_error_code`：总体错误码
- `steps[*].error_class`：逐步错误分类（param/task）
  - `PIPELINE_ENGINE_UNAVAILABLE`
  - `PIPELINE_PARAM_INVALID`
  - `PIPELINE_TASK_FAILED`
- `decision.reason`（v0.6.35）：pipeline 失败时与 `pipeline_error_code` 保持一致；成功时为稳定枚举 `PIPELINE_EXECUTED`
- `decision_trace.policy.input.pipeline_error_code`（v0.6.32）：策略层输入附带标准错误码，便于审计与问题定位（成功为空串）
  - v0.6.33：覆盖 pipeline execute/reject 全分支（包括 no-engine reject）
- v0.6.36：实现层将 `class->code`、`failed->reason`、`error object` 三处映射统一到辅助函数（单点维护，行为不变）
- v0.6.37：`decision_trace.policy.input` 在 execute/reject 分支统一包含 `failed/completed/pipeline_error_code` 字段，前端审计字段结构固定。
- v0.6.38：新增 `observation.pipeline_outcome={failed,reason,error}` 统一结论对象；`decision.reason` 与 `pipeline_error` 由该对象单点派生，进一步降低分支遗漏风险。

rollback hook（v0.6.26，执行骨架）：
- 配置 `pipeline_enable_rollback_hook=true` 时，若 fail-fast 且已完成步数 `>0`：
  - 逆序扫描已成功步骤，补偿白名单映射（v0.6.27）：
    - `start_recording -> stop_recording`
    - `stop_recording -> start_recording`
  - 执行补偿任务并记录明细到 `pipeline_execution.rollback.steps[]`
  - 输出 `status=done|failed|skipped`
  - 输出 `attempted_steps` 与 `executed_steps`
- 仍保持 fail-safe：补偿失败不覆盖原始失败结论，仅补充观测与轨迹。

文案模板：
- `cloud.task_pipeline_reject`
- `cloud.task_pipeline_submitted`
- `cloud.task_pipeline_failed`

---

## 7. 测试要点

1. pipeline 成功两步：`completed=2, failed=false`
2. 第二步参数非法：`completed=1, failed=true, failed_at=2`
3. 非法 action：仍在契约层拦截，`cloud_policy_contract_violation`
4. 保留 `tool_calls` 中 cloud_llm + task_submit/task_get 轨迹
