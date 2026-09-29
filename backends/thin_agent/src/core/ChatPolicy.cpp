#include "thin_agent/core/ChatPolicy.h"
#include "thin_agent/RuntimePaths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>

// ChatPolicy 实现：加载 chat_policy.json，提供关键词/模板/槽位/本地化等只读访问。
// 本版本以配置为准：业务词表/映射缺失时返回空，由上层决定行为。

namespace thin_agent {
namespace {

// --- 文本与 JSON 路径工具 ---

/// 转小写副本（ASCII）。
std::string to_lower_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

/// 去除首尾空白。
std::string trim_copy(std::string s) {
  const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

/// 子串包含判断，可选大小写不敏感。
bool contains_substr(const std::string& haystack, const std::string& needle, bool case_insensitive) {
  if (needle.empty()) return false;
  if (!case_insensitive) return haystack.find(needle) != std::string::npos;
  return to_lower_copy(haystack).find(to_lower_copy(needle)) != std::string::npos;
}

/// 按点号拆分 JSON 点分路径。
std::vector<std::string> split_dot_path(const std::string& path) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : path) {
    if (ch == '.') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(ch);
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

/// 点分路径访问 JSON 子节点。
const nlohmann::json* json_at_path(const nlohmann::json& root, const std::string& path) {
  const nlohmann::json* cur = &root;
  for (const auto& key : split_dot_path(path)) {
    if (!cur->is_object() || !cur->contains(key)) return nullptr;
    cur = &((*cur)[key]);
  }
  return cur;
}

/// 策略归一化时需剥离的标点符号列表。
std::vector<std::string> punct_list() {
  return {
      "，", "。", "！", "？", "：", "；", "（", "）", "【", "】", "、", "~",
      ",", ".", "!", "?", ":", ";", "(", ")", "[", "]", "\"", "'",
      "\xE2\x80\x99", "\xE2\x80\x98"};
}

/// 城市槽位配置项：aliases 匹配归一化文本，value 为规范城市名。
struct CityEntry {
  std::vector<std::string> aliases;  ///< 别名列表（中文或英文）
  std::string value;                 ///< 槽位标准值
  bool match_lower{false};           ///< 是否对 norm 做小写后再匹配
};

/// 新闻主题槽位配置项，结构同 CityEntry。
struct TopicEntry {
  std::vector<std::string> aliases;
  std::string value;
  bool match_lower{false};
};

/// 通用别名组：用于日期/时间范围等简单映射。
struct AliasGroup {
  std::vector<std::string> match;  ///< 命中任一即映射到 value
  std::string value;
};

/// 从 chat_policy 加载天气城市表，缺失时用内置默认。
std::vector<CityEntry> load_weather_cities() {
  const auto* node = json_at_path(chat_policy(), "slots.weather.cities");
  if (!node || !node->is_array() || node->empty()) return {};

  std::vector<CityEntry> out;
  for (const auto& item : *node) {
    if (!item.is_object()) continue;
    CityEntry e;
    e.value = item.value("value", "");
    e.match_lower = item.value("match_lower", false);
    if (item.contains("aliases") && item["aliases"].is_array()) {
      for (const auto& a : item["aliases"]) {
        if (a.is_string()) e.aliases.push_back(a.get<std::string>());
      }
    }
    if (e.value.empty() || e.aliases.empty()) continue;
    out.push_back(std::move(e));
  }
  return out;
}

/// 从 chat_policy 加载新闻主题表，缺失时用内置默认。
std::vector<TopicEntry> load_news_topics() {
  const auto* node = json_at_path(chat_policy(), "slots.news.topics");
  if (!node || !node->is_array() || node->empty()) return {};

  std::vector<TopicEntry> out;
  for (const auto& item : *node) {
    if (!item.is_object()) continue;
    TopicEntry e;
    e.value = item.value("value", "");
    e.match_lower = item.value("match_lower", false);
    if (item.contains("aliases") && item["aliases"].is_array()) {
      for (const auto& a : item["aliases"]) {
        if (a.is_string()) e.aliases.push_back(a.get<std::string>());
      }
    }
    if (e.value.empty() || e.aliases.empty()) continue;
    out.push_back(std::move(e));
  }
  return out;
}

/// 加载别名组（日期/时间范围等），缺失时返回 defaults。
std::vector<AliasGroup> load_alias_groups(const std::string& dot_path,
                                          std::vector<AliasGroup> defaults) {
  const auto* node = json_at_path(chat_policy(), dot_path);
  if (!node || !node->is_array() || node->empty()) return defaults;

  std::vector<AliasGroup> out;
  for (const auto& item : *node) {
    if (!item.is_object()) continue;
    AliasGroup g;
    g.value = item.value("value", "");
    if (item.contains("match") && item["match"].is_array()) {
      for (const auto& m : item["match"]) {
        if (m.is_string()) g.match.push_back(m.get<std::string>());
      }
    }
    if (g.value.empty() || g.match.empty()) continue;
    out.push_back(std::move(g));
  }
  return out.empty() ? std::move(defaults) : out;
}

/// 在别名组中查找首个命中项并返回规范 value。
std::string match_alias_groups(const std::string& norm, const std::vector<AliasGroup>& groups) {
  for (const auto& g : groups) {
    for (const auto& m : g.match) {
      if (norm.find(m) != std::string::npos) return g.value;
    }
  }
  return "";
}

/// 在城市/主题槽位表中按 aliases 匹配规范值。
template <typename Entry>
std::string match_slot_entries(const std::string& norm, const std::vector<Entry>& entries) {
  for (const auto& e : entries) {
    for (const auto& alias : e.aliases) {
      if (contains_substr(norm, alias, e.match_lower)) return e.value;
    }
  }
  return "";
}

/// 英文天气状况 → 中文映射表（带路径缓存）。
const std::unordered_map<std::string, std::string>& weather_zh_map() {
  static std::unordered_map<std::string, std::string> cached;
  static std::string cached_path;
  const std::string path = chat_policy_path();
  if (path == cached_path && !cached.empty()) return cached;

  cached.clear();
  cached_path = path;
  const auto* node = json_at_path(chat_policy(), "localization.weather_zh");
  if (node && node->is_object()) {
    for (auto it = node->begin(); it != node->end(); ++it) {
      if (it.value().is_string()) cached[it.key()] = it.value().get<std::string>();
    }
  }
  return cached;
}

/// 英文城市名 → 中文显示名映射表。
const std::unordered_map<std::string, std::string>& cities_zh_map() {
  static std::unordered_map<std::string, std::string> cached;
  static std::string cached_path;
  const std::string path = chat_policy_path();
  if (path == cached_path && !cached.empty()) return cached;

  cached.clear();
  cached_path = path;
  const auto* node = json_at_path(chat_policy(), "localization.cities_zh");
  if (node && node->is_object()) {
    for (auto it = node->begin(); it != node->end(); ++it) {
      if (it.value().is_string()) cached[to_lower_copy(it.key())] = it.value().get<std::string>();
    }
  }
  return cached;
}

/// 中文城市名 → 英文显示名映射表（反转 cities_zh，值做 Title Case）。
const std::unordered_map<std::string, std::string>& cities_en_map() {
  static std::unordered_map<std::string, std::string> cached;
  static std::string cached_path;
  const std::string path = chat_policy_path();
  if (path == cached_path && !cached.empty()) return cached;

  cached.clear();
  cached_path = path;
  for (const auto& [en_lower, zh] : cities_zh_map()) {
    std::string title;
    bool cap = true;
    for (char c : en_lower) {
      if (c == ' ') {
        cap = true;
        title.push_back(c);
      } else {
        title.push_back(cap ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
        cap = false;
      }
    }
    cached.emplace(zh, title);  // 多个英文别名映射同一中文时保留首个
  }
  return cached;
}

/// 复合天气英文短语 → 中文的规则（contains 全部命中）。
struct WeatherZhRule {
  std::vector<std::string> contains;
  std::string value;
};

/// 加载 localization.weather_zh_rules，缺失时用内置默认。
std::vector<WeatherZhRule> weather_zh_rules() {
  static std::vector<WeatherZhRule> cached;
  static std::string cached_path;
  const std::string path = chat_policy_path();
  if (path == cached_path && !cached.empty()) return cached;

  cached.clear();
  cached_path = path;
  const auto* node = json_at_path(chat_policy(), "localization.weather_zh_rules");
  if (node && node->is_array()) {
    for (const auto& item : *node) {
      if (!item.is_object()) continue;
      WeatherZhRule r;
      r.value = item.value("value", "");
      if (item.contains("contains") && item["contains"].is_array()) {
        for (const auto& c : item["contains"]) {
          if (c.is_string()) r.contains.push_back(to_lower_copy(c.get<std::string>()));
        }
      }
      if (!r.value.empty() && !r.contains.empty()) cached.push_back(std::move(r));
    }
  }
  return cached;
}

/// 去除天气 token 首尾空白。
std::string trim_weather_token(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  return s;
}

/// 将单段英文天气描述转为中文（直译表 + 规则表）。
std::string lookup_weather_zh_part(std::string part) {
  part = trim_weather_token(std::move(part));
  if (part.empty()) return part;
  const auto& mp = weather_zh_map();
  if (auto it = mp.find(part); it != mp.end()) return it->second;

  const std::string lower = to_lower_copy(part);
  for (const auto& [k, v] : mp) {
    if (to_lower_copy(k) == lower) return v;
  }
  for (const auto& rule : weather_zh_rules()) {
    bool ok = true;
    for (const auto& token : rule.contains) {
      if (lower.find(token) == std::string::npos) {
        ok = false;
        break;
      }
    }
    if (ok) return rule.value;
  }
  return part;
}

}  // namespace

// --- 策略加载与基础访问 ---

// 进程内策略缓存（chat_policy() 和 chat_policy_reset() 共享）
static std::string g_cached_path;
static nlohmann::json g_cached = nlohmann::json::object();
/// v0.53.60: 缓存互斥——多 worker 并发首调(冷缓存)同时写
/// g_cached/g_cached_path=json 并发写=堆坏;读侧双检+写锁
static std::mutex g_policy_mu;

/// 解析策略文件路径：环境变量优先，否则按候选路径查找。
std::string chat_policy_path() {
  if (const char* env = std::getenv("THIN_AGENT_CHAT_POLICY_PATH")) {
    if (*env) return env;
  }
  static const std::vector<std::string> candidates = {
      default_chat_policy_path(),
      "config/chat_policy.json",
      "../config/chat_policy.json",
      // v0.53.60: 移除硬编码开发机绝对路径(发布残留,他机无效且泄漏)
  };
  for (const auto& c : candidates) {
    if (std::filesystem::exists(c)) return c;
  }
  return candidates.front();
}

/// 重置进程内策略缓存，强制下次 chat_policy() 重新读盘。
void chat_policy_reset() {
  std::lock_guard<std::mutex> lk(g_policy_mu);  // v0.53.60
  g_cached_path.clear();
  g_cached = nlohmann::json::object();
}

const nlohmann::json& chat_policy() {
  const std::string path = chat_policy_path();
  {  // v0.53.60: 双检——快路径无锁读已缓存的引用是安全的(写只在锁内
     /// 且 reset 后引用失效由调用方短用语义保证,与旧版一致)
    std::lock_guard<std::mutex> lk(g_policy_mu);
    if (path == g_cached_path && !g_cached.is_null()) return g_cached;
  }
  // 慢路径:读盘(锁外 IO)+锁内提交
  nlohmann::json fresh = nlohmann::json::object();
  std::ifstream in(path);
  if (in.is_open()) {
    try { in >> fresh; } catch (...) { fresh = nlohmann::json::object(); }
  }
  std::lock_guard<std::mutex> lk(g_policy_mu);
  g_cached_path = path;
  g_cached = std::move(fresh);
  return g_cached;
}

/// 读取配置中任意点分路径的字符串数组节点。
std::vector<std::string> policy_string_list(const std::string& dot_path) {
  const auto* node = json_at_path(chat_policy(), dot_path);
  if (!node || !node->is_array()) return {};
  std::vector<std::string> out;
  out.reserve(node->size());
  for (const auto& it : *node) {
    if (it.is_string()) out.push_back(it.get<std::string>());
  }
  return out;
}

/// keywords.<key> 的便捷封装。
std::vector<std::string> policy_keywords(const std::string& key) {
  return policy_string_list("keywords." + key);
}

/// templates.<key> 文案读取，支持顶层 templates 对象或点分路径。
std::string policy_text(const std::string& key, const std::string& fallback) {
  const auto pick_variant = [](const nlohmann::json& node, const std::string& fb) -> std::string {
    if (node.is_string()) return node.get<std::string>();
    if (node.is_array()) {
      std::vector<std::string> variants;
      variants.reserve(node.size());
      for (const auto& it : node) {
        if (it.is_string()) variants.push_back(it.get<std::string>());
      }
      if (!variants.empty()) {
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, variants.size() - 1);
        return variants[dist(rng)];
      }
    }
    return fb;
  };

  const auto& p = chat_policy();
  if (p.contains("templates") && p["templates"].is_object()) {
    const auto& t = p["templates"];
    if (t.contains(key)) return pick_variant(t[key], fallback);
  }
  const auto* node = json_at_path(p, "templates." + key);
  if (node) return pick_variant(*node, fallback);
  return fallback;
}

/// 简单 {key} 占位符替换，与 AgentService::render_template 语义一致。
std::string render_policy_template(std::string tpl,
                                   const std::vector<std::pair<std::string, std::string>>& kv) {
  for (const auto& [k, v] : kv) {
    const std::string token = "{" + k + "}";
    std::string::size_type pos = 0;
    while ((pos = tpl.find(token, pos)) != std::string::npos) {
      tpl.replace(pos, token.size(), v);
      pos += v.size();
    }
  }
  return tpl;
}

// --- NLP 辅助与槽位后缀 ---

/// 查询类口语前缀列表（剥离用户输入填充词）。
std::vector<std::string> query_filler_prefixes() {
  auto list = policy_string_list("nlp.query_filler_prefix");
  if (list.empty()) {
    list = {"请", "帮我", "给我", "麻烦", "做", "进行", "来个", "来一段", "一下", "下",
            "please", "do", "run"};
  }
  return list;
}

/// 读取 slots.*.query_suffix，缺失时用 fallback。
std::string slot_query_suffix(const char* slot_path, const char* fallback) {
  const auto* node = json_at_path(chat_policy(), slot_path);
  if (node && node->is_string()) return node->get<std::string>();
  return fallback;
}

/// 天气槽位续接后缀，见 ChatPolicy.h。
std::string weather_query_suffix() {
  return slot_query_suffix("slots.weather.query_suffix", "的天气");
}

/// 新闻槽位续接后缀，见 ChatPolicy.h。
std::string news_query_suffix() {
  return slot_query_suffix("slots.news.query_suffix", "新闻");
}

/// profile 详细模式触发词列表。
std::vector<std::string> profile_detail_markers() {
  auto list = policy_string_list("profile.detail_markers");
  if (list.empty()) list = {"详细"};
  return list;
}

// --- 云端策略与穿衣建议 ---

/// 云策略顾问 system prompt。
std::string cloud_strategist_system_prompt() {
  const std::string tpl = policy_text("cloud.strategist_system", "");
  if (!tpl.empty()) return tpl;
  return "Return a single JSON object for thin_agent cloud strategy.";
}

/// 云策略 JSON 解析失败时的启发式词表。
std::vector<std::string> cloud_parse_heuristics(const std::string& category) {
  auto list = policy_string_list("routing.cloud_parse_heuristics." + category);
  if (!list.empty()) return list;
  static const std::unordered_map<std::string, std::vector<std::string>> kDefaults = {
      {"clarify", {"clarify", "need clarify", "澄清", "请确认", "需要确认", "ask user"}},
      {"local_status", {"route:local_status", "local_status route"}},
      {"local_profile", {"route:local_profile", "local_profile route", "who are you", "你是谁", "你有哪些能力"}},
      {"local_task_capture", {"route:capture_photo", "route:local_task_inline_capture_photo"}},
      {"local_task_recording", {"route:start_recording", "route:local_task_inline_start_recording"}},
      {"high_risk", {"high risk", "高风险", "sensitive", "敏感"}},
      {"low_risk", {"low risk", "低风险"}},
      {"reject", {"reject", "deny", "blocked", "refuse", "拒绝", "阻断", "禁止"}},
  };
  if (const auto it = kDefaults.find(category); it != kDefaults.end()) return it->second;
  return {};
}

/// 按气温档位与语言渲染穿衣/出行建议文案。
std::string render_weather_advice(const std::string& city,
                                  int temp_c,
                                  const std::string& condition,
                                  int humidity,
                                  const std::string& lang) {
  const bool en = (lang == "en");
  const char* band = "cold";
  if (temp_c >= 35) band = "35";
  else if (temp_c >= 28) band = "28";
  else if (temp_c >= 20) band = "20";
  else if (temp_c >= 10) band = "10";

  const std::string key = std::string("weather.advice.") + band + (en ? ".en" : ".zh");
  std::string advice = render_policy_template(
      policy_text(key, ""),
      {{"city", city},
       {"temp_c", std::to_string(temp_c)},
       {"condition", condition}});

  if (humidity >= 80 &&
      (condition.find("雨") != std::string::npos ||
       condition.find("rain", 0) != std::string::npos)) {
    advice += policy_text(en ? "weather.advice.rain_extra.en" : "weather.advice.rain_extra.zh", "");
  }
  return advice;
}

// --- 槽位检测与路由探测 ---

/// 展开常见聊天缩写（u→you, r→are 等）。配置来源 nlp.chat_abbreviations。
/// 采用全词匹配，避免 "unique" 被误展开为 "youunique"。
std::string expand_chat_abbreviations(std::string text) {
  const auto* node = json_at_path(chat_policy(), "nlp.chat_abbreviations");
  if (!node || !node->is_object() || node->empty()) return text;

  // 收集缩写映射并缓存静态副本
  static std::vector<std::pair<std::string, std::string>> s_map;
  static std::string s_policy_path;
  const std::string cur_path = chat_policy_path();
  if (s_policy_path != cur_path) {
    s_map.clear();
    for (auto it = node->begin(); it != node->end(); ++it) {
      if (it.value().is_string()) {
        s_map.emplace_back(it.key(), it.value().get<std::string>());
      }
    }
    // 按缩写长度降序排列，避免短缩写先替换破坏长缩写（如 "ur" 先于 "u"）
    std::sort(s_map.begin(), s_map.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    s_policy_path = cur_path;
  }

  for (const auto& [abbr, full] : s_map) {
    size_t pos = 0;
    while ((pos = text.find(abbr, pos)) != std::string::npos) {
      // 全词匹配检查
      const bool left_ok = (pos == 0 || text[pos - 1] == ' ');
      const bool right_ok = (pos + abbr.size() >= text.size() || text[pos + abbr.size()] == ' ');
      if (left_ok && right_ok) {
        text.replace(pos, abbr.size(), full);
        pos += full.size();
      } else {
        pos += abbr.size();
      }
    }
  }
  return text;
}

/// 策略侧文本归一化：去标点、缩写展开、小写、trim。
std::string normalize_text_for_policy(std::string text) {
  for (const auto& p : punct_list()) {
    std::string::size_type pos = 0;
    while ((pos = text.find(p, pos)) != std::string::npos) {
      text.replace(pos, p.size(), " ");
      pos += 1;
    }
  }
  text = expand_chat_abbreviations(std::move(text));
  return trim_copy(to_lower_copy(std::move(text)));
}

/// 获取意图分类器 system prompt（按语言）。
std::string intent_classifier_prompt(const std::string& lang) {
  const std::string key = lang == "zh" ? "intent_classifier.prompt_zh" : "intent_classifier.prompt_en";
  const auto* node = json_at_path(chat_policy(), key);
  if (node && node->is_string()) return node->get<std::string>();
  return "";
}

/// 查询 intent_map：该意图是否有本地 handler。
bool intent_has_local_handler(const std::string& intent) {
  const auto* node = json_at_path(chat_policy(), "intent_map." + intent + ".has_local_handler");
  return node && node->is_boolean() && node->get<bool>();
}

/// 判断 UTF-8 字符串是否至少包含 min_chars 个字符。
bool utf8_char_count_at_least(const std::string& s, size_t min_chars) {
  size_t count = 0;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) i += 1;
    else if ((c & 0xE0) == 0xC0) i += 2;
    else if ((c & 0xF0) == 0xE0) i += 3;
    else if ((c & 0xF8) == 0xF0) i += 4;
    else i += 1;
    ++count;
    if (count >= min_chars) return true;
  }
  return count >= min_chars;
}

// --- 槽位检测（天气 / 新闻 / 续问）---

/// 从归一化文本检测天气城市槽位。
std::string detect_city_slot(const std::string& norm) {
  return match_slot_entries(norm, load_weather_cities());
}

/// 从归一化文本检测新闻主题槽位。
std::string detect_news_topic_slot(const std::string& norm) {
  return match_slot_entries(norm, load_news_topics());
}

/// 多轮续问短句中提取城市（先全句匹配，再按后缀截取；仅接受城市表白名单）。
std::string extract_city_followup(const std::string& text, const std::string& norm) {
  std::string city = detect_city_slot(norm);
  if (!city.empty()) return city;

  std::string cleaned = text;
  for (const auto& suffix : weather_followup_suffixes()) {
    const auto pos = cleaned.find(suffix);
    if (pos != std::string::npos && pos >= 2) {
      std::string cand = trim_copy(cleaned.substr(0, pos));
      // v0.53.16: 全角？字节串比较（-Wmultichar）
    while (!cand.empty() && (cand.back() == '?' ||
           cand.size() >= 3 && cand.compare(cand.size()-3, 3, "\xEF\xBC\x9F") == 0)) cand.pop_back();
      if (!utf8_char_count_at_least(cand, 2) || cand.size() > 36) continue;
      const std::string cand_norm = normalize_text_for_policy(cand);
      if (is_weather_date_token(cand_norm)) continue;
      const std::string known = detect_city_slot(cand_norm);
      if (!known.empty()) return known;
    }
  }
  return "";
}

/// 判断 token 是否为天气日期词（今天/明天等）。
bool is_weather_date_token(const std::string& token_norm) {
  if (token_norm.empty()) return false;
  const auto groups = load_alias_groups(
      "slots.weather.date_aliases",
      {{{"今天", "今日"}, "今天"}, {{"明天"}, "明天"}});
  for (const auto& g : groups) {
    for (const auto& m : g.match) {
      const std::string alias_norm = normalize_text_for_policy(m);
      if (token_norm == alias_norm || token_norm == g.value) return true;
    }
  }
  return false;
}

/// 解析合法天气城市：须在白名单内且排除日期词。
std::string resolve_weather_city_candidate(const std::string& text, const std::string& norm) {
  std::string city = detect_city_slot(norm);
  if (!city.empty() && !is_weather_date_token(normalize_text_for_policy(city))) return city;

  city = extract_city_followup(text, norm);
  if (city.empty()) return "";

  const std::string city_norm = normalize_text_for_policy(city);
  if (is_weather_date_token(city_norm)) return "";
  const std::string known = detect_city_slot(city_norm);
  return known;
}

/// 澄清态下用户是否明显切换了意图。
bool should_abort_slot_for_intent_switch(const std::string& norm, const std::string& pending_intent) {
  auto contains_kw = [&](const std::string& key) {
    for (const auto& kw : policy_keywords(key)) {
      const std::string needle = normalize_text_for_policy(kw);
      if (!needle.empty() && norm.find(needle) != std::string::npos) return true;
    }
    return false;
  };

  if (pending_intent == "weather") {
    if (contains_kw("news")) return true;
    if (contains_kw("profile") || contains_kw("profile_short") || contains_kw("profile_detail")) return true;
    if (contains_kw("status") || contains_kw("model_status")) return true;
    if (contains_kw("event")) return true;
    if (contains_kw("memory_recent") || contains_kw("memory_history") ||
        contains_kw("memory_search") || contains_kw("memory_summary")) {
      return true;
    }
    if (contains_kw("action_capture") || contains_kw("action_start_recording") ||
        contains_kw("task_capture_inline") || contains_kw("task_start_recording_inline")) {
      return true;
    }
  }
  if (pending_intent == "news") {
    if (contains_kw("weather") || is_weather_extra_trigger(norm)) return true;
  }
  return false;
}

/// 解析天气日期槽（今天/明天等别名组）。
std::string resolve_weather_date_slot(const std::string& norm) {
  const auto groups = load_alias_groups(
      "slots.weather.date_aliases",
      {{{"今天", "今日"}, "今天"}, {{"明天"}, "明天"}});
  return match_alias_groups(norm, groups);
}

/// 解析新闻时间范围槽（最近/本周等别名组）。
std::string resolve_news_time_range_slot(const std::string& norm) {
  const auto groups = load_alias_groups(
      "slots.news.time_range_aliases",
      {{{"今天", "今日"}, "今天"}, {{"最近", "近24小时", "24小时"}, "最近"}});
  return match_alias_groups(norm, groups);
}

/// 天气续接后缀词列表。
std::vector<std::string> weather_followup_suffixes() {
  auto list = policy_string_list("slots.weather.followup_suffixes");
  if (list.empty()) {
    list = {"的呢", "的天气", "天气", "呢", "怎么样", "如何"};
  }
  return list;
}

/// 城市槽位尾部助词列表。
std::vector<std::string> weather_slot_trailing_particles() {
  auto list = policy_string_list("slots.weather.trailing_particles");
  if (list.empty()) list = {"的"};
  return list;
}

/// 天气建议确认回复词列表。
std::vector<std::string> weather_confirm_replies() {
  auto list = policy_string_list("slots.weather.confirm_replies");
  if (list.empty()) {
    list = {"需要", "好的", "好", "行", "是", "是的", "对", "对的",
            "yes", "ok", "okay", "yep", "yeah", "要", "可以", "可", "嗯"};
  }
  return list;
}

/// 新闻续接后缀词列表。
std::vector<std::string> news_followup_suffixes() {
  auto list = policy_string_list("slots.news.followup_suffixes");
  if (list.empty()) list = {"新闻", "消息", "呢", "方面的"};
  return list;
}

// --- 路由探测（meta 问题 / 复杂意图 / 云 hint 白名单）---

/// 弱天气触发表达（slots.weather.extra_triggers）。
bool is_weather_extra_trigger(const std::string& norm) {
  for (const auto& t : policy_string_list("slots.weather.extra_triggers")) {
    if (norm.find(to_lower_copy(t)) != std::string::npos) return true;
  }
  const auto kws = policy_keywords("weather");
  for (const auto& t : kws) {
    const std::string lower = to_lower_copy(t);
    if (lower.size() >= 4 && norm.find(lower) != std::string::npos) return true;
  }
  return norm.find("weather") != std::string::npos || norm.find("wether") != std::string::npos;
}

/// 判断是否为端云分工类 meta 问题（含云关键词但排除纯身份介绍）。
bool is_cloud_meta_question(const std::string& text) {
  const std::string norm = normalize_text_for_policy(text);
  auto cloud_kws = policy_string_list("meta_questions.cloud_keywords");
  if (cloud_kws.empty()) {
    cloud_kws = {"云端", "云模型", "大模型", "转云端", "cloud", "还会用云", "不会用云", "都不会用",
                 "不用云", "用云", "走云", "调用云", "连不上云"};
  }
  auto identity_kws = policy_string_list("meta_questions.identity_exclude");
  if (identity_kws.empty()) {
    identity_kws = {"你是谁", "你是哪个", "介绍自己", "自我介绍", "who are you", "what are you"};
  }

  bool asks_cloud = false;
  for (const auto& kw : cloud_kws) {
    if (norm.find(to_lower_copy(kw)) != std::string::npos) {
      asks_cloud = true;
      break;
    }
  }
  if (!asks_cloud) return false;

  for (const auto& kw : identity_kws) {
    if (norm.find(to_lower_copy(kw)) != std::string::npos) return false;
  }
  return true;
}

/// 复杂意图子串标记表，用于 force_cloud 门控探测。
std::vector<std::pair<std::string, std::string>> complex_intent_markers() {
  const auto* node = json_at_path(chat_policy(), "routing.complex_intent_markers");
  if (node && node->is_array() && !node->empty()) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& item : *node) {
      if (!item.is_object()) continue;
      const std::string text = item.value("text", "");
      const std::string tag = item.value("tag", "");
      if (!text.empty() && !tag.empty()) out.emplace_back(text, tag);
    }
    if (!out.empty()) return out;
  }
  return {
      {"如果", "cn_if"}, {"先", "cn_first"}, {"然后", "cn_then"}, {"否则", "cn_else"},
      {"if", "en_if"}, {"then", "en_then"}, {"else", "en_else"},
  };
}

