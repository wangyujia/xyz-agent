# thin_agent 状态机草案（v0.1）

> 目标：定义 thin_agent 在 MVP 阶段的运行状态机与任务状态机，统一状态迁移、异常处理和恢复策略。

---

## 1. 状态机范围

本文件包含两层状态机：

1. **Agent 运行状态机（进程级）**
   - 描述 Agent 实例从启动到退出的整体状态
2. **Task 任务状态机（任务级）**
   - 描述单个任务从接收到完成/失败的生命周期

---

## 2. Agent 运行状态机（进程级）

## 2.1 状态定义

- `BOOTING`：启动中，初始化配置/日志/数据库/网络
- `REGISTERING`：尝试注册（可选）
- `ONLINE`：主循环运行（heartbeat + poll + execute + callback）
- `DEGRADED`：降级运行（网络不稳定/服务端不可达，但本地能力可继续）
- `RECOVERING`：恢复中（重连、补发、恢复任务）
- `STOPPING`：优雅退出（停止拉取、等待执行结束、刷盘）
- `STOPPED`：已退出

## 2.2 迁移规则

```text
BOOTING -> REGISTERING -> ONLINE
BOOTING -> ONLINE                 (若配置为跳过注册)
ONLINE -> DEGRADED               (连续网络失败阈值触发)
DEGRADED -> RECOVERING           (网络恢复探测成功)
RECOVERING -> ONLINE             (补发与状态恢复成功)
ONLINE/DEGRADED/RECOVERING -> STOPPING -> STOPPED
```

补充说明：
- WebSocket 服务随 `ONLINE` 状态进入可用态，`STOPPING` 时先停止接收新输入再排空队列。

## 2.3 关键事件触发

- `EV_INIT_OK`：初始化成功
- `EV_REGISTER_OK` / `EV_REGISTER_FAIL`
- `EV_NETWORK_FAIL_N`：连续 N 次网络失败
- `EV_NETWORK_BACK`：网络恢复
- `EV_RECOVER_DONE`：恢复流程完成
- `EV_SIGNAL_STOP`：收到退出信号（SIGTERM/SIGINT）

## 2.4 行为约束

1. `DEGRADED` 状态允许：
   - 本地任务执行（若任务来源于本地队列）
   - 回传缓存与重试计划更新
2. `DEGRADED` 状态禁止：
   - 高频无退避的网络重试
3. `STOPPING` 状态：
   - 不再拉取新任务
   - 尽量完成运行中任务或安全中断

---

## 3. Task 任务状态机（任务级，当前实现）

## 3.1 状态定义（当前代码）

- `queued`：任务已创建待执行
- `running`：执行中
- `retrying`：某次执行失败后进入重试阶段（含指数退避+jitter）
- `success`：执行成功（终态）
- `failed`：重试耗尽后失败（终态）
- `cancelled`：任务被取消（终态）

## 3.2 标准迁移（当前实现）

```text
queued -> running
running -> success
running -> retrying
retrying -> success
retrying -> retrying   (重试继续)
retrying -> failed     (超过重试上限)
queued/running/retrying -> cancelled
```

## 3.3 状态迁移合法性表

| 当前状态 | 允许迁移到 |
|---|---|
| queued | running, cancelled |
| running | success, retrying, cancelled |
| retrying | success, retrying, failed, cancelled |
| success | (终态) |
| failed | (终态) |
| cancelled | (终态) |

---

## 4. 回传状态机（callback 子状态）

任务执行状态与回传状态分离，避免“执行成功但结果未送达”丢失。

## 4.1 回传状态定义

- `PENDING_SENT`：待发送
- `SENT`：已发送待确认（可选）
- `ACKED`：服务端确认
- `SEND_FAILED`：发送失败待重试

## 4.2 迁移

```text
PENDING_SENT -> SENT -> ACKED
PENDING_SENT/SENT -> SEND_FAILED -> PENDING_SENT
```

建议：MVP 可简化为 `PENDING_SENT -> ACKED | SEND_FAILED`。

---

## 5. 错误处理策略

## 5.1 错误分类

1. **可重试错误**
   - 网络超时
   - DNS 失败
   - 服务端 5xx
   - 短暂资源不足

2. **不可重试错误**
   - action 不在白名单
   - 参数校验失败
   - 鉴权失败（除非 token 刷新成功）

3. **条件重试错误**
   - 429 限流（按服务端建议重试时间）
   - 401（尝试刷新凭据后重试一次）

## 5.2 重试策略（建议）

- 指数退避：`delay = min(max_delay, base * 2^attempt)`
- 抖动：`delay *= random(0.8, 1.2)`
- 默认参数：
  - `base=1s`
  - `max_delay=60s`
  - `max_retries=3`（可配置）

---

## 6. 崩溃恢复策略

## 6.1 启动恢复扫描

进程启动后：
1. 扫描 `tasks.status = RUNNING`
2. 按策略处理：
   - 标记为 `RETRY_WAIT`（推荐）
   - 或标记为 `FAILED`（若动作不可重入）
3. 扫描 `task_results.callback_status in (PENDING_SENT, SEND_FAILED)`
4. 到达 `next_send_at` 的记录进入补发队列

## 6.2 幂等保护

- 任务执行前检查 `task_id` 是否已终态
- 对具有副作用动作，要求 `idempotency_key`
- 重复下发同一 `task_id` 不应重复执行副作用

---

## 7. 并发与线程安全约束

1. 同一 `task_id` 同时只允许一个执行实例
2. 状态迁移必须在事务或原子更新中完成
3. 网络线程不得阻塞执行线程；执行线程不得阻塞网络事件循环
4. 所有跨线程队列必须有容量控制与背压策略

---

## 8. 可观测性要求（状态机相关）

每次状态迁移都应记录审计日志：
- `from_status`
- `to_status`
- `task_id`
- `trace_id`
- `reason`
- `timestamp`

建议日志事件类型：
- `task_transition`
- `task_retry_scheduled`
- `task_enter_dead_letter`
- `agent_state_transition`
- `ws_input_received`
- `ws_output_sent`

补充：
- 日志子系统支持外部回调挂接（用于转发到宿主系统或上层服务）。

---

## 9. MVP 最小实现建议

MVP 先实现：
1. Agent 状态：`BOOTING/ONLINE/DEGRADED/STOPPING/STOPPED`
2. Task 状态：`PENDING/RUNNING/COMPLETED/FAILED/TIMEOUT/RETRY_WAIT/DEAD_LETTER`
3. Callback 状态：`PENDING_SENT/ACKED/SEND_FAILED`
4. 启动恢复 + 重试 + 审计日志

`CANCELLED` 和复杂恢复策略可延后。

---

## 10. 与其他文档关系

- 架构：`ARCHITECTURE.md`
- 协议：`PROTOCOL.md`
- 目录结构：`PROJECT_STRUCTURE.md`
- 数据库：`DB_SCHEMA.md`
- 本文档：状态迁移与异常恢复规则
