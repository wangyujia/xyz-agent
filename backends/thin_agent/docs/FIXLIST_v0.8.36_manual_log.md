# v0.8.36 人工验证修复清单（定稿）

依据：`E:\work\dev\cloud\log\agent.log`（2026-07-08 16:10–16:21）

## 不在本次范围
- wttr.in 偶发超时（网络环境）
- 「需要」在「好的」已消费 `weather_advice` 槽后再问 → 走云（会话顺序问题，低优）
- 云策略直返路径策略 B（已知 backlog，单独立项）

---

## P0 — 必须修

### F1 对话槽位污染（`查天气` → 柏林、`台灣天氣` → 华沙）

**现象**：`查天气` 未澄清却查 Berlin；`台灣天氣如何` 查 Warsaw。

**根因**：`should_apply_dialog_slot()` 在 `has_referential_context_markers` 为 false 时仍因 `dialog_context_matches_intent` 回填上一轮 `city`。

**修复**：天气/新闻仅在有**指代标记**（刚才/上次/的呢）或**续问短句**（`extract_city_followup`）时回填 dialog city；裸 `查天气` 必须走缺城澄清。

**文件**：`src/core/DialogSlotRecall.cpp`

---

### F2 无变音符拉丁语误判为 `en`（法/波）

**现象**：
- `Présentez - vous` → `query_lang=en`，中文 profile
- `Jaka jest pogoda w Warszawie?` → `query_lang=en`，英文天气
- `Actualités récentes AI` → `query_lang=en`，英文新闻

**根因**：`detect_latin_query_lang` 依赖变音符或少量关键词；上述句子无 ą/é 等或未命中 hint。

**修复**：扩展关键词表（不依赖变音符）：
- pl: `pogoda`, `jaka`, `jest`, `opowiedz`, `sobie`
- fr: `présentez`, `presentez`, `actualités`, `actualites`, `récentes`, `recentes`, `bonjour`

**文件**：`src/core/ChatPolicy.cpp`

---

### F3 英文天气城市 recall 未命中

**现象**：`what city was the weather` → `memory_recent` 列表，非 recall 文案。

**根因**：`is_weather_location_recall_query` 仅匹配 `which city`，不含 `what city`。

**修复**：`asks_where` 增加 `what city` / `which city was`。

**文件**：`src/core/DialogSlotRecall.cpp`；recall 路由置于 tier2 之前或 tier2 跳过 recall 问句。

---

## P1 — 应修

### F4 天气模板 `{city}{date}` 粘连

**现象**：`Shenzhentoday` / `Warszawatoday`。

**根因**：`weather.summary_en` 首条 `{city}{date}:` 无分隔。

**修复**：改为 `{city} {date}:`；`slots`/tool_calls 中 `date` 展示用 `localize_weather_date`。

**文件**：`config/chat_policy.json`，`AgentService.cpp`（observation 可选）

---

### F5 中文模板 condition 中英混排

**现象**：`雷暴，Shower In Vicinity，...`

**根因**：`detect_query_language(cond)` 含汉字 → `zh`，不触发 `needs_translation`。

**修复**：`tpl_lang` 为 zh/zh-TW 且 condition 含拉丁词时，对 condition 送翻译或清洗。

**文件**：`AgentService.cpp` 天气分支

---

### F6 台灣未解析城市 + 繁体模板

**现象**：`台灣天氣如何` → `template_lang=zh`（应为 zh-TW），且无台北映射。

**修复**：
- `cities` 增加 `台灣`/`台湾` → `台北`
- 确认 `pick_template_lang` 对繁体标记返回 zh-TW（已有 traditional_markers）

**文件**：`config/chat_policy.json` 或 `ChatPolicy.cpp` cities 表

---

## P2 — 可选 / 后续

- Profile 非法语专用模板，依赖策略 B（F2 后 `query_lang=fr` 应自动翻译）
- 日文天气整段日语（当前英日混杂，依赖策略 B 加强）
- `cloud_classify` 与 recall 路由优先级统一梳理

---

## 验收用例（修后重测）

| # | 输入 | 期望 |
|---|------|------|
| R1 | 新会话：`查天气` | 澄清要城市，**不**带上一轮 Berlin |
| R2 | `Jaka jest pogoda w Warszawie?` | `query_lang=pl`，波兰语或 en 模板+策略 B 波兰语 |
| R3 | `Présentez-vous` | `query_lang=fr`，法语 profile 或策略 B 法语 |
| R4 | `台灣天氣如何`（新会话） | 查台北，`template_lang=zh-TW` |
| R5 | 先 Shanghai weather，再 `what city was the weather` | recall 英文模板 + Shanghai |
| R6 | `weather in Shenzhen today` | 无 `Shenzhentoday` 粘连 |
