# THIN_AGENT_EDGE_CLOUD_AGENT_ONE_PAGER_V0_1

> 版本：v0.1（对应实现基线 v0.3.78）
> 目标：用一页说明 thin_agent 的“真正端云协同智能体”设计与落地口径。

---

## 1. 一句话定义

**thin_agent 是“本地主控 + 云端顾问”的端云协同智能体**：
- 云端给策略建议（不是最终回答）
- 本地做二次裁决并生成最终输出
- 危险动作始终受本地策略门控

---

## 2. 角色分工

### 2.1 端侧（Controller，唯一主控）
- 负责意图识别与上下文汇聚
- 负责策略裁决：`execute / clarify / fallback_cloud / reject`
- 负责最终用户可见文本生成（统一本地口径）

### 2.2 云侧（Strategist，策略顾问）
- 返回结构化策略建议（`cloud_policy`）
- 不直接驱动设备动作
- 不直接成为最终回包

---

## 3. 核心闭环（chat 路径）

1. 用户输入到达端侧
2. 端侧先走本地规则/意图识别（rules/onnx）
3. 需要云辅助时调用云模型
4. 云返回策略建议（优先 JSON，失败则启发式解析）
5. 端侧策略层二次裁决
6. 端侧生成最终答复并输出 `decision_trace`

> 硬约束：最终文本由本地生成，当前实现前缀为 `"[端云协同-本地裁决]"`。

---

## 4. 关键协议可观测字段

- `intent_backend`
  - `rules | onnx | cloud | cloud-strategy`
- `decision`
  - `route / reason / policy / intent / confidence`
- `observation.cloud_policy`
  - 云策略建议解析结果（如 `strategy/intent/confidence/risk/local_route_hint/response_draft/parser_mode`）
- `decision_trace`
  - 至少包含：`intent -> llm -> policy`

---

## 5. 当前已落地能力（v0.3.78）

- 云建议触发本地执行：`local_status`（`policy=execute`）
- 云建议触发本地澄清：`local_clarify`（`policy=clarify`）
- 非照搬约束：最终文案含本地裁决前缀
- 可观测闭环：`intent_backend=cloud-strategy` + `observation.cloud_policy` + 分层 `decision_trace`

---

## 6. 风险控制

- 未命中白名单动作：拒绝或澄清
- 云调用失败：按配置离线回退
- 高风险场景：优先 `reject` 或 `clarify`
- 所有关键路径保留审计与结构化可观测字段

---

## 7. 测试与验收口径

### 单元测试
- 覆盖云策略 -> 本地执行/澄清
- 覆盖 `intent_backend=cloud-strategy`
- 覆盖 `observation.cloud_policy` 存在性
- 覆盖非照搬前缀断言

### 运行态 E2E
- mock 云返回 `local_route_hint=local_status`：期望 `local-agent + execute`
- mock 云返回 `strategy=clarify`：期望 `local-agent + clarify`
- 断言 `decision_trace` 分层齐全

---

## 8. 下一步（建议）

1. 扩展到 task/action 路由（如 `capture_photo/start_recording`）
2. 增加高风险 `reject` 实战路径
3. 引入成本预算（token/延迟）进入策略裁决
4. 形成可配置策略模板（按业务域调优）

---

## 9. 关联文档

- PRD：`PRD.md`
- 架构：`ARCHITECTURE.md`
- 协议：`PROTOCOL.md`
- 测试策略：`TESTING_STRATEGY.md`
