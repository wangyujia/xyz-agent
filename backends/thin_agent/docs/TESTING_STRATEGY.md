# thin_agent 测试策略与架构隔离（v0.1）

> 目标：在不依赖设备原生能力（FDBus/硬件总线）的前提下，优先在 x86 Linux 完成单元测试与 demo 测试；随后在 aarch64 设备上进行真实联调。

---

## 1. 背景

thin_agent 运行在相机嵌入式设备（aarch64），并通过 FDBus 调用设备原生能力。
为了提升开发效率和测试覆盖率，必须保证：

- x86 Linux 环境可独立执行绝大多数测试
- 设备相关能力通过抽象隔离，避免直接耦合
- 同一套业务逻辑在 x86（mock）与 aarch64（real）两端复用

---

## 2. 核心策略

## 2.1 抽象先行

定义统一接口：`IDeviceControl`
- 上层（task/tools/policy）只依赖接口，不依赖 FDBus C API
- 真实实现：`FdbusDeviceControl`（仅设备运行时 aarch64）
- 测试实现：`FakeDeviceControl`（x86 单测与 demo）
- 可选：`MockDeviceControl`（用于更细粒度单元测试桩）

## 2.2 架构分流

- `x86_64`：默认不链接 `fdbus_clib`，使用 fake/stub
- `aarch64`：设备运行时可链接 `fdbus_clib`，启用真实设备调用

## 2.3 测试分层

1. **单元测试（x86）**
   - 目标：验证纯逻辑正确性
   - 约束：禁止依赖真实 FDBus/设备硬件

2. **Demo 测试（x86）**
   - 目标：验证主链路可跑通（poll -> execute -> callback）
   - 手段：mock transport + fake device control

3. **设备联调（aarch64）**
   - 目标：验证真实 FDBus 调用与服务协作
   - 依赖：设备原生服务可用（参考 leaptic_app 服务进程）
   - 依赖来源策略：x86 阶段不引入 FDBus 依赖；待 demo 完成后，在设备侧联调阶段再从 `leaptic_app` 工程对齐/引入 FDBus 头文件与库

---

## 3. CMake 约定（建议）

建议开关：
- `THIN_AGENT_TARGET_ARCH`：`x86_64` / `aarch64`
- `THIN_AGENT_WITH_FDBUS`：`ON` / `OFF`

推荐默认：
- x86: `THIN_AGENT_WITH_FDBUS=OFF`
- aarch64: `THIN_AGENT_WITH_FDBUS=ON`

伪代码示例：

```cmake
option(THIN_AGENT_WITH_FDBUS "Enable real FDBus adapter (device runtime only)" OFF)

if(THIN_AGENT_WITH_FDBUS)
  target_compile_definitions(thin_agent PRIVATE THIN_AGENT_WITH_FDBUS=1)
  target_sources(thin_agent PRIVATE src/fdbus/FdbusDeviceControl.cpp)
  target_link_libraries(thin_agent PRIVATE fdbus_clib)
else()
  target_compile_definitions(thin_agent PRIVATE THIN_AGENT_WITH_FDBUS=0)
  target_sources(thin_agent PRIVATE src/fdbus/FakeDeviceControl.cpp)
endif()
```

---

## 4. 目录与文件建议

与当前工程文档保持一致：

- `src/fdbus/IDeviceControl.h`
- `src/fdbus/FdbusDeviceControl.*`（真实实现，仅设备运行时）
- `src/fdbus/FakeDeviceControl.*`（x86 单测/demo 默认实现）
- `src/fdbus/MockDeviceControl.*`（可选：单测桩）
- `tests/unit/*`（逻辑层）
- `tests/integration/*`（链路层）
- `tests/demo/*`（演示场景，可选）

---

## 5. 验收标准

本节验收口径以 `PRD.md` 为准；本策略文档提供测试执行方法与覆盖建议。

## 5.1 x86 CI 必过

- 配置加载、状态机、策略校验、任务编排等核心单测通过
- 至少 1 条 demo 流程（mock）跑通
- 覆盖关键异常分支（超时、重试、回传失败）

## 5.2 aarch64 联调必过（含构建前置）

- 构建前置：`THIN_AGENT_WITH_FDBUS=ON` 时必须提供 `FDBUS_ROOT`
- 构建前置：CMake 校验通过（存在 `include/fdbus/fdbus_clib.h` 与 `libfdbus-clib.so*`）
- FDBus 真实调用可达
- 至少 1 个原生 action 闭环成功
- 异常路径（服务不可达/超时）可回退并记录审计

---

## 5.3 端云协同策略回归（v0.6.2 新增）

新增 `chat` 端云协同回归建议（x86 可执行）：

1. **本地执行路由**
   - 输入触发云策略 `local_route_hint=local_status`
   - 断言：`mode_used=local-agent`、`intent_backend=cloud-strategy`、`decision.route=local_status`、`decision.policy=execute`

