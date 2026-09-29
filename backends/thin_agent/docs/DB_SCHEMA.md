# thin_agent SQLite 表结构草案（v0.1）

> 目标：定义 thin_agent 在嵌入式端最小可用的数据模型，支持任务执行、断网重传、审计追踪和本地记忆。

---

## 1. 设计原则

1. **最小够用**：优先支持 MVP 主链路（poll -> execute -> callback）
2. **可恢复**：进程重启后可恢复未完成任务
3. **可追踪**：所有关键操作有审计记录
4. **可扩展**：字段预留，后续可无痛演进

---

## 2. 数据库基础设置

- 数据库文件建议：`/data/agent/runtime/thin_agent.db`
- SQLite 建议 PRAGMA：
  - `PRAGMA journal_mode=WAL;`
  - `PRAGMA synchronous=NORMAL;`
  - `PRAGMA foreign_keys=ON;`
  - `PRAGMA busy_timeout=3000;`

---

## 3. 表结构定义

## 3.1 `tasks`（任务主表）

用途：保存任务生命周期状态，支持重启恢复。

```sql
CREATE TABLE IF NOT EXISTS tasks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id TEXT NOT NULL UNIQUE,
  trace_id TEXT,
  idempotency_key TEXT,

  action TEXT NOT NULL,
  priority TEXT DEFAULT 'normal',
  args_json TEXT NOT NULL,

  status TEXT NOT NULL,                 -- pending/running/completed/failed/timeout/retry_wait/dead_letter
  error_code TEXT,
  error_message TEXT,

  attempt_count INTEGER NOT NULL DEFAULT 0,
  max_retries INTEGER NOT NULL DEFAULT 3,
  next_retry_at INTEGER,                -- unix ts (sec)

  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  started_at INTEGER,
  finished_at INTEGER,

  source_json TEXT                      -- 原始下发任务快照
);
```

建议索引：

```sql
CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
CREATE INDEX IF NOT EXISTS idx_tasks_next_retry_at ON tasks(next_retry_at);
CREATE INDEX IF NOT EXISTS idx_tasks_trace_id ON tasks(trace_id);
```

---

## 3.2 `task_results`（任务结果表）

用途：保存执行结果与回传 payload，支持断网重传。

```sql
CREATE TABLE IF NOT EXISTS task_results (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id TEXT NOT NULL,
  trace_id TEXT,

  callback_status TEXT NOT NULL,        -- pending_sent/sent/acked/failed
  result_json TEXT,
  error_json TEXT,

  duration_ms INTEGER,
  payload_json TEXT NOT NULL,           -- 准备发给服务端的完整回传体

  send_attempts INTEGER NOT NULL DEFAULT 0,
  next_send_at INTEGER,
  last_send_error TEXT,

  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,

  FOREIGN KEY(task_id) REFERENCES tasks(task_id) ON DELETE CASCADE
);
```

建议索引：

```sql
CREATE INDEX IF NOT EXISTS idx_task_results_callback_status ON task_results(callback_status);
CREATE INDEX IF NOT EXISTS idx_task_results_next_send_at ON task_results(next_send_at);
CREATE INDEX IF NOT EXISTS idx_task_results_task_id ON task_results(task_id);
```

---

## 3.3 `audit_logs`（审计日志表）

用途：记录安全和关键行为，便于排障和合规追溯。

```sql
CREATE TABLE IF NOT EXISTS audit_logs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  event_type TEXT NOT NULL,             -- register/heartbeat/poll/task_start/task_finish/callback/policy_deny/...
  level TEXT NOT NULL,                  -- info/warn/error

  trace_id TEXT,
  task_id TEXT,
  request_id TEXT,

  action TEXT,
  message TEXT,
  detail_json TEXT,

  created_at INTEGER NOT NULL
);
```

建议索引：

```sql
CREATE INDEX IF NOT EXISTS idx_audit_logs_created_at ON audit_logs(created_at);
CREATE INDEX IF NOT EXISTS idx_audit_logs_event_type ON audit_logs(event_type);
CREATE INDEX IF NOT EXISTS idx_audit_logs_trace_id ON audit_logs(trace_id);
```

---

## 3.4 `kv_store`（通用键值配置/状态）

用途：保存轻量运行时状态（如注册态、上次成功心跳时间、token 版本）。