/// 云策略 local_route_hint 允许值集合（带进程内缓存）。
std::unordered_set<std::string> cloud_allowed_route_hints() {
  static std::unordered_set<std::string> cached;
  static std::string cached_path;
  const std::string path = chat_policy_path();
  if (path == cached_path && !cached.empty()) return cached;

  cached.clear();
  cached_path = path;
  const auto list = policy_string_list("routing.cloud_allowed_route_hints");
  if (!list.empty()) {
    cached.insert(list.begin(), list.end());
    return cached;
  }
  cached = {"", "local_profile", "local_status", "local_supported_languages",
            "local_external_weather", "local_external_news",
            "local_task_inline_capture_photo", "local_task_inline_start_recording",
            "local_task_pipeline"};
  return cached;
}

/// Fast 快速通道配置（从 chat_policy.json fast_path 读取，带进程内缓存）。
FastPathConfig fast_path_config() {
  static FastPathConfig cached;
  static std::string cached_path;
  const std::string path = chat_policy_path();
  if (path == cached_path) return cached;

  cached = FastPathConfig{};
  cached_path = path;
  const auto& p = chat_policy();
  if (!p.contains("fast_path") || !p["fast_path"].is_object()) return cached;

  const auto& f = p["fast_path"];
  if (f.contains("model") && f["model"].is_string())
    cached.model = f["model"].get<std::string>();
  if (f.contains("timeout_ms") && f["timeout_ms"].is_number_integer())
    cached.timeout_ms = f["timeout_ms"].get<int>();
  if (f.contains("max_tool_rounds") && f["max_tool_rounds"].is_number_integer())
    cached.max_tool_rounds = f["max_tool_rounds"].get<int>();
  if (f.contains("prompt_zh") && f["prompt_zh"].is_string())
    cached.prompt_zh = f["prompt_zh"].get<std::string>();
  if (f.contains("prompt_en") && f["prompt_en"].is_string())
    cached.prompt_en = f["prompt_en"].get<std::string>();
  cached.file_keywords = policy_string_list("fast_path.file_keywords");
  cached.complex_exclude = policy_string_list("fast_path.complex_exclude");
  cached.multi_step_markers = policy_string_list("fast_path.multi_step_markers");
  return cached;
}

