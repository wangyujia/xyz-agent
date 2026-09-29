# thin_agent chat_policy 配置策略说明（v0.6.13）

> 目的：沉淀 v0.6.13 配置策略变化关键点，并给出可直接执行的配置/验证说明。

---

## 1. 背景

v0.6.13 继续推进 chat 文案配置化，重点覆盖 cloud/offline 回退文案，避免 C++ 中固定中文硬编码，满足“策略可配置、可灰度、可回滚”的工程要求。

---

## 2. 本次变化关键点

### 2.1 新键命名（优先）

- `cloud.offline_local_fallback`
- `cloud.fallback_missing_key`
- `cloud.fallback_call_failed`
- `cloud.error_call_failed_no_fallback`

### 2.2 兼容旧键（可保留）

- `cloud.missing_key_fallback`
- `cloud.call_failed_fallback`
- `cloud.call_failed_no_fallback`

### 2.3 关键策略

采用 **“新键优先 -> 旧键兼容 -> 默认文案兜底”** 的三层读取策略，确保：
1. 新版本可统一命名风格；
2. 旧配置不需要一次性迁移完；
3. 缺项不会导致服务异常。

---

## 3. 读取优先级（按分支）

### 3.1 `mode=offline` 本地回显

1. `cloud.offline_local_fallback`
2. `cloud.local_offline_fallback`（历史兼容）
3. 内置默认文案

### 3.2 cloud 缺密钥（`fallback_reason=missing_api_key`）

1. `cloud.fallback_missing_key`
2. `cloud.missing_key_fallback`
3. 内置默认文案

### 3.3 cloud 调用失败且允许离线回退（`fallback=offline`）

1. `cloud.fallback_call_failed`
2. `cloud.call_failed_fallback`
3. 内置默认文案

### 3.4 cloud 调用失败且不允许离线回退（`fallback!=offline`）

1. `cloud.error_call_failed_no_fallback`
2. `cloud.call_failed_no_fallback`
3. 内置默认文案

---

## 4. 配置文件写法

### 4.1 最小模板（建议）

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

### 4.2 变量约束

- `cloud.error_call_failed_no_fallback`：可不包含变量。
- 其余三项建议包含 `{text}`，用于输入回显。

---

## 5. 如何启用配置

1. 准备策略文件（可在 `config/chat_policy.json` 基础上改，或单独文件）。
2. 设置环境变量：

```bash
export THIN_AGENT_CHAT_POLICY_PATH=/absolute/path/to/chat_policy.json
```

3. 重启 thin_agent 进程。
4. 发送 chat 请求进行 smoke 验证。

> 注意：配置路径由服务进程在运行时读取；仅修改文件但不重启，可能仍使用旧缓存/旧进程。

---

## 6. 推荐验证清单

### 6.1 单测（已纳入）

- cloud 缺密钥回退命中新模板（`[CFG_CLOUD_MISSING_KEY]`）
- offline 分支命中新模板（`[CFG_OFFLINE_LOCAL]`）
- cloud 无回退错误命中新模板（`[CFG_CLOUD_ERROR]`）

### 6.2 运行时 smoke

- `mode=offline` 下发送任意 chat，检查 `text` 是否命中 `cloud.offline_local_fallback`。
- `mode=cloud + 缺 key`，检查 `fallback_reason=missing_api_key` 且文案命中 `cloud.fallback_missing_key`。
- `mode=cloud + fallback=none + 调用失败`，检查 `mode_used=cloud-error` 且文案命中 `cloud.error_call_failed_no_fallback`。

---

## 7. 运维建议

1. 迁移策略：先新增新键，不立刻删除旧键；稳定后再清理。
2. 文案改动流程：改配置 -> 重启服务 -> smoke 验证 -> 再回归测试。
3. 故障排查优先级：
   - 检查 `THIN_AGENT_CHAT_POLICY_PATH` 是否指向预期文件；
   - 检查进程是否重启；
   - 检查键名是否写错（特别是 `cloud.fallback_*` 新命名）。

---

## 8. 关联文件

- `src/core/AgentService.cpp`
- `config/chat_policy.json`
- `tests/unit/test_agent_service.cpp`
- `PROTOCOL.md`
- `PROJECT_STRUCTURE.md`
