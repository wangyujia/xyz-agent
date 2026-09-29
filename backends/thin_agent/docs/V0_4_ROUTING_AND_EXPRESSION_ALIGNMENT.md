# thin_agent v0.4 路由与表达策略对齐稿（v0.1）

> 状态：讨论稿（未开发）
> 适用范围：x86 demo 阶段（FDBus 暂缓）
> 目标：先统一“分层意图路由 + 本地回答自然度”策略，再进入实现

---

## 1. 目标与非目标

### 1.1 目标
1. 路由稳定：同义口语不再频繁误路由到 cloud fallback。  
2. 策略可控：本地主裁决，云仅建议。  
3. 回答自然：减少机械感，保持工程可控与可审计。

### 1.2 非目标（本阶段不做）
1. 不引入 FDBus 适配。  
2. 不做大规模模型替换。  
3. 不做 UI 大改（仅沿用现有 IM + trace toggle）。

---

## 2. 分层路由方案（v0.4 基线）

### L0：规则层（硬约束）
- 用途：安全拒绝、品牌口径、关键固定入口。  
- 典型：`你是谁`、`你有什么能力`、高风险动作拒绝、明确 task 指令。

### L1：本地意图层（intent + slots）
- 首批 intent 集合（6 类）：
  - `profile`
  - `status`
  - `task`
  - `weather`
  - `news`
  - `general`
- 输出统一结构：
```json
{
  "intent": "weather",
  "confidence": 0.82,
  "slots": {"city": "上海", "date": "今天"}
}
```

### L2：本地策略层（policy）
- 统一决策：`execute | clarify | reject | fallback_cloud`
- 本地策略为最终裁决层，保证可解释与一致性。

### L3：云策略层（可选兜底）
- 触发条件：低置信度/复杂语义/跨域知识。  
- 输出仅作为建议（intent_hint / route_hint / clarify_hint），禁止直出云文案。

---

## 3. 先后顺序（开发顺序草案）

### Phase A（先稳）
1. 固化 intent taxonomy（上述 6 类）。
2. 固化 weather/news clarify 模板。
3. 建立误路由回放集（至少 50 条）。

### Phase B（再准）
4. 完善 L1 词槽抽取（city/topic/date/time_range/count）。
5. 接入策略阈值并打通 decision_trace 结构化。

### Phase C（再自然）
6. 回答表达层升级（见第 5 节）。
7. 基于真实样本周迭代（测试先行）。

---

## 4. 阈值策略（讨论建议值）

> 注：阈值先给建议值，后续可按回放集微调。

- `confidence >= 0.75`：优先 `execute`（本地）
- `0.45 <= confidence < 0.75`：优先 `clarify`
- `< 0.45`：`fallback_cloud`（云给建议，本地裁决输出）

附加规则：
- 命中高风险策略：无条件 `reject`
- 槽位缺失（weather 缺 city / news 缺 topic）：直接 `clarify`

---

## 5. “本地回答僵硬”改进方案（不改架构，改生成链路）

采用两段式：

### 5.1 内容骨架层（稳定）
先生成结构化内容：
- 结论
- 关键点（2~5 条）
- 下一步动作/澄清问题

### 5.2 表达层（自然）
把骨架转成自然中文，要求：
- 结论先行
- 短句优先
- 列表有层次
- 避免每次固定开头

### 5.3 风格档位（建议）
- `concise`：极简（运维/调试）
- `normal`：默认（建议）
- `warm`：更自然（面向终端用户）

默认建议：`normal`。

---

## 6. 统一 clarify 模板（首版）

### weather
- 缺 city：
  - `可以查天气。请告诉我要查询的城市或地区（例如：上海、北京朝阳）。`

### news
- 缺 topic/scope：
  - `可以查新闻。你想看哪个方向（如 科技/AI/财经/本地）？`

### general external
- 信息不足：
  - `我可以帮你外部查询。请补充关键词和范围（时间/地区/主题）。`

---

## 7. 可观测与验收字段

建议 `chat_result` 至少包含：
- `mode_used`
- `intent_backend`
- `decision.route`
- `decision.policy`
- `decision.slots`
- `decision_trace[]`（layer/input/output/ts）

---

## 8. 测试与验收口径（先测后改）

### 单测（必须先 RED）
- profile/status/task 不回退
- weather/news 缺槽位走 clarify
- weather/news 槽位齐全走 execute
- 低置信度走 fallback_cloud（但最终本地裁决）

### E2E（最少 6 条）
1. 你有什么能力 -> local_profile
2. 你是什么模型 -> local_status
3. 查天气 -> local_external_clarify
4. 今天上海天气 -> local_external_weather
5. 看新闻 -> local_external_clarify
6. 最近AI新闻 -> local_external_news

### 体验验收（主观）
- 回答自然度：减少“模板腔”重复开头
- 可读性：结论先行，列表清晰
- 一致性：同意图不同表述风格一致

---

## 9. 本次待确认决策（你拍板）

1. intent 第一版是否锁定 6 类（profile/status/task/weather/news/general）？
2. 阈值是否先用 `0.75 / 0.45`？
3. 默认风格是否设为 `normal`？
4. weather/news clarify 模板是否直接采用第 6 节文案？

---

## 10. 下一步（确认后再开发）

- 输出《v0.4 实施任务拆解（测试优先）》：
  - T1 路由契约单测
  - T2 本地 intent/slot 实现
  - T3 policy + cloud fallback
  - T4 表达层自然化
  - T5 E2E + 文档 + 版本

> 你确认第 9 节后，再进入代码实施。