// --- 天气/城市本地化 ---

/// 按 lang 本地化城市显示名：zh 时英文→中文，en 时中文→英文，其它保持原样。
std::string localize_city_name(std::string city, const std::string& lang) {
  if (lang == "zh") {
    const auto& mp = cities_zh_map();
    const std::string lower = to_lower_copy(city);
    if (auto it = mp.find(lower); it != mp.end()) return it->second;
    return city;
  }
  if (lang == "en") {
    const auto& mp = cities_en_map();
    if (auto it = mp.find(city); it != mp.end()) return it->second;
    return city;
  }
  return city;
}

/// 按 lang 本地化天气日期槽：en 时中文日期词转英文，其它保持原样。
std::string localize_weather_date(const std::string& date, const std::string& lang) {
  if (lang != "en") return date;
  if (date == "今天" || date == "今日") return "today";
  if (date == "明天") return "tomorrow";
  if (date == "后天") return "the day after tomorrow";
  return date;
}

/// 按 lang 本地化天气状况描述（zh 时段落拆分后逐段翻译）。
std::string localize_weather_condition(std::string condition, const std::string& lang) {
  if (lang != "zh") return condition;

  std::string out;
  std::string part;
  auto flush = [&]() {
    if (part.empty()) return;
    const std::string zh = lookup_weather_zh_part(part);
    if (!out.empty()) out += "，";
    out += zh;
    part.clear();
  };
  for (size_t i = 0; i <= condition.size(); ++i) {
    const bool at_end = i == condition.size();
    const unsigned char ch = at_end ? ',' : static_cast<unsigned char>(condition[i]);
    if (ch == ',' || ch == 0xEF) {
      if (!at_end && i + 2 < condition.size() &&
          static_cast<unsigned char>(condition[i]) == 0xEF &&
          static_cast<unsigned char>(condition[i + 1]) == 0xBC &&
          static_cast<unsigned char>(condition[i + 2]) == 0x8C) {
        flush();
        i += 2;
        continue;
      }
      if (ch == ',') {
        flush();
        continue;
      }
    }
    if (!at_end) part.push_back(static_cast<char>(ch));
  }
  flush();
  return out.empty() ? condition : out;
}

