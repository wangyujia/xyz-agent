#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {

// ChatPolicy：对话策略配置的统一访问层。
// 从 config/chat_policy.json（或 THIN_AGENT_CHAT_POLICY_PATH）加载关键词、模板、槽位与路由规则，
// 供 AgentService / IntentScorer / ExternalInfoClient 使用，避免在 C++ 中硬编码文案与匹配词。

/// 意图声明（来自 chat_policy.json intents.<name>）：external 或 local。
struct IntentSpec {
  std::string name;                       ///< weather / news / profile / status 等
  std::string kind;                       ///< external / local
  std::vector<std::string> required_slots;
  std::string primary_slot;               ///< 主槽位名（澄清目标）
  std::string slot_detector;              ///< weather_city / news_topic
  std::string dialog_slot_policy;         ///< referential / same_intent / always
  std::string clarify_template_key;
  std::string fetch_failed_template_key;
  std::string summary_template_key;
  std::string route_execute;
  std::string route_clarify;
  std::string fetcher;                    ///< weather / news（external）
  std::string handler;                    ///< profile / status / event_recent / memory_*（local）
  std::string keyword_key;                ///< keywords.<key>，本地意图触发词
  std::string predicate;                  ///< short_ack / messaging_capability / onnx_knowledge / supported_languages / budget_exceeded
  std::string gate_mode;                  ///< gate 仅在指定运行模式下生效（如 cloud）
  std::string action_name;                ///< task/action 执行的动作名
  std::string intent_label;               ///< decision.intent 显示名（默认同 name）
  std::string query_suffix;
  std::string post_execute;               ///< 如 weather_advice；空表示无
  bool boost_on_slot_filled{true};
  bool require_execute_threshold{false};
  double default_confidence{0.95};        ///< 本地意图默认置信度
  std::string reason_missing;
  std::string reason_conf_below;
  std::string reason_fetch_ok;
  std::string reason_fetch_failed;
  std::string reason_execute;             ///< 本地意图执行 reason
  std::string reason_execute_detail;      ///< 本地意图详细模式 reason（如 profile detailed）
  std::string reason_budget_input;        ///< gate：输入超长
  std::string reason_budget_latency;      ///< gate：延迟预算不足
  std::string reason_budget_cost;         ///< gate：成本预算超限
  std::vector<std::string> route_excludes; ///< 本地路由排除谓词（model_status / cloud_meta 等）
  /// optional_slots: slot_key -> {default, resolver}
  nlohmann::json optional_slots = nlohmann::json::object();
};

/// 阶段3：意图门控策略（副作用 / 话轮要求 / 不匹配时行为）。
struct IntentGatePolicy {
  std::string name;
  std::string family;                   ///< 如 media
  bool side_effect{false};              ///< 是否有硬件/不可逆副作用
  std::string requires_dialogue_act;    ///< command / question / 空=不限
  bool requires_media_context{false};   ///< 是否要求上一轮媒体上下文
  std::string on_mismatch;              ///< clarify / block_execute / status_query
  std::string fuzzy_alias;              ///< 对应 IntentScorer media.* 名（可选）
  bool ok{false};
};

/// 门控评估结果。
struct IntentGateDecision {
  bool allow_execute{true};
  std::string action;   ///< allow / clarify / block_execute / status_query
  std::string reason;
};

/// 加载 intent_policy.<name>（缺失时尝试从 intent_anchors 回退 side_effect 字段）。
bool load_intent_gate_policy(const std::string& intent_name, IntentGatePolicy* out);

/// 列出 intent_policy 中所有意图名（跳过 _ 前缀文档键）。
std::vector<std::string> intent_policy_names();

/// 意图是否声明为副作用动作。
bool intent_has_side_effect(const std::string& intent_name);

/// 按配置评估：当前话轮能否执行该意图。
IntentGateDecision evaluate_intent_gate(const std::string& intent_name,
                                        const std::string& dialogue_act,
                                        bool has_media_context);

/// 按意图名加载 IntentSpec；配置缺失时返回 ok=false 的空 spec（name 仍填入）。
bool load_intent_spec(const std::string& intent_name, IntentSpec* out);

/// 是否存在 intents.<name> 声明且 kind=external。
bool is_external_intent(const std::string& intent_name);

/// 是否存在 intents.<name> 声明且 kind=local。
bool is_local_intent(const std::string& intent_name);

/// 返回 intents 段下所有 kind=external 的意图名。
std::vector<std::string> external_intent_names();

