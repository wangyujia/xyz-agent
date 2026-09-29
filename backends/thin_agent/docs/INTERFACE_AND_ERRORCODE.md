# thin_agent 接口与错误码规范（v0.1）

> 目标：定义 `IDeviceControl` 抽象接口（get/set/act/evt）及统一错误码，确保 x86/aarch64 可替换实现一致行为。

---

## 1. IDeviceControl 抽象接口

建议最小接口（C++ 语义草案）：

```cpp
struct DcResult {
  int code;                 // 统一错误码（见下文）
  std::string message;      // 可读说明
  nlohmann::json data;      // 结果数据
};

class IDeviceControl {
public:
  virtual ~IDeviceControl() = default;

  // 获取设备状态/属性
  virtual DcResult get(const std::string& key,
                       const nlohmann::json& args,
                       int timeout_ms) = 0;

  // 设置设备状态/属性
  virtual DcResult set(const std::string& key,
                       const nlohmann::json& value,
                       int timeout_ms) = 0;

  // 执行动作（action）
  virtual DcResult act(const std::string& action,
                       const nlohmann::json& args,
                       int timeout_ms) = 0;

  // 事件接口（固定操作集合）：sub/unsub/pub/poll
  virtual DcResult evt(const std::string& op,
                       const std::string& event,
                       const nlohmann::json& payload) = 0;
};
```

---

## 2. 首批 action 白名单

- `health_report`
- `collect_logs`
- `switch_mode`
- `capture_photo`
- `start_recording`
- `stop_recording`
- `fetch_capture_results`

说明：
- 以上 action 由 `policy` 做参数校验与准入。
- 最终执行通过 `IDeviceControl::act()` 路由到 real/mock 适配器。
- 首批真实 FDBus 联调服务固定为 `camera`（模式切换、拍照、录像、文件结果）。

---

## 3. 统一错误码（v0.1）

## 3.1 通用成功

- `0` `OK`

## 3.2 通用失败（1000 段）

- `1001` `INVALID_ARGUMENT` 参数非法
- `1002` `TIMEOUT` 执行超时
- `1003` `NOT_SUPPORTED` 能力不支持
- `1004` `BUSY` 设备忙
- `1005` `INTERNAL_ERROR` 内部错误

## 3.3 策略层（2000 段）

- `2001` `ACTION_NOT_ALLOWED` action 不在白名单
- `2002` `POLICY_DENIED` 风险策略拒绝
- `2003` `RATE_LIMITED` 触发限流

## 3.4 传输与网络（3000 段）

- `3001` `NETWORK_UNREACHABLE`
- `3002` `DNS_RESOLVE_FAILED`
- `3003` `HTTP_REQUEST_FAILED`
- `3004` `WS_DISCONNECTED`
- `3005` `REMOTE_5XX`

## 3.5 持久化（4000 段）

- `4001` `DB_OPEN_FAILED`
- `4002` `DB_WRITE_FAILED`
- `4003` `DB_READ_FAILED`
- `4004` `SERIALIZATION_FAILED`

## 3.6 设备控制/FDBus（5000 段）

- `5001` `DC_BACKEND_UNAVAILABLE`（如 FDBus 服务不可达）
- `5002` `DC_CALL_FAILED`
- `5003` `DC_RESPONSE_INVALID`
- `5004` `DC_EVENT_SUBSCRIBE_FAILED`

## 3.7 模型与推理（6000 段）

- `6001` `MODEL_DISABLED`（offline 模式下禁止）
- `6002` `MODEL_REQUEST_FAILED`
- `6003` `MODEL_TIMEOUT`
- `6004` `MODEL_BAD_RESPONSE`

---

## 3.8 任务引擎统一错误码域（v0.2增强）

针对 `TaskEngine` 执行路径，增加统一错误码映射域：

- `24xxx`：动作/策略相关映射错误
  - `24001`：unsupported action（由 action code `4001` 映射）
  - `24004`：capture not found（由 action code `4004` 映射）
  - `24008`：invalid mode（由 action code `4008` 映射）
- `25xxx`：执行器/后端内部错误
  - `25001`：executor unavailable
  - `25NNN`：由 `5000+` 类错误映射
- `26xxx`：任务生命周期控制
  - `26010`：task cancelled
- `29999`：未知错误兜底

说明：
- 该映射用于 `task_submit/task_get/task_list/task_audit` 返回中的 `task.code`。
- 原始 action 错误码保留在 `task.error.raw_code`（若有）。

---

## 4. FDBus 错误码映射原则

在 `FdbusDeviceControl` 中统一转换：

- 首批联调目标：`camera` 服务
- FDBus 服务未连接/断开 -> `5001`
- FDBus 调用返回失败 -> `5002`
- 返回包格式不符/字段缺失 -> `5003`
- 订阅事件失败 -> `5004`
- 底层超时 -> `1002`

上层模块不得直接使用 FDBus 原始错误码。

---

## 5. 返回体建议

```json
{
  "code": 0,
  "message": "ok",
  "data": {}
}
```

失败示例：

```json
{
  "code": 5001,
  "message": "fdbus service unavailable",
  "data": {
    "service": "ql-camera-svc"
  }
}
```

---

## 6. 与现有文档关系

- 架构：`ARCHITECTURE.md`
- 协议：`PROTOCOL.md`
- 目录边界：`PROJECT_STRUCTURE.md`
- 本文档：接口抽象与错误码基线