// --- 27 语支持 / 15 语模板 ---

namespace {

const nlohmann::json* languages_node() { return json_at_path(chat_policy(), "languages"); }

std::string pick_template_variant(const nlohmann::json& node, const std::string& fb) {
  if (node.is_string()) return node.get<std::string>();
  if (node.is_array()) {
    std::vector<std::string> variants;
    variants.reserve(node.size());
    for (const auto& it : node) {
      if (it.is_string()) variants.push_back(it.get<std::string>());
    }
    if (!variants.empty()) {
      static thread_local std::mt19937 rng(std::random_device{}());
      std::uniform_int_distribution<size_t> dist(0, variants.size() - 1);
      return variants[dist(rng)];
    }
  }
  return fb;
}

std::string template_key_for_lang(const std::string& key, const std::string& lang) {
  if (lang.empty() || lang == "zh") return key;
  return key + "_" + lang;
}

bool contains_any_substr(const std::string& haystack, const std::vector<std::string>& needles) {
  for (const auto& n : needles) {
    if (!n.empty() && haystack.find(n) != std::string::npos) return true;
  }
  return false;
}

bool has_any_utf8_marker(const std::string& s, const std::vector<const char*>& markers) {
  for (const char* m : markers) {
    if (m && *m && s.find(m) != std::string::npos) return true;
  }
  return false;
}

std::string detect_latin_query_lang(const std::string& norm, const std::string& raw) {
  // 配置驱动的 latin hints：languages.latin_hints 为对象 { "fr": ["bonjour", ...], "pl": ["pogoda", ...] }
  if (const auto* hints = json_at_path(chat_policy(), "languages.latin_hints");
      hints && hints->is_object()) {
    for (auto it = hints->begin(); it != hints->end(); ++it) {
      if (!it.value().is_array()) continue;
      const std::string code = it.key();
      for (const auto& h : it.value()) {
        if (!h.is_string()) continue;
        const std::string needle = normalize_text_for_policy(h.get<std::string>());
        if (!needle.empty() && norm.find(needle) != std::string::npos) return code;
      }
    }
  }

  // 配置未覆盖时再用字符特征兜底（仍然不包含具体业务词表）。
  if (has_any_utf8_marker(raw, {"ą", "ć", "ę", "ł", "ń", "ó", "ś", "ź", "ż"})) return "pl";
  if (has_any_utf8_marker(raw, {"ě", "ř", "ů"})) return "cs";
  if (has_any_utf8_marker(raw, {"ő", "ű"})) return "hu";
  if (has_any_utf8_marker(raw, {"õ"})) return "et";
  if (has_any_utf8_marker(raw, {"ä"}) &&
      !has_any_utf8_marker(raw, {"ą", "ć", "ę", "ł", "ń", "ś", "ź", "ż", "ő", "ű", "õ"})) {
    return "fi";
  }
  if (has_any_utf8_marker(raw, {"æ", "ø", "å"})) return "no";
  if (norm.find("nyhed") != std::string::npos || norm.find("vejr") != std::string::npos) return "da";
  if (has_any_utf8_marker(raw, {"ą", "č", "ė", "į", "ų"})) return "lt";
  if (has_any_utf8_marker(raw, {"ģ", "ķ", "ļ", "ņ"})) return "lv";
  if (norm.find("sprava") != std::string::npos || has_any_utf8_marker(raw, {"ľ", "ô"})) return "sk";

  if (norm.find("cuaca") != std::string::npos || norm.find("berita") != std::string::npos) return "ms";

  return "en";
}

}  // namespace