2. **本地澄清路由**
   - 输入触发云策略 `strategy=clarify`
   - 断言：`decision.route=local_clarify`、`decision.policy=clarify`

3. **非照搬约束**
   - 断言最终文案包含本地裁决前缀（`[端云协同-本地裁决]`）
   - 断言 `observation.cloud_policy` 存在且可解析（`parser_mode=json|fallback`）

4. **可观测链路（v0.3.80）**
   - 断言 `decision_trace` 每层均包含 `layer/input/output/ts`
   - 断言 `ts` 非空字符串（毫秒时间戳）

5. **本地任务内联执行（v0.3.79）**
   - 输入触发云策略 `local_route_hint=local_task_inline_capture_photo/local_task_inline_start_recording`
   - 断言：`decision.route=local_task_inline`、`decision.policy=execute`
   - 断言：`observation.inline_task.action in {capture_photo,start_recording}`，且 `observation.task` 存在

6. **高风险拒绝门控（v0.3.79）**
   - 输入触发云策略 `strategy=reject` 或 `risk=high`
   - 断言：`decision.route=local_reject`、`decision.policy=reject`
   - 断言：`observation.risk_gate.blocked=true`，最终文案包含“拒绝执行”

7. **外部查询分层路由（v0.6.2）**
   - 输入：`查天气`、`今天上海天气`、`看新闻`、`最近AI新闻`、`帮我查一下`
   - 断言：
     - weather 缺 city -> `decision.route=local_external_clarify` + `decision.policy=clarify`
     - weather 槽位齐全 -> `decision.route=local_external_weather` + `decision.policy=execute`
     - news 缺 topic -> `decision.route=local_external_clarify` + `decision.policy=clarify`
     - news 槽位齐全 -> `decision.route=local_external_news` + `decision.policy=execute`
     - general 外部查询 -> `decision.route=local_external_clarify`（阈值区间 0.45~0.75）

8. **四层模块映射文档存在性（v0.6.2）**
   - 断言存在文档：`FOUR_LAYER_MODULE_MAPPING.md`
   - 断言文档至少覆盖四层关键词：`LLM` / `Prompt/Policy` / `Context` / `Harness Loop`

9. **嵌入式轻量约束回归（v0.6.2）**
   - 断言 `build/thin_agent` 为 Linux ELF 可执行文件
   - 断言主服务二进制体积维持轻量级（当前基线约 2.5MB，允许小幅波动）
   - 断言启动日志持续包含 `[ws_agent_cpp] version=...`

10. **端云协同三级架构（v0.14.0 新增）**

   **Tier 2 意图分类回归（x86 mock 可执行）：**
   - 输入 `"what r u"`（缩写）→ cloud-classify → local_profile
   - 输入 `"what are u"` → cloud-classify → local_profile
   - 输入 `"how is the system"` → cloud-classify → local_status
   - 输入 `"write quicksort"` → cloud-classify → complex → fallthrough Tier 3

   **缩写归一化回归：**
   - `"what r u"` → 展开为 "what are you" → profile
   - `"who r u"` → 展开为 "who are you" → profile
   - `"unique"` → 不误展开（保持原样）
   - `"please help plz"` → "please help please"

   **多语言对话覆盖（离线模式，local-agent）：**
   - `"你好"` → type=chat_result, text 非空, mode=local/offline
   - `"你是谁"` → route=local_profile, profile_mode=concise
   - `"who are you"` → route=local_profile, profile_mode=concise
   - `"目前有哪些技能"` → route=local_profile, profile_mode=concise, skills_only=true
   - `"你有什么能力"` → route=local_profile, profile_mode=concise, skills_only=true
   - `"介绍一下你自己和主体能力"` → route=local_profile, profile_mode=detailed
   - `"你是什么模型"` → route=local_status（非 profile）

   **意图门控验证：**
   - `intent_has_local_handler(profile/status/weather/news/event/general_query)` → true
   - `intent_has_local_handler(complex/external/unknown)` → false
   - `intent_classifier_prompt("zh")` → 包含 "意图分类器"
   - `intent_classifier_prompt("en")` → 包含 "intent classifier"


---

## 6. 常见风险与规避

1. **风险：测试代码误依赖 FDBus 头文件**
   - 规避：仅 `FdbusDeviceControl.*` 包含 `fdbus_clib.h`

2. **风险：x86 与 aarch64 行为不一致**
   - 规避：mock 行为契约与真实返回码保持一致，维护统一错误码映射

3. **风险：demo 测试“假通过”**
   - 规避：demo 必须覆盖重试和失败路径，不只跑 happy path

---

## 7. 与其他文档关系

- 架构总览：`ARCHITECTURE.md`
- 目录边界：`PROJECT_STRUCTURE.md`
- 协议：`PROTOCOL.md`
- 接口与错误码：`INTERFACE_AND_ERRORCODE.md`
- 状态机：`STATE_MACHINE.md`
- 数据库：`DB_SCHEMA.md`