/// 返回 intents 段下所有 kind=local 的意图名。
std::vector<std::string> local_intent_names();

/// 返回指定 kind 的意图名（clarify / task / action / external_clarify 等）。
std::vector<std::string> intent_names_by_kind(const std::string& kind);

/// 返回已加载的策略 JSON 根对象（进程内缓存，路径变化时自动重载）。
const nlohmann::json& chat_policy();

/// 解析策略文件路径：环境变量优先，否则按候选路径查找。
std::string chat_policy_path();

/// 重置进程内策略缓存，强制下次 chat_policy() 重新读盘。供测试及热重载使用。
void chat_policy_reset();

/// 读取点分路径下的字符串数组，例如 "nlp.query_filler_prefix"。
std::vector<std::string> policy_string_list(const std::string& dot_path);

/// 读取 keywords.<key> 下的意图/规则匹配词列表。
std::vector<std::string> policy_keywords(const std::string& key);

/// 读取 templates.<key> 文案；缺失时返回 fallback。
std::string policy_text(const std::string& key, const std::string& fallback = "");

/// 将模板中的 {name} 占位符替换为 kv 中的值。
std::string render_policy_template(std::string tpl,
                                   const std::vector<std::pair<std::string, std::string>>& kv);

/// 查询类口语前缀（如「请」「帮我」），用于剥离用户输入中的填充词。
std::vector<std::string> query_filler_prefixes();

/// 天气槽位续接时拼接到城市名后的查询后缀（默认「的天气」）。
std::string weather_query_suffix();

/// 新闻槽位续接时拼接到主题后的查询后缀（默认「新闻」）。
std::string news_query_suffix();

/// profile 详细模式触发词（如「详细」「详细能力」）。
std::vector<std::string> profile_detail_markers();

/// 云端策略顾问 system prompt，来自 cloud.strategist_system。
std::string cloud_strategist_system_prompt();

/// 云策略 JSON 解析失败时的文本启发式词表，category 如 clarify / local_profile / reject。
std::vector<std::string> cloud_parse_heuristics(const std::string& category);

/// 按气温档位与语言渲染穿衣/出行建议文案。
std::string render_weather_advice(const std::string& city,
                                  int temp_c,
                                  const std::string& condition,
                                  int humidity,
                                  const std::string& lang);

/// 策略侧文本归一化：去标点、缩写展开、小写、trim，供槽位与关键词匹配使用。
std::string normalize_text_for_policy(std::string text);

/// 展开常见聊天缩写（u→you, r→are 等），配置来源 nlp.chat_abbreviations。
std::string expand_chat_abbreviations(std::string text);

/// 获取意图分类器 system prompt（按语言）。
std::string intent_classifier_prompt(const std::string& lang);

/// 查询 intent_map：该意图是否有本地 handler。
bool intent_has_local_handler(const std::string& intent);

/// 判断 UTF-8 字符串是否至少包含 min_chars 个字符（用于短句判定）。
bool utf8_char_count_at_least(const std::string& s, size_t min_chars);

/// 从归一化文本中检测天气城市槽位。
std::string detect_city_slot(const std::string& norm);

/// 从归一化文本中检测新闻主题槽位。
std::string detect_news_topic_slot(const std::string& norm);

/// 多轮续问场景下从短句中提取城市（如「深圳的呢」）。
std::string extract_city_followup(const std::string& text, const std::string& norm);

/// 判断 token 是否为天气日期词（今天/明天等），不可当作城市。
bool is_weather_date_token(const std::string& token_norm);

/// 从文本解析合法天气城市：必须在城市表内，且排除日期词。
std::string resolve_weather_city_candidate(const std::string& text, const std::string& norm);

/// 澄清态下用户是否明显切换了意图（如 pending weather 时说「看新闻」）。
bool should_abort_slot_for_intent_switch(const std::string& norm, const std::string& pending_intent);

/// 解析天气查询中的日期槽（今天/明天等）。
std::string resolve_weather_date_slot(const std::string& norm);

/// 解析新闻查询中的时间范围槽（最近/本周等）。
std::string resolve_news_time_range_slot(const std::string& norm);

/// 天气续接后缀词（如「呢」「的天气」），用于从短句截取城市。
std::vector<std::string> weather_followup_suffixes();

/// 城市槽位尾部助词，剥离时一并去除。
std::vector<std::string> weather_slot_trailing_particles();