std::string policy_text_for_lang(const std::string& key,
                                 const std::string& template_lang,
                                 const std::string& fallback) {
  const auto& p = chat_policy();
  if (!p.contains("templates") || !p["templates"].is_object()) {
    return policy_text(key, fallback);
  }
  const auto& t = p["templates"];

  const std::string localized = template_key_for_lang(key, template_lang);
  if (t.contains(localized)) return pick_template_variant(t[localized], "");

  // 繁体无专用变体时回退简体 zh，而非英文 en。
  if (template_lang == "zh-TW" && t.contains(key)) {
    return pick_template_variant(t[key], fallback);
  }

  if (template_lang != "en" && template_lang != "zh" && template_lang != "zh-TW" &&
      t.contains(template_key_for_lang(key, "en"))) {
    return pick_template_variant(t[template_key_for_lang(key, "en")], "");
  }
  if (t.contains(key)) return pick_template_variant(t[key], fallback);
  return fallback;
}

bool is_template_language(const std::string& code) {
  const auto* node = languages_node();
  if (!node || !node->contains("template_codes") || !(*node)["template_codes"].is_array()) {
    static const std::unordered_set<std::string> kDefault = {
        "en", "zh", "zh-TW", "ja", "ko", "th", "de", "fr", "nl", "sv", "ru", "it", "es", "pt"};
    return kDefault.count(code) > 0;
  }
  for (const auto& it : (*node)["template_codes"]) {
    if (it.is_string() && it.get<std::string>() == code) return true;
  }
  return false;
}

