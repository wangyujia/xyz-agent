#include <regex>

#include "thin_agent/core/ChatPolicy.h"
#include "thin_agent/core/ExternalInfoClient.h"

#include <cctype>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

#include "thin_agent/core/ChatPolicy.h"
#include "thin_agent/llm/IHttpClient.h"

// ExternalInfoClient：天气/新闻 HTTP 或 mock 拉取，展示文案经 ChatPolicy 本地化。

namespace thin_agent {
namespace {

/// 读取环境变量，缺失时返回默认值。
std::string env_or_default(const char* key, const std::string& defv = "") {
  const char* v = std::getenv(key);
  return (v && *v) ? std::string(v) : defv;
}

std::string env_or_empty(const char* key) {
  return env_or_default(key, "");
}

/// GET 请求并解析 JSON 响应；失败时写入 err。
bool http_get_json(IHttpClient& http, const std::string& url,
                   int& http_status, nlohmann::json& out_json, std::string& err) {
  auto resp = http.get(url, "");
  http_status = resp.status_code;
  if (!resp.ok) {
    err = resp.error;
    return false;
  }
  if (resp.status_code < 200 || resp.status_code >= 300) {
    err = "http_status_" + std::to_string(resp.status_code);
    return false;
  }
  try {
    out_json = nlohmann::json::parse(resp.body);
    return true;
  } catch (...) {
    err = "invalid_json_response";
    return false;
  }
}

/// 全局替换子串。
std::string replace_all(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

/// URL 编码空格为 %20（简易版，用于 wttr.in 路径段）。
std::string url_encode_spaces(std::string s) {
  return replace_all(std::move(s), " ", "%20");
}

/// 完整百分号编码 URL query 组件：非 unreserved 字符（含中文 UTF-8 字节）逐字节转 %XX。
/// 修复中文 topic（如「科技」）未编码导致外部接口返回 HTTP 400 的问题。
std::string url_encode_component(const std::string& s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

/// 标题中是否含独立的 "ai" 词（前后非字母），避免 airfoil/said 等误匹配。
bool title_has_ai_token(const std::string& lower_title) {
  size_t pos = 0;
  while ((pos = lower_title.find("ai", pos)) != std::string::npos) {
    const bool left_ok = (pos == 0) || !std::isalpha(static_cast<unsigned char>(lower_title[pos - 1]));
    const bool right_ok = (pos + 2 >= lower_title.size()) ||
                          !std::isalpha(static_cast<unsigned char>(lower_title[pos + 2]));
    if (left_ok && right_ok) return true;
    pos += 2;
  }
  return false;
}

/// 是否为 wttr.in 自动定位占位城市（空/今天/明天等）。
bool is_auto_location_city(const std::string& city) {
  return city.empty() || city == "今天" || city == "明天" || city == "今日" || city == "~";
}

/// 将城市名转为 wttr.in 路径 token（自动定位用 ~）。
std::string wttr_city_token(const std::string& city) {
  if (is_auto_location_city(city)) return "~";
  return city;
}

/// ASCII 字母转小写。
std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

/// 粗略检测文本是否含 CJK 字符（UTF-8 三字节起）。
bool contains_cjk(const std::string& text) {
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c >= 0xE4) return true;
  }
  return false;
}

/// 过滤 HN 元帖、招聘帖等噪声标题。
bool news_title_is_noise(const std::string& title) {
  const std::string lower = to_lower_ascii(title);
  static const char* kNoise[] = {
      "don't post", "generated comment", "ai-edited", "show hn", "ask hn",
      "meta:", "announcement:", "who is hiring", "launch hn",
  };
  for (const char* n : kNoise) {
    if (lower.find(n) != std::string::npos) return true;
  }
  return false;
}

/// AI/科技主题下按标题关键词与 CJK 过滤新闻条目。
bool news_title_matches_topic(const std::string& title, const std::string& topic) {
  if (news_title_is_noise(title)) return false;
  const std::string lower_title = to_lower_ascii(title);
  const std::string lower_topic = to_lower_ascii(topic);
  const bool ai_topic = lower_topic.find("ai") != std::string::npos ||
                        topic.find("科技") != std::string::npos ||
                        topic.find("智能") != std::string::npos;
  if (!ai_topic) return true;
  if (contains_cjk(title)) return true;
  if (title_has_ai_token(lower_title)) return true;
  // "ai" 单独用词边界判定（见 title_has_ai_token），其余关键词按子串匹配即可。
  static const char* kAiHints[] = {
      "artificial intelligence", "machine learning", "llm", "gpt", "model",
      "chip", "gpu", "agent", "neural", "openai", "deepseek", "robot",
  };
  for (const char* h : kAiHints) {
    if (lower_title.find(h) != std::string::npos) return true;
  }
  return false;
}

/// 按主题过滤新闻列表；全被滤掉时降级为去噪后的原列表。
nlohmann::json filter_news_items(const nlohmann::json& items, const std::string& topic) {
  if (!items.is_array()) return nlohmann::json::array();
  nlohmann::json filtered = nlohmann::json::array();
  for (const auto& item : items) {
    if (!item.is_object()) continue;
    const std::string title = item.value("title", "");
    if (title.empty()) continue;
    if (news_title_matches_topic(title, topic)) filtered.push_back(item);
  }
  if (!filtered.empty()) return filtered;
  for (const auto& item : items) {
    if (!item.is_object()) continue;
    const std::string title = item.value("title", "");
    if (!title.empty() && !news_title_is_noise(title)) filtered.push_back(item);
  }
  return filtered.empty() ? items : filtered;
}

/// 从 wttr.in JSON 响应提取最近区域名称。
std::string extract_wttr_area_name(const nlohmann::json& jr) {
  const auto areas = jr.value("nearest_area", nlohmann::json::array());
  if (!areas.is_array() || areas.empty() || !areas[0].is_object()) return "";
  const auto names = areas[0].value("areaName", nlohmann::json::array());
  if (!names.is_array() || names.empty() || !names[0].is_object()) return "";
  return names[0].value("value", std::string(""));
}

/// 向 URL 追加 query 参数（自动选择 ? 或 &）。
std::string append_query_param(std::string url, const std::string& key, const std::string& value) {
  url += (url.find('?') == std::string::npos) ? '?' : '&';
  url += key;
  url += '=';
  url += value;
  return url;
}

}  // namespace

/// 查询天气；真实 HTTP 失败或 provider=mock 时返回 mock 数据，城市/状况按 lang 本地化。
ExternalInfoClient::FetchResult ExternalInfoClient::fetch_weather(IHttpClient& http,
                                                                  const std::string& city,
                                                                  const std::string& date,
                                                                  const std::string& external_provider,
                                                                  const std::string& external_weather_url,
                                                                  const std::string& lang) {
  FetchResult r;
  // Config param takes priority, env var as fallback, default to mock
  std::string provider = external_provider;
  if (provider == "mock") {
    const char* env = std::getenv("THIN_AGENT_EXTERNAL_PROVIDER");
    if (env && *env) provider = env;
  }

  if (provider == "http") {
    const std::string lang_code = (lang == "zh" || lang == "zh-TW") ? "zh" : "en";

    // v0.27.2: mock HTTP 绕过（单测 / 离线环境无需真实联网）
    const std::string mock_http_json = env_or_empty("THIN_AGENT_EXTERNAL_HTTP_MOCK_WEATHER_JSON");
    if (!mock_http_json.empty()) {
      try {
        auto j = nlohmann::json::parse(mock_http_json);
        r.ok = true;
        r.http_status = 200;
        r.source = "mock-http";
        r.data = nlohmann::json{
            {"provider", "mock-http"},
            {"city", city},
            {"date", date},
            {"temperature_c", j.value("temperature_c", 0)},
            {"condition", j.value("condition", "Unknown")},
            {"humidity_pct", j.value("humidity_pct", 0)},
            {"lang", lang_code},
        };
        return r;
      } catch (...) {
        r.ok = false;
        r.http_status = 0;
        r.source = "mock-http";
        r.error = "invalid_http_mock_weather_json";
        return r;
      }
    }

    const std::string wttr_city = wttr_city_token(city);

    std::string endpoint = external_weather_url;
    if (endpoint.empty()) {
      const char* env_url = std::getenv("THIN_AGENT_EXTERNAL_WEATHER_URL");
      if (env_url && *env_url) endpoint = env_url;
    }
    if (endpoint.empty()) {
      endpoint = "https://wttr.in/" + url_encode_component(wttr_city) + "?format=j1";
    }
    endpoint = replace_all(endpoint, "{city}", url_encode_component(wttr_city));
    endpoint = replace_all(endpoint, "{date}", date);
    if (endpoint.find("lang=") == std::string::npos) {
      endpoint = append_query_param(endpoint, "lang", lang_code);
    }

    nlohmann::json jr;
    std::string err;
    int code = 0;
    if (http_get_json(http, endpoint, code, jr, err)) {
      const auto current = jr.value("current_condition", nlohmann::json::array());
      std::string condition = lang_code == "zh" ? "未知" : "Unknown";
      double temp = 0.0;
      int humidity = 0;
      if (!current.empty() && current[0].is_object()) {
        const auto& cc = current[0];
        temp = std::stod(cc.value("temp_C", "0"));
        humidity = std::stoi(cc.value("humidity", "0"));
        const auto& desc = cc.value("weatherDesc", nlohmann::json::array());
        if (!desc.empty() && desc[0].is_object()) {
          condition = desc[0].value("value", condition);
          condition = localize_weather_condition(condition, lang_code);
        }
      }
      std::string resolved_city = city;
      if (is_auto_location_city(city)) {
        const std::string area = extract_wttr_area_name(jr);
        if (!area.empty()) {
          resolved_city = area;
        } else {
          resolved_city = lang_code == "zh" ? "本地" : "local";
        }
      }
      resolved_city = localize_city_name(resolved_city, lang_code);
      r.ok = true;
      r.http_status = code;
      r.source = "wttr.in";
      r.data = {
          {"provider", "wttr.in"},
          {"city", resolved_city},
          {"date", date},
          {"temperature_c", static_cast<int>(temp)},
          {"condition", condition},
          {"humidity_pct", humidity},
          {"lang", lang_code},
      };
      return r;
    }

    r.ok = false;
    r.http_status = code;
    r.source = "wttr.in";
    r.error = err.empty() ? "weather_http_failed" : err;
    return r;
  }

  const std::string mock_temp = env_or_default("THIN_AGENT_MOCK_WEATHER_TEMP", "28");
  const std::string mock_cond = env_or_default(
      lang == "en" ? "THIN_AGENT_MOCK_WEATHER_CONDITION_EN" : "THIN_AGENT_MOCK_WEATHER_CONDITION",
      lang == "en" ? "Partly cloudy" : "多云");
  const std::string mock_humidity = env_or_default("THIN_AGENT_MOCK_WEATHER_HUMIDITY", "61");

  r.ok = true;
  r.http_status = 200;
  r.source = "mock";
  r.data = {
      {"provider", "mock-weather"},
      {"city", city},
      {"date", date},
      {"temperature_c", std::atoi(mock_temp.c_str())},
      {"condition", mock_cond},
      {"humidity_pct", std::atoi(mock_humidity.c_str())},
  };

  return r;
}

/// 查询新闻；provider=mock 时返回内置示例条目。
ExternalInfoClient::FetchResult ExternalInfoClient::fetch_news(IHttpClient& http,
                                                               const std::string& topic,
                                                               const std::string& time_range,
                                                               const std::string& external_provider) {
  FetchResult r;
  // Config param takes priority, env var as fallback, default to mock
  std::string provider = external_provider;
  if (provider == "mock") {
    const char* env = std::getenv("THIN_AGENT_EXTERNAL_PROVIDER");
    if (env && *env) provider = env;
  }

  if (provider == "http") {
    const std::string mock_http_json = env_or_empty("THIN_AGENT_EXTERNAL_HTTP_MOCK_NEWS_JSON");
    if (!mock_http_json.empty()) {
      try {
        auto j = nlohmann::json::parse(mock_http_json);
        r.ok = true;
        r.http_status = 200;
        r.source = "real-http";
        r.data = {
            {"provider", "hn-algolia"},
            {"topic", topic},
            {"time_range", time_range},
            {"items", filter_news_items(j.value("items", nlohmann::json::array()), topic)},
        };
        return r;
      } catch (...) {
        r.ok = false;
        r.http_status = 0;
        r.source = "real-http";
        r.error = "invalid_http_mock_news_json";
        return r;
      }
    }

    std::string endpoint = env_or_empty("THIN_AGENT_EXTERNAL_NEWS_URL");
    if (endpoint.empty()) {
      endpoint = "https://hn.algolia.com/api/v1/search?query={topic}&tags=story&hitsPerPage=5";
    }
    endpoint = replace_all(endpoint, "{topic}", url_encode_component(topic));
    endpoint = replace_all(endpoint, "{time_range}", url_encode_component(time_range));

    nlohmann::json jr;
    std::string err;
    int code = 0;
    if (http_get_json(http, endpoint, code, jr, err)) {
      nlohmann::json items = nlohmann::json::array();
      if (jr.contains("hits") && jr["hits"].is_array()) {
        for (const auto& h : jr["hits"]) {
          if (!h.is_object()) continue;
          const std::string title = h.value("title", h.value("story_title", ""));
          if (title.empty()) continue;
          items.push_back({
              {"title", title},
              {"source", h.value("author", "hn")},
              {"url", h.value("url", h.value("story_url", ""))},
          });
          if (items.size() >= 5) break;
        }
      }

      r.ok = true;
      r.http_status = code;
      r.source = "real-http";
      r.data = {
          {"provider", "hn-algolia"},
          {"topic", topic},
          {"time_range", time_range},
          {"items", filter_news_items(std::move(items), topic)},
      };
      return r;
    }

    r.ok = false;
    r.http_status = code;
    r.source = "real-http";
    r.error = err.empty() ? "news_http_failed" : err;
    return r;
  }

  const std::string mock_title_1 = env_or_default("THIN_AGENT_MOCK_NEWS_TITLE_1", "AI 模型推理效率持续提升");
  const std::string mock_title_2 = env_or_default("THIN_AGENT_MOCK_NEWS_TITLE_2", "端侧智能体进入规模化验证阶段");

  r.ok = true;
  r.http_status = 200;
  r.source = "mock";
  r.data = {
      {"provider", "mock-news"},
      {"topic", topic},
      {"time_range", time_range},
      {"items", nlohmann::json::array({
                    {{"title", mock_title_1}, {"source", "mock_feed"}},
                    {{"title", mock_title_2}, {"source", "mock_feed"}},
                })},
  };

  return r;
}



/// v0.53.10: 剥离 html 标签（Bing 解析用）
static std::string strip_html_tags(const std::string& in) {
  static const std::regex tag_re(R"(<[^>]*>)");
  return std::regex_replace(in, tag_re, "");
}

/// v0.53.10: 常见 html 实体解码
static std::string html_unescape(const std::string& in) {
  std::string s2 = in;
  const std::pair<std::string, std::string> ents[] = {
      {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
      {"&#39;", "'"}, {"&nbsp;", " "}};
  for (const auto& [e, c] : ents) {
    size_t p;
    while ((p = s2.find(e)) != std::string::npos) s2.replace(p, e.size(), c);
  }
  return s2;
}

// ── v0.53.10: Bing 网页搜索适配器（免费无 key，html 解析）──
// 国内可达性实测：www.bing.com/search 302→200，b_algo 结果块正则可稳定解析 10 条。
// 依赖 UA 头 + 跟随重定向（CurlHttpClient::get v0.53.10 增强）。
static ExternalInfoClient::FetchResult fetch_search_bing(
    IHttpClient& http, const std::string& query, const std::string& lang,
    const std::string& base, int timeout_ms) {
  ExternalInfoClient::FetchResult r;
  // mkt 参数：zh→zh-CN，en→en-US（Bing 区域偏好）
  const std::string mkt = (lang == "zh") ? "zh-CN" : "en-US";
  std::string url = (base.empty() ? "https://www.bing.com/search" : base) +
                    "?q=" + url_encode_component(query) +
                    "&mkt=" + mkt + "&count=8";
  HttpResponse resp = http.get(url, "");
  if (!resp.ok) {
    r.ok = false; r.http_status = resp.status_code; r.source = "bing";
    r.error = resp.error.empty() ? "search_http_failed" : resp.error;
    return r;
  }
  // b_algo 结果块：<li class="b_algo">...<h2><a href="url">title</a></h2>...<p>snippet</p>
  static const std::regex algo_re(
      R"rx(<li class="b_algo"[\s\S]*?<h2[^>]*><a[^>]*href="([^"]+)"[^>]*>([\s\S]*?)</a></h2>([\s\S]*?)</li>)rx",
      std::regex::icase);
  nlohmann::json results = nlohmann::json::array();
  auto it_end = std::sregex_iterator();
  for (auto it = std::sregex_iterator(resp.body.begin(), resp.body.end(), algo_re);
       it != it_end && results.size() < 8; ++it) {
    std::string u = (*it)[1].str();
    std::string title = strip_html_tags((*it)[2].str());
    std::string snippet = strip_html_tags((*it)[3].str());
    // snippet 块可能含嵌套标签已剥；截断
    if (title.empty()) continue;
    if (title.size() > 150) title = title.substr(0, 147) + "...";
    if (snippet.size() > 300) snippet = snippet.substr(0, 297) + "...";
    // html 实体解码（&amp; 等）
    title = html_unescape(title);
    snippet = html_unescape(snippet);
    results.push_back({{"title", title}, {"snippet", snippet},
                       {"url", html_unescape(u)}, {"source", "bing"}});
  }
  r.ok = !results.empty();
  r.http_status = resp.status_code;
  r.source = "bing";
  r.data = {{"query", query}, {"results", std::move(results)}};
  if (!r.ok) r.error = "bing_parse_empty";
  (void)timeout_ms;
  return r;
}

/// 查询网页搜索（DuckDuckGo Instant Answer API / mock）。
ExternalInfoClient::FetchResult ExternalInfoClient::fetch_search(
    IHttpClient& http, const std::string& query, const std::string& lang,
    const std::string& provider) {
  FetchResult r;
  std::string prov = provider;
  // v0.53.10: "auto"=读 chat_policy.json web_search.provider（运行时可改可存）
  if (prov == "auto" || prov == "mock" || prov.empty()) {
    const auto& pol = chat_policy();
    std::string cfg_prov;
    if (pol.contains("web_search") && pol["web_search"].is_object())
      cfg_prov = pol["web_search"].value("provider", "");
    if (!cfg_prov.empty() && cfg_prov != "auto") prov = cfg_prov;
  }
  if (prov.empty() || prov == "auto") prov = "bing";  // 兜底源

  if (prov == "bing") {
    const auto& pol = chat_policy();
    std::string base;
    if (pol.contains("web_search") && pol["web_search"].is_object() &&
        pol["web_search"].contains("bing") && pol["web_search"]["bing"].is_object())
      base = pol["web_search"]["bing"].value("base", "");
    return fetch_search_bing(http, query, lang, base, 8000);
  }

  if (prov == "duckduckgo") {
    const std::string region = (lang == "zh") ? "cn-zh" : "us-en";
    std::string url = "https://api.duckduckgo.com/?q=" + url_encode_component(query) +
                      "&format=json&no_html=1&skip_disambig=1&t=thin_agent&kl=" + region;

    nlohmann::json jr;
    std::string err;
    int code = 0;
    if (http_get_json(http, url, code, jr, err)) {
      nlohmann::json results = nlohmann::json::array();

      // Abstract (primary answer)
      std::string abstract = jr.value("AbstractText", jr.value("Abstract", ""));
      std::string abstract_url = jr.value("AbstractURL", "");
      std::string abstract_source = jr.value("AbstractSource", "");
      if (!abstract.empty()) {
        results.push_back({
            {"title", abstract.size() > 200 ? abstract.substr(0, 197) + "..." : abstract},
            {"snippet", abstract},
            {"url", abstract_url},
            {"source", abstract_source.empty() ? "DuckDuckGo" : abstract_source},
        });
      }

      // Related topics (up to 5)
      if (jr.contains("RelatedTopics") && jr["RelatedTopics"].is_array()) {
        for (const auto& topic : jr["RelatedTopics"]) {
          if (!topic.is_object()) continue;
          std::string text = topic.value("Text", "");
          if (text.empty()) continue;
          results.push_back({
              {"title", text.size() > 150 ? text.substr(0, 147) + "..." : text},
              {"snippet", text},
              {"url", topic.value("FirstURL", "")},
              {"source", "DuckDuckGo"},
          });
          if (results.size() >= 6) break;
        }
      }

      r.ok = !results.empty();
      r.http_status = code;
      r.source = "duckduckgo";
      r.data = {{"query", query}, {"results", std::move(results)}};
      return r;
    }

    r.ok = false;
    r.http_status = code;
    r.source = "duckduckgo";
    r.error = err.empty() ? "search_http_failed" : err;
    return r;
  }

  // mock fallback
  r.ok = true;
  r.http_status = 200;
  r.source = "mock";
  r.data = {
      {"query", query},
      {"results", nlohmann::json::array({
                      {{"title", lang == "zh" ? "搜索结果示例 1" : "Search result example 1"},
                       {"snippet", lang == "zh" ? "这是 mock 搜索结果的摘要内容。" : "This is a mock search result snippet."},
                       {"url", "https://example.com/1"},
                       {"source", "mock"}},
                      {{"title", lang == "zh" ? "搜索结果示例 2" : "Search result example 2"},
                       {"snippet", lang == "zh" ? "更多 mock 搜索结果的摘要。" : "More mock search result snippets."},
                       {"url", "https://example.com/2"},
                       {"source", "mock"}},
                  })},
  };
  return r;
}

/// 将搜索结果的 data JSON 格式化为 LLM 可读文本。
std::string ExternalInfoClient::format_search_results(const nlohmann::json& data,
                                                       const std::string& lang) {
  const auto& results = data.value("results", nlohmann::json::array());
  if (results.empty()) {
    return lang == "zh" ? "未找到相关搜索结果。" : "No search results found.";
  }

  std::string out;
  if (lang == "zh") {
    out = "搜索结果（" + data.value("query", "") + "）：\n\n";
  } else {
    out = "Search results for \"" + data.value("query", "") + "\":\n\n";
  }

  for (size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    out += std::to_string(i + 1) + ". **" + r.value("title", "") + "**\n";
    out += "   " + r.value("snippet", "") + "\n";
    if (!r.value("url", "").empty()) {
      out += "   " + r.value("url", "") + "\n";
    }
    out += "\n";
  }

  return out;
}

}  // namespace thin_agent