/// 天气建议确认回复词（如「需要」「好的」）。
std::vector<std::string> weather_confirm_replies();

/// 新闻续接后缀词，用于从短句截取主题。
std::vector<std::string> news_followup_suffixes();

/// 是否为天气相关的弱触发表达（配置 slots.weather.extra_triggers）。
bool is_weather_extra_trigger(const std::string& norm);

/// 是否为询问端云分工/模型使用的 meta 问题（避免误路由到 profile）。
bool is_cloud_meta_question(const std::string& text);

/// 复杂意图探测标记：{子串, 标记名}，用于决定是否强制走云策略。
std::vector<std::pair<std::string, std::string>> complex_intent_markers();

/// Fast 快速通道配置：简单文件操作走 fast 模型，复杂任务走 main 模型。
struct FastPathConfig {
  std::string model = "glm-4.5-flash";
  int timeout_ms = 15000;
  int max_tool_rounds = 3;
  std::string prompt_zh;
  std::string prompt_en;
  std::vector<std::string> file_keywords;
  std::vector<std::string> complex_exclude;
  std::vector<std::string> multi_step_markers;
};
FastPathConfig fast_path_config();

/// 云策略 local_route_hint 白名单集合。
std::unordered_set<std::string> cloud_allowed_route_hints();

/// 按语言本地化城市显示名（中/英互转等）。
std::string localize_city_name(std::string city, const std::string& lang);

/// 按语言本地化天气状况描述。
std::string localize_weather_condition(std::string condition, const std::string& lang);

/// 按语言本地化天气日期槽（en 时今天→today、明天→tomorrow）。
std::string localize_weather_date(const std::string& date, const std::string& lang);

// --- 27 语支持 / 15 语模板 / 策略 B ---

/// 检测用户提问语种（27 语 code；汉字共用 zh，由 pick_template_lang 再分 zh/zh-TW）。
std::string detect_query_language(const std::string& text);

/// 选择回复模板语种：15 语专用模板，其余 fallback 为 en。
std::string pick_template_lang(const std::string& query_lang, const std::string& text);

/// 按 template_lang 读取 templates.<key> 或 templates.<key>_<lang>，回退 en → 默认 key。
std::string policy_text_for_lang(const std::string& key,
                                 const std::string& template_lang,
                                 const std::string& fallback = "");

/// 语言代码是否在 15 语模板白名单内。
bool is_template_language(const std::string& code);

/// 对外支持的语种数量（配置 languages.supported 长度，当前为 27）。
size_t supported_language_count();

/// 用户是否在询问支持哪些语言。
bool is_supported_languages_query(const std::string& text);

/// 渲染「支持 N 种语言」确定性回复（名单来自配置，非 LLM 编造）。
std::string render_supported_languages_answer(const std::string& template_lang);

/// 语言 code → LLM 翻译提示用英文名。
std::string language_display_name(const std::string& lang_code);

/// 策略 B：是否需对整段回复做 LLM 翻译（template_lang ≠ reply_target）。
bool needs_reply_translation(const std::string& query_lang, const std::string& template_lang);

/// 整段回复的翻译目标语种（zh 共用检测时目标为 template_lang）。
std::string reply_target_language(const std::string& query_lang, const std::string& template_lang);

/// 输入翻译配置（从 chat_policy.json translate_input 读取）。
struct TranslateInputConfig {
  std::string cloud_model = "glm-4.5-flash";
  int cloud_timeout_ms = 5000;
  std::string cloud_prompt = "Translate to English. Return only the translation, no explanation.";
  int local_max_tokens = 128;
  int min_length_for_detect = 10;
};
TranslateInputConfig translate_input_config();

/// 逐字符快速检测：是否为简体中文（含 CJK 统一汉字但无繁体特有字符）。
bool is_simplified_chinese(const std::string& text);

/// 逐字符快速检测：是否为纯英文（ASCII 字母 + 空格 + 标点，无其他语种特征）。
bool is_english_text(const std::string& text);

/// 是否需要翻译输入：非简体中文、非英文 → true。
bool needs_input_translation(const std::string& text);

/// 获取进度/动作消息（按语言选择 zh/en）。
/// @param key      消息键（如 "local_analysis"）
/// @param use_zh   true=简体中文，false=英文
/// @param params   占位符替换表（如 {{"path", "/tmp"}}），可选
std::string progress_msg(const std::string& key, bool use_zh,
                         const std::vector<std::pair<std::string, std::string>>& params = {});

}  // namespace thin_agent