size_t supported_language_count() {
  const auto* node = languages_node();
  if (!node || !node->contains("supported") || !(*node)["supported"].is_array()) return 29;
  return (*node)["supported"].size();
}

std::string detect_query_language(const std::string& text) {
  bool has_han = false, has_kana = false, has_hangul = false, has_thai = false;
  bool has_cyrillic = false, has_arabic = false, has_vietnamese = false, has_latin = false;
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    uint32_t cp = 0;
    int len = 1;
    if (c < 0x80) {
      cp = c;
      len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      len = 4;
    } else {
      i += 1;
      continue;
    }
    if (i + static_cast<size_t>(len) > n) break;
    for (int k = 1; k < len; ++k) {
      cp = (cp << 6) | (static_cast<unsigned char>(text[i + static_cast<size_t>(k)]) & 0x3F);
    }
    i += static_cast<size_t>(len);
    if (cp <= 0x7F) {
      if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) has_latin = true;
    } else if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF)) {
      has_kana = true;
    } else if ((cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x1100 && cp <= 0x11FF) ||
               (cp >= 0x3130 && cp <= 0x318F)) {
      has_hangul = true;
    } else if (cp >= 0x0E00 && cp <= 0x0E7F) {
      has_thai = true;
    } else if ((cp >= 0x0400 && cp <= 0x04FF) || (cp >= 0x0500 && cp <= 0x052F)) {
      has_cyrillic = true;
    } else if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F)) {
      has_arabic = true;
    } else if (cp >= 0x0100 && cp <= 0x024F) {
      has_vietnamese = true;
    } else if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
               (cp >= 0xF900 && cp <= 0xFAFF)) {
      has_han = true;
    }
  }

  if (has_kana) return "ja";
  if (has_hangul) return "ko";
  if (has_thai) return "th";
  if (has_arabic) return "ar";
  if (has_cyrillic) {
    // 西里尔子检测：哈萨克语特有字符 / 乌克兰语特有字符
    bool has_kz = false, has_uk = false;
    i = 0;
    while (i < n) {
      const unsigned char c2 = static_cast<unsigned char>(text[i]);
      uint32_t cp2 = 0;
      int len2 = 1;
      if (c2 < 0x80) { cp2 = c2; len2 = 1; }
      else if ((c2 & 0xE0) == 0xC0) { cp2 = c2 & 0x1F; len2 = 2; }
      else if ((c2 & 0xF0) == 0xE0) { cp2 = c2 & 0x0F; len2 = 3; }
      else if ((c2 & 0xF8) == 0xF0) { cp2 = c2 & 0x07; len2 = 4; }
      else { i += 1; continue; }
      if (i + static_cast<size_t>(len2) > n) break;
      for (int k = 1; k < len2; ++k)
        cp2 = (cp2 << 6) | (static_cast<unsigned char>(text[i + static_cast<size_t>(k)]) & 0x3F);
      i += static_cast<size_t>(len2);
      // 哈萨克语特有 (Әә=0x4D8-9, Ғғ=0x492-3, Ққ=0x49A-B, Ңң=0x4A2-3, Өө=0x4E8-9, Ұұ=0x4B0-1, Үү=0x4AE-F, Һһ=0x4BA-B)
      if ((cp2 >= 0x04D8 && cp2 <= 0x04D9) || (cp2 >= 0x0492 && cp2 <= 0x0493) ||
          (cp2 >= 0x049A && cp2 <= 0x049B) || (cp2 >= 0x04A2 && cp2 <= 0x04A3) ||
          (cp2 >= 0x04E8 && cp2 <= 0x04E9) || (cp2 >= 0x04B0 && cp2 <= 0x04B1) ||
          (cp2 >= 0x04AE && cp2 <= 0x04AF) || (cp2 >= 0x04BA && cp2 <= 0x04BB))
        has_kz = true;
      // 乌克兰语特有 (Її=0x407/0x457, Єє=0x404/0x454, Ґґ=0x490-1)
      if (cp2 == 0x0407 || cp2 == 0x0457 || cp2 == 0x0404 || cp2 == 0x0454 ||
          cp2 == 0x0490 || cp2 == 0x0491)
        has_uk = true;
    }
    if (has_kz && !has_uk) return "kk";
    if (has_uk && !has_kz) return "uk";
    return "ru";
  }
  if (has_vietnamese || has_any_utf8_marker(text, {"ă", "â", "đ", "ê", "ô", "ơ", "ư"})) return "vi";
  if (has_han) return "zh";
  if (has_latin) return detect_latin_query_lang(normalize_text_for_policy(text), text);
  return "en";
}

bool is_traditional_chinese_preferred(const std::string& text) {
  const auto* node = languages_node();
  std::vector<std::string> markers;
  if (node && node->contains("traditional_markers") && (*node)["traditional_markers"].is_array()) {
    for (const auto& it : (*node)["traditional_markers"]) {
      if (it.is_string()) markers.push_back(it.get<std::string>());
    }
  }
  if (markers.empty()) {
    markers = {"繁體", "繁体", "臺灣", "台湾", "香港", "澳門", "澳门", "軟體", "網路", "程式", "資訊"};
  }
  return contains_any_substr(text, markers);
}

std::string pick_template_lang(const std::string& query_lang, const std::string& text) {
  if (query_lang == "zh") {
    return is_traditional_chinese_preferred(text) ? "zh-TW" : "zh";
  }
  if (is_template_language(query_lang)) return query_lang;
  return "en";
}

bool is_supported_languages_query(const std::string& text) {
  const std::string norm = normalize_text_for_policy(text);
  auto keywords = policy_string_list("languages.query_keywords");
  if (keywords.empty()) {
    keywords = {"支持哪些语言", "支持什么语言", "多少种语言", "几种语言", "what languages",
                "which languages", "language support", "supported languages"};
  }
  for (const auto& kw : keywords) {
    if (kw.empty()) continue;
    const std::string k = normalize_text_for_policy(kw);
    if (norm.find(k) != std::string::npos) return true;
  }
  return false;
}

std::string language_label_for_entry(const nlohmann::json& entry, const std::string& template_lang) {
  if (!entry.is_object()) return "";
  if (template_lang == "zh" || template_lang == "zh-TW") {
    return entry.value("label_zh", entry.value("label_en", ""));
  }
  return entry.value("label_en", entry.value("label_zh", ""));
}

std::string render_supported_languages_answer(const std::string& template_lang) {
  const auto* node = languages_node();
  if (!node || !node->contains("supported") || !(*node)["supported"].is_array()) return "";

  std::vector<std::string> labels;
  for (const auto& entry : (*node)["supported"]) {
    const std::string label = language_label_for_entry(entry, template_lang);
    if (!label.empty()) labels.push_back(label);
  }

  std::string list_str;
  const std::string sep = (template_lang == "en") ? ", " : "、";
  for (size_t i = 0; i < labels.size(); ++i) {
    if (i > 0) list_str += sep;
    list_str += labels[i];
  }

  std::string tpl;
  if (template_lang == "zh-TW" && node->contains("supported_answer_zh-TW") &&
      (*node)["supported_answer_zh-TW"].is_string()) {
    tpl = (*node)["supported_answer_zh-TW"].get<std::string>();
  } else if (template_lang == "en" && node->contains("supported_answer_en") &&
             (*node)["supported_answer_en"].is_string()) {
    tpl = (*node)["supported_answer_en"].get<std::string>();
  } else if (node->contains("supported_answer_zh") && (*node)["supported_answer_zh"].is_string()) {
    tpl = (*node)["supported_answer_zh"].get<std::string>();
  } else {
    tpl = "Supported languages ({count}): {list}";
  }

  return render_policy_template(
      std::move(tpl),
      {{"count", std::to_string(labels.size())}, {"list", list_str}});
}

