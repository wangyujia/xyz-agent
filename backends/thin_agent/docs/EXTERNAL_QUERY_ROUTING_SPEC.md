# thin_agent 外部查询意图路由规范（v0.1）

> 适用阶段：x86 demo 优先（FDBus 暂缓）
> 
> 目标：把“天气 / 新闻 / 外部查询”类问题稳定路由到统一策略，减少口语变体导致的误路由。

---

## 1. 设计目标

1. **稳定**：常见口语表达不因标点/措辞变化而漂移。
2. **可控**：高频意图先本地识别；不确定再云策略兜底。
3. **一致**：clarify（澄清）话术统一模板，避免回复风格波动。
4. **可迭代**：误路由样本可回放，支持每周小步扩展词表/样本。

---

## 2. 分层路由策略（最佳实践落地）

### L0：硬规则（少量高价值）

- 作用：品牌口径、安全策略、固定入口（例如“你是谁/你有什么能力/高风险拒绝”）。
- 原则：只保留稳定高价值规则，不无限堆同义词。

### L1：本地意图识别（external-query 分类）

新增意图：
- `external_weather`
- `external_news`
- `external_general`

输出：
- `intent`
- `confidence`
- `slots`

### L2：云策略兜底（cloud-strategy）

当 L1 低置信度或槽位不完整时，走云策略建议；最终仍由本地裁决输出。

### L3：统一澄清模板（clarify）

对槽位缺失场景，不自由发挥，使用固定模板。

---

## 3. 意图定义与槽位

### 3.1 external_weather

**触发语义**：天气、温度、下雨、空气质量、风力、体感、预报等。  
**必需槽位**：
- `city`（城市/地区）

**可选槽位**：
- `date`（今天/明天/具体日期）
- `metric`（温度/降雨/空气质量等）

**澄清模板（缺 city）**：
- `可以查天气。请告诉我你要查询的城市或地区（例如：上海、北京朝阳）。`

---

### 3.2 external_news

**触发语义**：新闻、头条、快讯、热点、今天发生了什么等。  
**必需槽位**：
- `topic_or_scope`（主题或范围：如“科技”“AI”“国际”“上海本地”）

**可选槽位**：
- `time_range`（今天/本周/最近24小时）
- `count`（条数）

**澄清模板（缺 topic_or_scope）**：
- `可以查新闻。你想看哪个方向（如 科技/AI/财经/本地）？`

---

### 3.3 external_general

**触发语义**：查一下、搜一下、帮我看下外部信息，但无法明确归入天气/新闻。  
**澄清模板**：
- `我可以帮你外部查询。请补充要查的主题和范围（例如：关键词 + 时间范围/地区）。`

---

## 4. 口语触发样例（首批）

### weather（应命中 external_weather）
- `查天气`
- `今天上海天气`
- `北京明天天气怎么样`
- `深圳会下雨吗`
- `广州现在多少度`

### news（应命中 external_news）
- `看下今天新闻`
- `最近AI新闻`
- `给我科技头条`
- `上海本地新闻`
- `过去24小时财经快讯`

### general（应命中 external_general）
- `帮我查一下`
- `搜一下最新消息`
- `查个外部信息`

---

## 5. 决策与返回字段约束

当命中外部查询类，`chat_result` 建议包含：

- `mode_used=local-agent`（本地裁决）
- `intent_backend=rules|local-model|cloud-strategy`
- `decision.route`：
  - `local_external_weather`
  - `local_external_news`
  - `local_external_clarify`
- `decision.policy`：`execute|clarify`
- `decision.slots`：包含已提取槽位
- `decision_trace[]`：至少 `intent -> policy`（若走云则含 `llm`）

---

## 6. TDD 测试清单（必须先 RED）

### 6.1 单测（路由正确性）

正例：
1. `查天气` -> `local_external_clarify`（缺 city）
2. `今天上海天气` -> `local_external_weather`
3. `看下今天新闻` -> `local_external_clarify`（缺 topic_or_scope）
4. `最近AI新闻` -> `local_external_news`

反例：
1. `你是谁` 不能路由到 external
2. `你有什么能力` 不能路由到 external

### 6.2 E2E（运行态）

最少 4 条：
- weather clarify
- weather execute
- news clarify
- news execute

并断言：
- `mode_used`
- `decision.route`
- `decision.policy`
- `intent_backend`
- `decision_trace` 结构完整（含 `ts`）

---

## 7. 迭代流程（周更）

1. 收集误路由样本（日志中 top N）
2. 人工标注 intent/slots
3. 先补测试（RED）
4. 小步补规则或分类器词表
5. 回归（unit + ctest + E2E）
6. 版本递增并更新 PRD/协议/测试策略

---

## 8. x86 demo 阶段建议

- 先以 `rules + 槽位提取` 快速落地（不等 FDBus）
- 保持云兜底但本地主裁决
- 等样本量够再切 `local-model` 分类器（ONNX/TFLite）

---

## 9. external provider 默认策略（v0.6.2）

- `THIN_AGENT_EXTERNAL_PROVIDER=mock`（默认）：
  - 使用本地 mock 数据，稳定、可离线。

- `THIN_AGENT_EXTERNAL_PROVIDER=http`：
  - weather 默认 provider：`open-meteo`
    - 默认 URL：`https://api.open-meteo.com/v1/forecast?latitude=31.23&longitude=121.47&current=temperature_2m,relative_humidity_2m`
    - 可通过 `THIN_AGENT_EXTERNAL_WEATHER_URL` 覆盖，支持 `{city}` / `{date}` 占位符。
  - news 默认 provider：`hn-algolia`
    - 默认 URL：`https://hn.algolia.com/api/v1/search?query={topic}&tags=story&hitsPerPage=5`
    - 可通过 `THIN_AGENT_EXTERNAL_NEWS_URL` 覆盖，支持 `{topic}` / `{time_range}` 占位符。

- 测试注入（不出网）：
  - `THIN_AGENT_EXTERNAL_HTTP_MOCK_WEATHER_JSON`
  - `THIN_AGENT_EXTERNAL_HTTP_MOCK_NEWS_JSON`

- 可观测字段：
  - `observation.external_source in {mock, real-http}`
  - `observation.external_result.provider`（如 `open-meteo` / `hn-algolia` / `mock-*`）

---

## 10. 验收标准

达到以下即视为通过：

1. 天气/新闻核心句式路由准确率显著提升（回归集可复现）
2. 澄清话术统一（不再风格漂移）
3. 不破坏既有本地主体能力路由（profile/status/task）
4. 文档、测试、版本同步更新
