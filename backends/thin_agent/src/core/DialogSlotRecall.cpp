#include "thin_agent/core/DialogSlotRecall.h"

#include "thin_agent/core/ChatPolicy.h"

namespace thin_agent {
namespace {

/// 从 JSON 对象安全读取字符串槽位（跳过 null / 非字符串）。
std::string json_safe_string(const nlohmann::json& obj, const char* key, const std::string& fallback) {
  if (!obj.is_object() || !obj.contains(key)) return fallback;
  const auto& v = obj.at(key);
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number_integer()) return std::to_string(v.get<long long>());
  if (v.is_number_float()) return std::to_string(v.get<double>());
  return fallback;
}

}  // namespace

std::string recalled_slot_from_dialog(const DialogContext& dialog_ctx, const std::string& slot_key) {
  if (!dialog_ctx.last_slots.is_object()) return "";
  return json_safe_string(dialog_ctx.last_slots, slot_key.c_str(), "");
}

bool dialog_context_matches_intent(const DialogContext& dialog_ctx, const std::string& intent) {
  return dialog_ctx.ttl_turns > 0 && dialog_ctx.last_intent == intent;
}

bool has_referential_context_markers(const std::string& text) {
  const std::string norm = normalize_text_for_policy(text);
  static const char* kMarkers[] = {
      "刚才", "刚刚", "上次", "之前", "最后", "上一", "哪里", "哪儿", "哪个", "哪家",
      "什么方向", "什么话题", "哪个主题", "什么主题",
      "的呢", "如何", "怎么样", "怎样", "how", "what about", "last", "previous",
  };
  for (const char* m : kMarkers) {
    if (norm.find(m) != std::string::npos) return true;
  }
  return false;
}

bool is_weather_location_recall_query(const std::string& text) {
  const std::string norm = normalize_text_for_policy(text);
  const bool has_weather =
      norm.find("天气") != std::string::npos || norm.find("weather") != std::string::npos;
  if (!has_weather) return false;
  const bool asks_where = norm.find("哪里") != std::string::npos || norm.find("哪儿") != std::string::npos ||
                          norm.find("哪个城市") != std::string::npos ||
                          norm.find("什么地方") != std::string::npos ||
                          norm.find("which city") != std::string::npos ||
                          norm.find("what city") != std::string::npos ||
                          norm.find("which city was") != std::string::npos ||
                          norm.find("what city was") != std::string::npos;
  const bool explicit_weather_city_recall =
      norm.find("which city was the weather") != std::string::npos ||
      norm.find("what city was the weather") != std::string::npos ||
      norm.find("which city was weather") != std::string::npos ||
      norm.find("what city was weather") != std::string::npos;
  return asks_where && (has_referential_context_markers(text) || explicit_weather_city_recall);
}

bool is_news_topic_recall_query(const std::string& text) {
  const std::string norm = normalize_text_for_policy(text);
  const bool has_news =
      norm.find("新闻") != std::string::npos || norm.find("头条") != std::string::npos ||
      norm.find("快讯") != std::string::npos || norm.find("news") != std::string::npos;
  if (!has_news) return false;
  const bool asks_topic =
      norm.find("什么方向") != std::string::npos || norm.find("什么话题") != std::string::npos ||
      norm.find("哪个主题") != std::string::npos || norm.find("什么主题") != std::string::npos ||
      norm.find("哪方面") != std::string::npos || norm.find("what topic") != std::string::npos;
  return asks_topic && has_referential_context_markers(text);
}

bool should_apply_dialog_slot(const std::string& intent,
                              const std::string& slot_key,
                              const std::string& text,
                              const DialogContext& dialog_ctx,
                              const std::string& current_value) {
  if (!current_value.empty()) return false;
  if (recalled_slot_from_dialog(dialog_ctx, slot_key).empty()) return false;
  if (has_referential_context_markers(text)) return true;

  IntentSpec spec;
  if (load_intent_spec(intent, &spec) && spec.kind == "external") {
    if (spec.dialog_slot_policy == "always") return true;
    if (spec.dialog_slot_policy == "same_intent" &&
        dialog_context_matches_intent(dialog_ctx, intent)) {
      return true;
    }
    // referential: only when markers present (already handled above)
    return false;
  }

  // 未配置声明的意图：保持旧行为（同 intent 可续接）
  return dialog_context_matches_intent(dialog_ctx, intent);
}

}  // namespace thin_agent