std::string language_display_name(const std::string& lang) {
  static const std::unordered_map<std::string, std::string> kNames = {
      {"en", "English"}, {"zh", "Chinese"}, {"zh-TW", "Traditional Chinese"},
      {"ja", "Japanese"}, {"ko", "Korean"}, {"th", "Thai"},
      {"de", "German"}, {"fr", "French"}, {"nl", "Dutch"}, {"sv", "Swedish"},
      {"ru", "Russian"}, {"it", "Italian"}, {"es", "Spanish"}, {"pt", "Portuguese"},
      {"ar", "Arabic"}, {"pl", "Polish"}, {"ms", "Malay"}, {"vi", "Vietnamese"},
      {"fi", "Finnish"}, {"no", "Norwegian"}, {"da", "Danish"},
      {"cs", "Czech"}, {"hu", "Hungarian"}, {"sk", "Slovak"},
      {"lt", "Lithuanian"}, {"lv", "Latvian"}, {"et", "Estonian"},
      {"kk", "Kazakh"}, {"uk", "Ukrainian"},
  };
  if (const auto it = kNames.find(lang); it != kNames.end()) return it->second;
  return "";
}

std::string reply_target_language(const std::string& query_lang, const std::string& template_lang) {
  if (query_lang == "zh") return template_lang;
  return query_lang;
}

bool needs_reply_translation(const std::string& query_lang, const std::string& template_lang) {
  return reply_target_language(query_lang, template_lang) != template_lang;
}

bool load_intent_spec(const std::string& intent_name, IntentSpec* out) {
  if (!out || intent_name.empty()) return false;
  *out = IntentSpec{};
  out->name = intent_name;
  const auto* node = json_at_path(chat_policy(), "intents." + intent_name);
  if (!node || !node->is_object()) return false;

  out->kind = node->value("kind", "");
  out->primary_slot = node->value("primary_slot", "");
  out->slot_detector = node->value("slot_detector", "");
  out->dialog_slot_policy = node->value("dialog_slot_policy", "same_intent");
  out->clarify_template_key = node->value("clarify_template_key", "");
  out->fetch_failed_template_key = node->value("fetch_failed_template_key", "");
  out->summary_template_key = node->value("summary_template_key", "");
  out->route_execute = node->value("route_execute", "");
  out->route_clarify = node->value("route_clarify", "local_external_clarify");
  out->fetcher = node->value("fetcher", "");
  out->handler = node->value("handler", "");
  out->keyword_key = node->value("keyword_key", "");
  out->predicate = node->value("predicate", "");
  out->gate_mode = node->value("gate_mode", "");
  out->action_name = node->value("action_name", "");
  out->intent_label = node->value("intent_label", "");
  out->query_suffix = node->value("query_suffix", "");
  out->post_execute = node->value("post_execute", "");
  out->boost_on_slot_filled = node->value("boost_on_slot_filled", true);
  out->require_execute_threshold = node->value("require_execute_threshold", false);
  out->default_confidence = node->value("default_confidence", 0.95);

  if (node->contains("required_slots") && (*node)["required_slots"].is_array()) {
    for (const auto& s : (*node)["required_slots"]) {
      if (s.is_string()) out->required_slots.push_back(s.get<std::string>());
    }
  }
  if (out->primary_slot.empty() && !out->required_slots.empty()) {
    out->primary_slot = out->required_slots.front();
  }

  if (node->contains("optional_slots") && (*node)["optional_slots"].is_object()) {
    out->optional_slots = (*node)["optional_slots"];
  }

  if (node->contains("route_excludes") && (*node)["route_excludes"].is_array()) {
    for (const auto& ex : (*node)["route_excludes"]) {
      if (ex.is_string()) out->route_excludes.push_back(ex.get<std::string>());
    }
  }

  if (node->contains("reasons") && (*node)["reasons"].is_object()) {
    const auto& reasons = (*node)["reasons"];
    out->reason_missing = reasons.value("missing", intent_name + "_missing_slot");
    out->reason_conf_below = reasons.value("conf_below", intent_name + "_confidence_below_execute");
    out->reason_fetch_ok = reasons.value("fetch_ok", intent_name + "_fetch_ok");
    out->reason_fetch_failed = reasons.value("fetch_failed", intent_name + "_fetch_failed");
    out->reason_execute = reasons.value("execute", intent_name + "_intent");
    out->reason_execute_detail = reasons.value("execute_detail", out->reason_execute);
    out->reason_budget_input = reasons.value("budget_input", "budget_input_exceeded");
    out->reason_budget_latency = reasons.value("budget_latency", "budget_latency_exceeded");
    out->reason_budget_cost = reasons.value("budget_cost", "budget_cost_exceeded");
  } else {
    out->reason_missing = intent_name + "_missing_slot";
    out->reason_conf_below = intent_name + "_confidence_below_execute";
    out->reason_fetch_ok = intent_name + "_fetch_ok";
    out->reason_fetch_failed = intent_name + "_fetch_failed";
    out->reason_execute = intent_name + "_intent";
    out->reason_execute_detail = out->reason_execute;
    out->reason_budget_input = "budget_input_exceeded";
    out->reason_budget_latency = "budget_latency_exceeded";
    out->reason_budget_cost = "budget_cost_exceeded";
  }
  if (out->handler.empty() && out->kind == "local") {
    out->handler = intent_name;
  }
  if (out->keyword_key.empty() && out->kind == "local") {
    out->keyword_key = intent_name;
  }
  if (out->intent_label.empty()) {
    out->intent_label = intent_name;
  }
  return true;
}

bool is_external_intent(const std::string& intent_name) {
  IntentSpec spec;
  return load_intent_spec(intent_name, &spec) && spec.kind == "external";
}

bool is_local_intent(const std::string& intent_name) {
  IntentSpec spec;
  return load_intent_spec(intent_name, &spec) && spec.kind == "local";
}

std::vector<std::string> external_intent_names() {
  std::vector<std::string> out;
  const auto* node = json_at_path(chat_policy(), "intents");
  if (!node || !node->is_object()) return out;
  for (auto it = node->begin(); it != node->end(); ++it) {
    if (!it.value().is_object()) continue;
    if (it.value().value("kind", "") == "external") out.push_back(it.key());
  }
  return out;
}

std::vector<std::string> local_intent_names() {
  return intent_names_by_kind("local");
}

std::vector<std::string> intent_names_by_kind(const std::string& kind) {
  std::vector<std::string> out;
  const auto* node = json_at_path(chat_policy(), "intents");
  if (!node || !node->is_object()) return out;
  for (auto it = node->begin(); it != node->end(); ++it) {
    if (!it.value().is_object()) continue;
    if (it.value().value("kind", "") == kind) out.push_back(it.key());
  }
  return out;
}

bool load_intent_gate_policy(const std::string& intent_name, IntentGatePolicy* out) {
  if (!out || intent_name.empty()) return false;
  *out = IntentGatePolicy{};
  out->name = intent_name;

  const auto* node = json_at_path(chat_policy(), "intent_policy." + intent_name);
  if (node && node->is_object()) {
    out->family = node->value("family", "");
    out->side_effect = node->value("side_effect", false);
    out->requires_dialogue_act = node->value("requires_dialogue_act", "");
    out->requires_media_context = node->value("requires_media_context", false);
    out->on_mismatch = node->value("on_mismatch", "clarify");
    out->fuzzy_alias = node->value("fuzzy_alias", "");
    out->ok = true;
    return true;
  }

  // 回退：intent_anchors.<name> 上的 side_effect / requires_dialogue_act
  const auto* anchor = json_at_path(chat_policy(), "intent_anchors." + intent_name);
  if (anchor && anchor->is_object()) {
    out->family = anchor->value("family", "");
    out->side_effect = anchor->value("side_effect", false);
    out->requires_dialogue_act = anchor->value("requires_dialogue_act", "");
    out->requires_media_context = anchor->value("requires_media_context", false);
    out->on_mismatch = anchor->value("on_mismatch", out->side_effect ? "clarify" : "block_execute");
    out->ok = true;
    return true;
  }
  return false;
}

