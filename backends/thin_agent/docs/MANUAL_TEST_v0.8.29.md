# thin_agent v0.8.29 手工复测清单

对照 **Web 导出 `agent.log`** 与 **服务端 `decision_audit.jsonl`** 做回归验证。覆盖 v0.8.28（P0 路由）与 v0.8.29（P1 成本/质量、P2 ONNX/审计）改动。

---

## 一、测试前准备

### 1.1 环境

```bash
# 编译 + 单测门禁（必须先绿）
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# 启动 demo（pro 模式，与手工日志一致）
~/.thin_agent/run_agent.sh --pro
```

确认握手版本为 **v0.8.29**（`ws_agent.html` 标题 / hello 响应）。

### 1.2 开启审计日志（推荐）

在启动脚本或 shell 里加：

```bash
export THIN_AGENT_DECISION_AUDIT=1
export THIN_AGENT_DECISION_AUDIT_PATH=data/decision_audit.jsonl   # 可选，默认即此路径
export THIN_AGENT_INTENT_ONNX=1                                   # 与生产脚本默认一致
```

### 1.3 记录方式

| 来源 | 路径 | 用途 |
|------|------|------|
| Web 聊天气泡 + 调试 JSON | 页面「拷贝调试日志」→ `agent.log` | UI 可见性、完整 `decision_trace` |
| 服务端审计 | `data/decision_audit.jsonl` | 逐条路由、backend、延迟，便于批量对比 |

审计每行 JSON 字段：

`ts_ms` / `session_id` / `text` / `route` / `intent` / `intent_backend` / `policy` / `latency_ms` / `reason`（如有）

---

## 二、通过标准（全局）

每条用例需同时满足：

- [ ] **UI 正文**无 `_draft`、无裸 JSON 策略块、无 `[端云协同-本地裁决]` 硬前缀
- [ ] **`decision.route`** 与预期一致
- [ ] **`intent_backend`** 与 `decision_trace` 首层一致（`rules` / `fuzzy` / `onnx` / `cloud_classify` / `cloud-strategy`）
- [ ] **不应上云的场景**不出现 `mode_used=cloud` 或无意义的 `cloud_classify` 层
- [ ] **`latency_ms`**：本地路由通常 &lt; 500ms；含 `cloud_classify` 的轮次允许更高，但多轮续问不应重复调 Tier2

---

## 三、P0 路由歧义（必测，5 条）

在同一新会话中依次发送，**每条单独记结果**。

| # | 输入 | 预期 route | 预期 intent | 预期 backend | UI 检查 |
|---|------|------------|-------------|--------------|---------|
| P0-1 | `你的模型是什么` | `local_status` | `status` | `rules` | 状态摘要（mode/model/provider），**不是**能力清单 |
| P0-2 | `你的本地模型用的什么` | `local_status` | `status` | `rules` | 正文含 **ONNX** + 云 LLM 双层说明 |
| P0-3 | `不是问你，是问另外一个ONNX模型是什么` | `local_status` | `status` | `rules` | 解释 `intent_multiclass.onnx`，**不**反问澄清 |
| P0-4 | `消息能力` | `local_clarify` | `general` | `rules` | 澄清：消息记录 / 记忆 / 新闻，**不是** profile |
| P0-5 | `好的` | `local_clarify` | `general` | `rules` | 短确认「有需要再叫我」，**不上云** |

**失败特征（旧版 bug）**：

- P0-1/2 → `local_profile`
- P0-3 → `local_clarify` + 云策略
- P0-4 → `local_profile` 且 `intent_backend=onnx`
- P0-5 → `cloud-strategy` / `end_conversation`

---

## 四、P1 成本 / 延迟 / 质量

### 4.1 Tier2 跳过（多轮天气）

**会话 `ws-weather`，按序发送：**

| # | 输入 | 预期 route | 审计 / trace 检查 |
|---|------|------------|-------------------|
| W-1 | `查天气` | `local_external_clarify` | 可不调 Tier2（缺槽澄清） |
| W-2 | `上海` | `local_external_weather` | `slots.city=上海` |
| W-3 | `今天天气如何？` | `local_external_weather` | `slots.city=上海`；**无** `cloud_classify` 层 |
| W-4 | `成都的呢` | `local_external_weather` | `slots.city=成都` |
| W-5 | `需要` | `local_weather_advice` | 穿衣建议，非云泛答 |

**失败特征**：W-3 在 rules 已识别 weather 且继承 city 后，仍出现 `cloud_classify`（约 +10s 延迟）。

### 4.2 天气展示质量

| # | 输入 | 检查点 |
|---|------|--------|
| Q-1 | `今天上海天气` | `condition` 为中文（如「晴」「多云」「附近有雷雨」），**不是** `Thundery outbreaks in nearby` 原文 |
| Q-2 | 查看 `observation.external_result.condition` | 与 UI 展示一致且已本地化 |

