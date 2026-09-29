#include "thin_agent/core/ExternalIntentHandlers.h"

#include "thin_agent/core/ChatPolicy.h"

namespace thin_agent {
namespace {

std::string json_safe_string(const nlohmann::json& obj, const char* key, const std::string& fallback) {
  if (!obj.is_object() || !obj.contains(key)) return fallback;
  const auto& v = obj[key];
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
  if (v.is_number_float()) return std::to_string(v.get<double>());
  return fallback;
}

bool contains_ascii_alpha(const std::string& text) {
  for (unsigned char c : text) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
  }
  return false;
}

bool needs_field_translation(const std::string& target_lang, const std::string& source_lang) {
  if (source_lang.empty()) return false;
  return target_lang != source_lang;
}

std::string render_template(std::string tpl, const std::vector<std::pair<std::string, std::string>>& kv) {
  return render_policy_template(std::move(tpl), kv);
}

ExternalHandlerOutput handle_weather(const ExternalHandlerContext& ctx) {
  ExternalHandlerOutput out;
  const std::string city = ctx.primary;
  const std::string date = json_safe_string(ctx.slots, "date", "今天");
  out.ext = ExternalInfoClient::fetch_weather(
      *ctx.http, city, date, ctx.cfg.external_provider, ctx.cfg.external_weather_url, ctx.query_lang);
  if (!out.ext.ok) return out;

  out.fetch_ok = true;
  const int temp = out.ext.data.value("temperature_c", 0);
  std::string cond =
      json_safe_string(out.ext.data, "condition", ctx.tpl_lang == "en" ? "Unknown" : "未知");
  const int humidity = out.ext.data.value("humidity_pct", 0);
  const std::string display_city = json_safe_string(out.ext.data, "city", city);

  if (!cond.empty() && ctx.translate) {
    const std::string source_lang = detect_query_language(cond);
    const std::string target_lang = reply_target_language(ctx.query_lang, ctx.tpl_lang);
    const bool mixed_for_zh = (target_lang == "zh" || target_lang == "zh-TW") &&
                              source_lang == "zh" && contains_ascii_alpha(cond);
    if (needs_field_translation(target_lang, source_lang) || mixed_for_zh) {
      auto tr = ctx.translate({cond}, target_lang, ctx.text);
      if (!tr.empty() && !tr[0].empty()) {
        cond = tr[0];
        out.field_translated = true;
      }
    }
  }

  const std::string tpl_city = localize_city_name(display_city, ctx.tpl_lang);
  const std::string tpl_date = localize_weather_date(date, ctx.tpl_lang);
  out.summary = render_template(
      policy_text_for_lang(ctx.spec.summary_template_key, ctx.tpl_lang, ""),
      {{"city", tpl_city},
       {"date", tpl_date},
       {"condition", cond},
       {"temp_c", std::to_string(temp)},
       {"humidity", std::to_string(humidity)},
       {"source", out.ext.source}});
  out.tool_call = {{"tool", "external_weather"},
                   {"city", city},
                   {"date", date},
                   {"source", out.ext.source}};

  if (ctx.spec.post_execute == "weather_advice") {
    out.advice_slots = {{"city", display_city},
                        {"condition", cond},
                        {"temp_c", temp},
                        {"humidity", humidity},
                        {"query_lang", ctx.query_lang}};
  }
  return out;
}

ExternalHandlerOutput handle_news(const ExternalHandlerContext& ctx) {
  ExternalHandlerOutput out;
  const std::string topic = ctx.primary;
  const std::string tr = json_safe_string(ctx.slots, "time_range", "最近");
  out.ext = ExternalInfoClient::fetch_news(*ctx.http, topic, tr, ctx.cfg.external_provider);
  if (!out.ext.ok) return out;

  out.fetch_ok = true;

  // 收最多 5 条
  std::vector<std::string> items;
  if (out.ext.data.contains("items") && out.ext.data["items"].is_array()) {
    for (const auto& item : out.ext.data["items"]) {
      if (items.size() >= 5) break;
      if (!item.is_object()) continue;
      std::string title = item.value("title", "");
      if (!title.empty()) items.push_back(std::move(title));
    }
  }
  if (items.empty()) {
    const bool is_en = (ctx.tpl_lang == "en");
    out.summary = is_en
        ? "No news found about " + topic + "."
        : "没有找到" + topic + "相关的新闻。";
    out.tool_call = {{"tool", "external_news"},
                     {"topic", topic},
                     {"time_range", tr},
                     {"source", out.ext.source}};
    return out;
  }

  // 需要翻译时整批翻译
  if (ctx.translate) {
    const std::string source_lang = detect_query_language(items[0]);
    if (needs_field_translation(ctx.query_lang, source_lang)) {
      auto translated = ctx.translate(items, ctx.query_lang, ctx.text);
      if (translated.size() == items.size()) {
        items = std::move(translated);
        out.field_translated = true;
      }
    }
  }

  // 按条数拼摘要
  const bool is_en = (ctx.tpl_lang == "en");
  const int n = static_cast<int>(items.size());
  std::string header;
  if (is_en) {
    header = (n == 1) ? "Only one " + topic + " result:"
                      : "Found " + std::to_string(n) + " " + topic + " stories:";
  } else {
    header = (n == 1) ? "只找到一条" + topic + "的："
                      : "给你找了" + std::to_string(n) + "条" + topic + "的：";
  }
  for (const auto& t : items) {
    header += "\n▸ " + t;
  }

  out.summary = header;
  out.tool_call = {{"tool", "external_news"},
                   {"topic", topic},
                   {"time_range", tr},
                   {"source", out.ext.source}};
  return out;
}

ExternalHandlerOutput handle_search(const ExternalHandlerContext& ctx) {
  ExternalHandlerOutput out;
  const std::string query = ctx.primary;
  const std::string lang = ctx.query_lang.empty() ? "zh" : ctx.query_lang;

  // v0.53.10: provider 传 "auto"——fetch_search 内部读 chat_policy.json 的
  // web_search.provider（WS web_search_config 运行时可改可存）；未配置时
  // 退回 cfg.external_provider（yaml 语义保留），最终兜底 bing。
  std::string prov = "auto";
  if (prov == "auto") {
    const auto& pol = chat_policy();
    if (!pol.contains("web_search") || !pol["web_search"].is_object() ||
        !pol["web_search"].contains("provider")) {
      prov = ctx.cfg.external_provider;  // chat_policy 未配置→yaml 兜底
    }
  }
  out.ext = ExternalInfoClient::fetch_search(*ctx.http, query, lang, prov);
  if (!out.ext.ok) return out;

  out.fetch_ok = true;
  out.summary = ExternalInfoClient::format_search_results(out.ext.data, lang);
  out.tool_call = {{"tool", "web_search"},
                   {"query", query},
                   {"source", out.ext.source}};
  return out;
}

}  // namespace

ExternalHandlerOutput run_external_intent_handler(const ExternalHandlerContext& ctx) {
  if (ctx.spec.fetcher == "weather") return handle_weather(ctx);
  if (ctx.spec.fetcher == "news") return handle_news(ctx);
  if (ctx.spec.fetcher == "search") return handle_search(ctx);
  ExternalHandlerOutput out;
  out.ext.ok = false;
  out.ext.error = "unknown_fetcher:" + ctx.spec.fetcher;
  return out;
}

}  // namespace thin_agent