```sql
CREATE TABLE IF NOT EXISTS kv_store (
  k TEXT PRIMARY KEY,
  v TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);
```

典型 key：
- `agent.registered`
- `agent.last_heartbeat_at`
- `agent.last_poll_at`
- `agent.server_time_offset_sec`

---

## 3.5 `memory_events`（本地记忆事件，在线增强预留）

用途：保存近期关键事件，后续可用于 LLM 上下文摘要。

```sql
CREATE TABLE IF NOT EXISTS memory_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  category TEXT NOT NULL,               -- fault/recovery/config/network/task
  severity TEXT NOT NULL,               -- low/medium/high

  trace_id TEXT,
  task_id TEXT,
  title TEXT NOT NULL,
  content TEXT NOT NULL,
  tags_json TEXT,

  created_at INTEGER NOT NULL,
  expires_at INTEGER                    -- 可为空；用于TTL清理
);
```

建议索引：

```sql
CREATE INDEX IF NOT EXISTS idx_memory_events_category ON memory_events(category);
CREATE INDEX IF NOT EXISTS idx_memory_events_created_at ON memory_events(created_at);
CREATE INDEX IF NOT EXISTS idx_memory_events_expires_at ON memory_events(expires_at);
```

---

## 3.6 `task_audits`（任务迁移审计，当前代码已实现）

用途：记录任务状态迁移轨迹（from/to/attempt/code/message），用于排障与回放分析。

```sql
CREATE TABLE IF NOT EXISTS task_audits (
  audit_id INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id TEXT NOT NULL,
  from_state TEXT NOT NULL,
  to_state TEXT NOT NULL,
  attempts INTEGER NOT NULL,
  code INTEGER NOT NULL,
  message TEXT NOT NULL,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_task_audits_task ON task_audits(task_id, audit_id DESC);
```

补充说明（与当前 `TaskEngine` 一致）：
- `tasks.state` 当前实现使用：`queued/running/retrying/success/failed/cancelled`
- 幂等键：`tasks.idempotency_key`（唯一索引）
- 取消任务写入统一错误码：`26010`

---

## 4. 状态流与表交互

## 4.1 任务执行主流程

1. poll 拉到任务 -> 写入 `tasks(status=pending)`
2. 调度执行前 -> `tasks(status=running, started_at=...)`
3. 执行成功/失败 -> 更新 `tasks(status=completed/failed/timeout)`
4. 生成回传体 -> 插入 `task_results(callback_status=pending_sent)`
5. 回传成功 ack -> 更新 `task_results(callback_status=acked)`
6. 回传失败 -> 更新 `task_results(callback_status=failed, next_send_at=...)`，由重试器处理

## 4.2 重启恢复

启动时扫描：
- `tasks.status in ('running')` -> 标记为 `retry_wait` 或 `failed`（按策略）
- `task_results.callback_status in ('pending_sent','failed')` 且到达 `next_send_at` -> 重试发送

---

## 5. 数据保留与清理策略（建议）

- `audit_logs`：保留 7~30 天（按磁盘预算）
- `task_results`：ack 后保留 3~7 天
- `tasks`：completed/failed 超过 7~30 天归档或删除
- `memory_events`：按 `expires_at` 清理（默认 7 天）

建议每日低频清理任务（凌晨执行）。

---

## 6. 幂等与一致性建议

1. `task_id` 唯一，重复下发时不重复创建
2. 若 `task_id` 已存在且状态已终态（completed/failed），忽略重复执行
3. 回传以 `task_id + payload_hash` 去重（可在 `task_results` 增加唯一键）
4. 关键更新（任务状态迁移）使用事务

---

## 7. 版本演进建议

- 当前版本：`schema_version = 1`
- 在 `kv_store` 维护：`db.schema_version`
- 后续新增字段优先 `ALTER TABLE ADD COLUMN`
- 复杂变更走迁移脚本（`scripts/migrate_vX_to_vY.sql`）

---

## 8. 落地顺序建议

1. 先落 `tasks` + `task_results` + `audit_logs`
2. 再加 `kv_store`
3. 最后加 `memory_events`

MVP 最小闭环依赖前三张表即可。

---

## 9. 对应文档关系

- 总体架构：`ARCHITECTURE.md`
- 协议草案：`PROTOCOL.md`
- 模块边界：`PROJECT_STRUCTURE.md`
- 本文档：SQLite 数据模型基线