### 4.3 新闻内容质量

| # | 输入 | 检查点 |
|---|------|--------|
| N-1 | `最近AI新闻` | `local_external_news`；标题与 AI/科技相关 |
| N-2 | 正文标题 | **不应**出现 HN meta 帖（如 `Don't post generated/AI-edited comments`） |
| N-3 | `还有什么新闻？` | 继承 topic=AI，仍走 `local_external_news` |

### 4.4 短确认不上云（扩展）

| # | 输入 | 预期 |
|---|------|------|
| A-1 | `嗯` | 本地短 ack 或保持上下文，不上云 |
| A-2 | `行` | 同上 |
| A-3 | 天气多轮后说 `好的` | 短 ack，**不**结束会话上云 |

---

## 五、P2 ONNX / 工程 / 审计

### 5.1 ONNX 参与（`THIN_AGENT_INTENT_ONNX=1`）

| # | 输入 | 预期 | 说明 |
|---|------|------|------|
| O-1 | `who r u` | `local_profile`，backend 可为 `rules` 或 `onnx` | 规则先命中也 OK |
| O-2 | `说说你自己` | `local_profile` | 边界句，观察是否出现 `onnx` backend |
| O-3 | `你的模型是什么` | `local_status`，**非** `onnx→profile` | ONNX 不应把模型问句打成 profile |

### 5.2 云策略 telemetry

找一条**确实走云策略**的开放问句（如 `帮我写个 Python 快速排序`）：

- [ ] `observation.cloud_policy.parser_mode = "json"` 时，`reason` 应为 `cloud_strategy_parsed`（**不是** `cloud_response_parse_fallback`）
- [ ] `cloud_policy.raw` 中 `_draft` / `_draft ` 已被 strip（调试 JSON 可看，UI 不可见）

### 5.3 决策审计文件

跑完上述用例后检查 `data/decision_audit.jsonl`：

- [ ] 行数 ≥ 发送的 chat 条数
- [ ] 每行 `session_id` 与 Web 会话一致
- [ ] P0-1～P0-5 的 `route` / `intent_backend` 与页面 `decision_trace` 一致
- [ ] W-3 对应行**无**异常高 `latency_ms`（无 Tier2 时通常 &lt; 2s）

---

## 六、主链路冒烟（回归保底）

| # | 场景 | 输入序列 | 预期 |
|---|------|----------|------|
| S-1 | 身份 | `你是谁？` / `detail ability` / `who r u` | `local_profile` |
| S-2 | 状态 | `你是什么模型` / `系统状态` | `local_status` |
| S-3 | 新闻 recall | `最近AI新闻` → `刚才看的是什么方向的新闻？` | 后者 `news_topic_recall`，答「AI」 |
| S-4 | 任务 | `帮我拍张照` | `local_task_inline`，task 成功 |
| S-5 | 开放问答 | `帮我写个 Python 快速排序` | `cloud_llm` 或云策略后本地展示，代码正常 |

---

## 七、对照日志的快速检查（可选）

```bash
# P0 关键句路由（需 jq）
grep -E '你的模型|本地模型|ONNX模型|消息能力|"好的"' data/decision_audit.jsonl \
  | jq -c '{text,route,intent,intent_backend,latency_ms}'

# Tier2 调用次数（以 agent.log 为准）
grep 'cloud_classify' agent.log | wc -l
```

---

## 八、结果记录模板

```
测试人：
日期：
版本：v0.8.29
模式：--pro / ONNX=1 / AUDIT=1

| 用例ID | 输入 | route✓ | backend✓ | UI✓ | 备注 |
|--------|------|--------|----------|-----|------|
| P0-1   |      |        |          |     |      |
| ...    |      |        |          |     |      |

自动化：ctest __/10
手工通过率：__/__
阻塞问题：
```

---

## 九、建议执行顺序（约 15 分钟）

1. `ctest` 全绿
2. 开审计启动 demo
3. **P0 五条**（单会话）
4. **多轮天气 W-1～W-5**（第二会话）
5. **新闻 N-1～N-3**（第三会话）
6. 一条**云开放问答** + 检查 parser telemetry
7. 导出 Web `agent.log`，与 `decision_audit.jsonl` 交叉核对 P0 + W-3
8. 填结果表

---

## 相关文档

- 版本变更：`CHANGELOG.md`（v0.8.28 / v0.8.29）
- ONNX / 多轮槽位：`CHANGELOG.md`（v0.8.26）、`src/core/DialogSlotRecall.cpp`
- Golden 意图评估：`models/intent/golden_eval.jsonl`、`scripts/intent/eval_golden.py`