std::vector<std::string> intent_policy_names() {
  std::vector<std::string> out;
  const auto* node = json_at_path(chat_policy(), "intent_policy");
  if (!node || !node->is_object()) return out;
  for (auto it = node->begin(); it != node->end(); ++it) {
    if (it.key().empty() || it.key()[0] == '_') continue;
    if (!it.value().is_object()) continue;
    out.push_back(it.key());
  }
  return out;
}

bool intent_has_side_effect(const std::string& intent_name) {
  IntentGatePolicy pol;
  if (!load_intent_gate_policy(intent_name, &pol) || !pol.ok) return false;
  return pol.side_effect;
}

IntentGateDecision evaluate_intent_gate(const std::string& intent_name,
                                        const std::string& dialogue_act,
                                        bool has_media_context) {
  IntentGateDecision d;
  d.allow_execute = true;
  d.action = "allow";
  d.reason = "no_policy";

  IntentGatePolicy pol;
  if (!load_intent_gate_policy(intent_name, &pol) || !pol.ok) return d;

  if (pol.requires_media_context && !has_media_context) {
    d.allow_execute = false;
    d.action = pol.on_mismatch.empty() ? "clarify" : pol.on_mismatch;
    d.reason = "missing_media_context";
    return d;
  }

  if (!pol.requires_dialogue_act.empty() && !dialogue_act.empty() &&
      dialogue_act != pol.requires_dialogue_act) {
    d.allow_execute = false;
    d.action = pol.on_mismatch.empty() ? (pol.side_effect ? "clarify" : "block_execute") : pol.on_mismatch;
    d.reason = "dialogue_act_mismatch";
    return d;
  }

  // 副作用意图在 question 话轮下默认拦截（即使未写 requires，也兜底）
  if (pol.side_effect && dialogue_act == "question") {
    d.allow_execute = false;
    d.action = pol.on_mismatch.empty() ? "clarify" : pol.on_mismatch;
    d.reason = "side_effect_blocked_on_question";
    return d;
  }

  d.reason = "policy_allow";
  return d;
}

/// 输入翻译配置（从 chat_policy.json translate_input 读取，带进程内缓存）。
TranslateInputConfig translate_input_config() {
  static TranslateInputConfig cached;
  static std::string cached_path;
  const std::string path = chat_policy_path();
  if (path == cached_path) return cached;

  cached = TranslateInputConfig{};
  cached_path = path;
  const auto& p = chat_policy();
  if (!p.contains("translate_input") || !p["translate_input"].is_object()) return cached;

  const auto& t = p["translate_input"];
  if (t.contains("cloud_model") && t["cloud_model"].is_string())
    cached.cloud_model = t["cloud_model"].get<std::string>();
  if (t.contains("cloud_timeout_ms") && t["cloud_timeout_ms"].is_number_integer())
    cached.cloud_timeout_ms = t["cloud_timeout_ms"].get<int>();
  if (t.contains("cloud_prompt") && t["cloud_prompt"].is_string())
    cached.cloud_prompt = t["cloud_prompt"].get<std::string>();
  if (t.contains("local_max_tokens") && t["local_max_tokens"].is_number_integer())
    cached.local_max_tokens = t["local_max_tokens"].get<int>();
  if (t.contains("min_length_for_detect") && t["min_length_for_detect"].is_number_integer())
    cached.min_length_for_detect = t["min_length_for_detect"].get<int>();
  return cached;
}

/// 逐字符快速检测：是否为简体中文。
/// 规则：包含 CJK 统一汉字 (U+4E00–U+9FFF)，且不包含繁体特有字符。
bool is_simplified_chinese(const std::string& text) {
  bool has_cjk = false;
  for (size_t i = 0; i < text.size(); ) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    char32_t cp;
    int len;
    if (c < 0x80)       { cp = c; len = 1; }
    else if (c < 0xE0)  { cp = ((c & 0x1F) << 6) | (text[i+1] & 0x3F); len = 2; }
    else if (c < 0xF0)  { cp = ((c & 0x0F) << 12) | ((text[i+1] & 0x3F) << 6) | (text[i+2] & 0x3F); len = 3; }
    else                { cp = ((c & 0x07) << 18) | ((text[i+1] & 0x3F) << 12) | ((text[i+2] & 0x3F) << 6) | (text[i+3] & 0x3F); len = 4; }
    i += len;

    // 繁体特有字符：體關讓後與嗎還麼 + 常见繁简差异字
    // 出现任一繁体字 → 判定为非简体中文，触发翻译
    if (cp == 0x9AD4 || cp == 0x95DC || cp == 0x8B93 || cp == 0x5F8C ||
        cp == 0x8207 || cp == 0x5617 || cp == 0x9084 || cp == 0x55CE ||
        cp == 0x7747 || cp == 0x5605 || cp == 0x7576 || cp == 0x9304 ||  // 睇嘅當錄
        cp == 0x8A73 || cp == 0x7D30 || cp == 0x5167 || cp == 0x9580 ||  // 詳細內門
        cp == 0x958B || cp == 0x898B || cp == 0x8AAA || cp == 0x6642 ||  // 開見說時
        cp == 0x4F86 || cp == 0x500B || cp == 0x70BA || cp == 0x6703 ||  // 來個為會
        cp == 0x982D || cp == 0x904E || cp == 0x5C0D || cp == 0x570B ||  // 頭過對國
        cp == 0x6A5F || cp == 0x6C23 || cp == 0x96FB || cp == 0x8A71 ||  // 機氣電話
        cp == 0x9577 || cp == 0x8ECA || cp == 0x6771 || cp == 0x66F8 ||  // 長車東書
        cp == 0x9B5A || cp == 0x9CE5 || cp == 0x9F8D  // 魚鳥龍
       )
      return false;

    // CJK 统一汉字区间
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF))
      has_cjk = true;
  }
  return has_cjk;
}

/// 逐字符快速检测：是否为纯英文。
/// 规则：只含 ASCII 可打印字符 (0x20-0x7E)，不含其他 Unicode 语种特征。
bool is_english_text(const std::string& text) {
  for (size_t i = 0; i < text.size(); ) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x20 || c > 0x7E) return false;  // 非 ASCII → 非纯英文
    i += 1;
  }
  return true;
}

/// 是否需要翻译输入：非简体中文、非英文 → true。
/// 文本长度 < min_length 时跳过（太短无法可靠检测）。
bool needs_input_translation(const std::string& text) {
  const auto cfg = translate_input_config();
  if (static_cast<int>(text.size()) < cfg.min_length_for_detect) return false;
  if (is_simplified_chinese(text)) return false;
  if (is_english_text(text)) return false;
  return true;
}

std::string progress_msg(const std::string& key, bool use_zh,
                         const std::vector<std::pair<std::string, std::string>>& params) {
  const auto& root = chat_policy();
  const std::string lang = use_zh ? "zh" : "en";
  std::string tpl = root.value("progress_messages", nlohmann::json::object())
                         .value(lang, nlohmann::json::object())
                         .value(key, key);  // fallback: 返回 key 本身，避免空消息
  return render_policy_template(tpl, params);
}

}  // namespace thin_agent
