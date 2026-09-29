# 语言矩阵 v0.8.36 实现规格（定稿）

## 版本
- 发布：`v0.8.36`（保持 0.8.x；`v0.9` 留给真机 FDBus 对接）

## 对外 vs 对内
| 维度 | 定稿 |
|------|------|
| 对外支持语种 | **27**（简体 `zh`、繁体 `zh-TW` 分开计） |
| 专用模板语种 | **15**：en, zh, zh-TW, ja, ko, th, de, fr, nl, sv, ru, it, es, pt |
| Fallback（13） | ar, pl, ms, vi, fi, no, da, cs, hu, sk, lt, lv, et → `template_lang=en` + **策略 B** |
| 简繁检测 | 共用 `query_lang=zh`；`pick_template_lang` 启发式选 zh / zh-TW |
| 策略 B | `template_lang` 拼完 `text` 后，若 `template_lang != reply_target_lang`，整段 LLM 翻译 |

`reply_target_lang`：`query_lang==zh` 时用 `template_lang`（zh 或 zh-TW），否则用 `query_lang`。

## 已含于本版（延续上次 bugfix）
- 新闻 URL `url_encode_component`
- AI 标题 `ai` 词边界过滤
- 天气 en 模板 city/date 本地化

## 不做
- Profile 走云 draft（仍为固定能力清单）
- `v0.9` 版本号

## 配置：`chat_policy.json` → `languages`
- `supported[27]`：code + label_zh + label_en
- `template_codes[15]`
- `query_keywords`：支持哪些语言类问句
- `traditional_markers`：繁体模板启发式
- `supported_answer_zh` / `_en` / `_zh-TW`：含 `{count}` `{list}`

## 代码
- `ChatPolicy`：`detect_query_language`、`pick_template_lang`、`policy_text_for_lang`、`render_supported_languages_answer`、`is_supported_languages_query`、`language_display_name`、`needs_reply_translation`
- `AgentService`：`local_reply` 末尾策略 B；`local_supported_languages` 短路（在 profile 前）；天气/新闻用 `policy_text_for_lang`
