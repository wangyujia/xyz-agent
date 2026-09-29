#include "thin_agent/core/IntentScorer.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "thin_agent/core/ChatPolicy.h"

// IntentScorer：基于 chat_policy 锚点/关键词的模糊意图打分与 profile 判定。

namespace thin_agent {
namespace {

/// 从 chat_policy.intent_thresholds 读取阈值，缺失时用 fallback。
double policy_threshold(const char* key, double fallback) {
  const auto& p = chat_policy();
  if (p.contains("intent_thresholds") && p["intent_thresholds"].is_object()) {
    const auto& t = p["intent_thresholds"];
    if (t.contains(key) && t[key].is_number()) return t[key].get<double>();
  }
  return fallback;
}

/// 从 chat_policy.intent_anchors / keywords 加载某意图的锚点与权重。
std::vector<std::pair<std::string, double>> load_anchors_for_intent(const std::string& intent_key) {
  std::vector<std::pair<std::string, double>> out;
  const auto& p = chat_policy();

  if (p.contains("intent_anchors") && p["intent_anchors"].is_object()) {
    const auto& ia = p["intent_anchors"];
    if (ia.contains(intent_key) && ia[intent_key].is_object()) {
      const auto& node = ia[intent_key];
      if (node.contains("anchors") && node["anchors"].is_array()) {
        for (const auto& a : node["anchors"]) {
          if (!a.is_object()) continue;
          const std::string text = a.value("text", "");
          double weight = a.value("weight", 1.0);
          if (!text.empty()) out.emplace_back(text, weight);
        }
      }
    }
  }

  if (out.empty()) {
    for (const auto& kw : policy_keywords(intent_key)) {
      out.emplace_back(kw, 1.0);
    }
  }
  return out;
}

/// 多轮续问时对与上一轮同 intent 的分数加成。
double dialog_boost_for_intent(const std::string& intent_key, const DialogContext* dialog) {
  if (!dialog || dialog->last_intent.empty() || dialog->ttl_turns <= 0) return 0.0;
  const auto& p = chat_policy();
  if (p.contains("intent_anchors") && p["intent_anchors"].is_object()) {
    const auto& ia = p["intent_anchors"];
    if (ia.contains(intent_key) && ia[intent_key].is_object()) {
      return ia[intent_key].value("dialog_boost", 0.0);
    }
  }
  if (intent_key == "weather" && dialog->last_intent == "weather") return 0.35;
  if (intent_key == "news" && dialog->last_intent == "news") return 0.30;
  return 0.0;
}

bool intent_name_is_media_execute(const std::string& intent) {
  if (intent == "media_capture_execute" || intent == "media_recording_execute" || intent == "media_plan") {
    return true;
  }
  if (intent == "task_capture" || intent == "action_capture_photo" || intent == "task_start_recording" ||
      intent == "action_start_recording") {
    return true;
  }
  if (intent.find("capture") != std::string::npos) return true;
  if (intent.find("recording") != std::string::npos) return true;
  return false;
}

bool has_question_surface(const std::string& norm) {
  static const char* kQ[] = {"吗", "呢", "么", "？", "?", "完成了", "好了吗", "好了没", "成功了", "怎么样了",
                             "结果呢", "拍好了", "录好了"};
  for (const char* q : kQ) {
    if (norm.find(q) != std::string::npos) return true;
  }
  return false;
}

bool media_execute_context(const DialogContext* dialog) {
  if (!dialog || dialog->ttl_turns <= 0) return false;
  if (intent_name_is_media_execute(dialog->last_intent)) return true;
  if (dialog->last_route.find("task") != std::string::npos ||
      dialog->last_route.find("action") != std::string::npos ||
      dialog->last_route.find("pipeline") != std::string::npos ||
      dialog->last_route.find("media") != std::string::npos) {
    return true;
  }
  if (dialog->last_slots.is_object()) {
    if (dialog->last_slots.contains("task_id") || dialog->last_slots.contains("pipeline") ||
        dialog->last_slots.contains("action")) {
      return true;
    }
  }
  return false;
}

/// 按空白切分归一化文本为 token。
std::vector<std::string> tokenize(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (unsigned char ch : text) {
    if (std::isspace(ch)) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(static_cast<char>(ch));
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

/// 提取字符二元组，用于短文本相似度。
std::vector<std::string> char_bigrams(const std::string& s) {
  std::vector<std::string> out;
  if (s.size() < 2) {
    if (!s.empty()) out.push_back(s);
    return out;
  }
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    out.push_back(s.substr(i, 2));
  }
  return out;
}

/// 两集合 Jaccard 相似度。
double jaccard(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  if (a.empty() || b.empty()) return 0.0;
  std::unordered_set<std::string> sa(a.begin(), a.end());
  std::unordered_set<std::string> sb(b.begin(), b.end());
  size_t inter = 0;
  for (const auto& x : sa) {
    if (sb.count(x)) ++inter;
  }
  const size_t uni = sa.size() + sb.size() - inter;
  if (uni == 0) return 0.0;
  return static_cast<double>(inter) / static_cast<double>(uni);
}

/// 用户输入与锚点文本的相似度（子串 / Jaccard / 前缀）。
double anchor_similarity(const std::string& norm, const std::string& anchor_raw) {
  const std::string anchor = normalize_text_for_policy(anchor_raw);
  if (anchor.empty() || norm.empty()) return 0.0;
  if (norm.find(anchor) != std::string::npos) return 1.0;
  if (anchor.find(norm) != std::string::npos && norm.size() >= 2) return 0.92;

  const double token_sim = jaccard(tokenize(norm), tokenize(anchor));
  const double bigram_sim = jaccard(char_bigrams(norm), char_bigrams(anchor));

  double best = std::max(token_sim, bigram_sim);
  if (norm.size() >= 2 && anchor.size() >= 2) {
    size_t common_prefix = 0;
    const size_t n = std::min(norm.size(), anchor.size());
    while (common_prefix < n && norm[common_prefix] == anchor[common_prefix]) ++common_prefix;
    if (common_prefix >= 2) {
      const double prefix_boost = static_cast<double>(common_prefix) / static_cast<double>(std::max(norm.size(), anchor.size()));
      best = std::max(best, prefix_boost * 0.95);
    }
  }
  return best;
}

/// 对单一意图计算模糊打分（锚点 + 对话加成 + 槽位启发）。
double score_intent(const std::string& norm, const std::string& intent_key, const DialogContext* dialog) {
  double best = 0.0;
  for (const auto& [anchor, weight] : load_anchors_for_intent(intent_key)) {
    best = std::max(best, weight * anchor_similarity(norm, anchor));
  }

  if (intent_key == "weather") {
    if (!detect_city_slot(norm).empty() && is_weather_extra_trigger(norm)) {
      best = std::max(best, 0.88);
    }
    if (dialog && dialog->last_intent == "weather") {
      const std::string city = resolve_weather_city_candidate(norm, norm);
      if (!city.empty()) best = std::max(best, 0.82);
    }
  }

  // 媒体意图族：上下文条件分类（阶段2）+ 配置门控（阶段3）
  if (intent_key == "media_capture_status") {
    if (media_execute_context(dialog)) {
      if (has_question_surface(norm)) best = std::max(best, 0.88);
      if (norm.size() <= 24 && has_question_surface(norm)) {
        best = std::max(best, 0.90);
      }
      best += dialog_boost_for_intent(intent_key, dialog);
    } else if (!has_question_surface(norm)) {
      best *= 0.4;
    }
  } else if (intent_key == "media_capture_execute" || intent_key == "media_recording_execute" ||
             intent_key == "media_plan" || intent_has_side_effect(intent_key)) {
    IntentGatePolicy gate;
    const bool has_gate = load_intent_gate_policy(intent_key, &gate) && gate.ok;
    if ((has_gate ? gate.side_effect : true) && has_question_surface(norm) && !norm.empty()) {
      best *= 0.25;
    }
    if (has_gate && gate.requires_dialogue_act == "command" && has_question_surface(norm)) {
      best *= 0.5;
    }
    if (intent_key == "media_plan") {
      if (norm.find("再") != std::string::npos || norm.find("然后") != std::string::npos) {
        if (norm.find("拍") != std::string::npos &&
            (norm.find("录") != std::string::npos || norm.find("视频") != std::string::npos)) {
          best = std::max(best, 0.86);
        }
      }
    }
  }

  if (dialog && dialog->last_intent == intent_key) {
    best += dialog_boost_for_intent(intent_key, dialog);
  }
  if (best > 1.0) best = 1.0;
  return best;
}

/// 按意图类型从归一化文本提取槽位（city/topic/limit 等）。
nlohmann::json build_slots_for_intent(const std::string& intent_key, const std::string& norm, const std::string& raw) {
  nlohmann::json slots = nlohmann::json::object();
  if (intent_key == "profile") {
    bool detailed = false;
    for (const auto& m : profile_detail_markers()) {
      if (norm.find(m) != std::string::npos) {
        detailed = true;
        break;
      }
    }
    slots["profile_mode"] = detailed ? "detailed" : "concise";
  } else if (intent_key == "weather") {
    const std::string city = resolve_weather_city_candidate(raw, norm);
    if (!city.empty()) slots["city"] = city;
    const std::string date = resolve_weather_date_slot(norm);
    if (!date.empty()) slots["date"] = date;
  } else if (intent_key == "news") {
    const std::string topic = detect_news_topic_slot(norm);
    if (!topic.empty()) slots["topic_or_scope"] = topic;
    const std::string tr = resolve_news_time_range_slot(norm);
    if (!tr.empty()) slots["time_range"] = tr;
  } else if (intent_key == "memory_history") {
    slots["limit"] = 20;
  } else if (intent_key == "media_capture_execute" || intent_key == "media_plan") {
    slots["family"] = "media";
    slots["dialogue_act"] = "command";
  } else if (intent_key == "media_capture_status") {
    slots["family"] = "media";
    slots["dialogue_act"] = "question";
  } else if (intent_key == "media_recording_execute") {
    slots["family"] = "media";
    slots["dialogue_act"] = "command";
  }
  return slots;
}

static const std::vector<std::string> kIntentOrder = {
    "profile", "profile_detail", "profile_short", "status", "event_recent",
    "memory_recent", "memory_history", "memory_search", "memory_summary",
    "weather", "news", "general",
    "media_capture_status", "media_capture_execute", "media_recording_execute", "media_plan",
    // v0.46.0: 自主性意图（巡检/监控/目标/快照/遗忘/摘要）— 锚点来自
    // chat_policy keywords.autonomy + intent_anchors.autonomy。此前该意图
    // 不在评分列表 → 配置了也不参与打分（死配置），"监控文件"等措辞
    // 落入 unknown 或（含"状态/运行情况"时）被 status 吸走 → local 模板。
    "autonomy",
};

}  // namespace

double intent_execute_threshold() { return policy_threshold("execute", 0.75); }
double intent_clarify_threshold() { return policy_threshold("clarify", 0.45); }

bool is_media_family_intent(const std::string& intent) {
  return intent == "media_capture_execute" || intent == "media_capture_status" ||
         intent == "media_recording_execute" || intent == "media_plan";
}

bool dialog_has_media_execute_context(const DialogContext* dialog) {
  return media_execute_context(dialog);
}

/// 判断是否为模型/运行时状态类提问（非 profile 自我介绍）。
bool is_model_query_like(const std::string& norm) {
  for (const auto& kw : policy_keywords("model_status")) {
    const std::string k = normalize_text_for_policy(kw);
    if (!k.empty() && norm.find(k) != std::string::npos) return true;
  }
  if (norm.find("模型") == std::string::npos) return false;
  static const char* kHints[] = {"什么", "用的", "用啥", "本地", "端侧", "onnx", "runtime", "哪个"};
  for (const char* h : kHints) {
    if (norm.find(h) != std::string::npos) return true;
  }
  return norm.find("你的模型") != std::string::npos;
}

/// 结合 profile 关键词与 detail_markers 判断是否为主体介绍类输入。
bool is_profile_like_text(const std::string& text) {
  const std::string norm = normalize_text_for_policy(text);
  if (is_model_query_like(norm)) return false;

  // v0.47.6: 排除含操作动词的请求。
  // 文本含 "thin_agent" token 但同时含操作动词（返回/记住/读取/解释/explain 等）
  // 时，用户在下达操作指令而非询问 agent 身份 → 不应触发 profile 模板。
  // 与 is_model_query_like 同款排除模式。
  static const char* kActionVerbs[] = {
      "返回", "记住", "读取", "写入", "查看", "搜索", "运行", "执行",
      "创建", "删除", "修改", "编译", "安装", "解释", "说明", "翻译",
      "计算", "生成", "发送", "保存", "加载", "测试", "调试", "部署",
      "exlain", "remember", "return", "create", "delete", "search",
      "write", "read", "execute", "compile", "build", "explain",
      "describe", "translate", "calculate", "generate"};
  for (const char* v : kActionVerbs) {
    if (norm.find(v) != std::string::npos) return false;
  }

  const double s = score_intent(norm, "profile", nullptr);
  const double sd = score_intent(norm, "profile_detail", nullptr);
  const double ss = score_intent(norm, "profile_short", nullptr);
  return std::max({s, sd, ss}) >= intent_execute_threshold();
}

/// 对各意图打分并返回 Top-N，见 IntentScorer.h。
std::vector<std::pair<std::string, double>> top_intent_scores(const std::string& text,
                                                              int top_n,
                                                              const DialogContext* dialog) {
  const std::string norm = normalize_text_for_policy(text);
  std::vector<std::pair<std::string, double>> scored;
  for (const auto& intent : kIntentOrder) {
    const double s = score_intent(norm, intent, dialog);
    if (s > 0.01) scored.emplace_back(intent, s);
  }
  std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;
  });
  if (top_n > 0 && static_cast<int>(scored.size()) > top_n) scored.resize(static_cast<size_t>(top_n));
  return scored;
}

/// 模糊意图分类主入口，见 IntentScorer.h。
nlohmann::json classify_intent_fuzzy(const std::string& text, const DialogContext* dialog) {
  const std::string norm = normalize_text_for_policy(text);
  const auto ranked = top_intent_scores(text, 1, dialog);
  if (ranked.empty() || ranked[0].second < intent_clarify_threshold()) {
    return {{"intent", "unknown"}, {"confidence", 0.0}, {"backend", "fuzzy"}, {"slots", nlohmann::json::object()}};
  }

  const std::string intent = ranked[0].first;
  double confidence = ranked[0].second;

  std::string mapped_intent = intent;
  if (intent == "profile_detail" || intent == "profile_short") mapped_intent = "profile";

  nlohmann::json slots = build_slots_for_intent(intent, norm, text);
  if (mapped_intent == "weather") {
    if (!slots.value("city", "").empty()) {
      confidence = std::max(confidence, intent_execute_threshold());
    } else if (confidence >= intent_clarify_threshold()) {
      if (dialog && dialog->last_intent == "weather") {
        const std::string city = resolve_weather_city_candidate(text, norm);
        if (!city.empty()) {
          slots["city"] = city;
          confidence = std::max(confidence, intent_execute_threshold());
        }
      }
      confidence = std::min(confidence, 0.64);
    }
  } else if (mapped_intent == "news") {
    if (slots.value("topic_or_scope", "").empty()) {
      confidence = std::min(confidence, 0.62);
    } else {
      confidence = std::max(confidence, 0.83);
    }
  } else if (mapped_intent == "general") {
    confidence = std::min(confidence, 0.52);
  } else if (mapped_intent == "profile") {
    if (slots.value("profile_mode", "") == "detailed") confidence = std::max(confidence, 0.95);
    else confidence = std::max(confidence, 0.86);
  } else if (mapped_intent == "media_capture_status") {
    if (media_execute_context(dialog)) {
      confidence = std::max(confidence, intent_execute_threshold());
    } else {
      confidence = std::min(confidence, 0.70);
    }
  } else if (mapped_intent == "media_capture_execute" || mapped_intent == "media_recording_execute") {
    if (has_question_surface(norm)) {
      confidence = std::min(confidence, 0.40);
    } else {
      confidence = std::max(confidence, 0.80);
    }
  } else if (mapped_intent == "media_plan") {
    confidence = std::max(confidence, 0.82);
  }

  return {
      {"intent", mapped_intent},
      {"confidence", confidence},
      {"backend", "fuzzy"},
      {"slots", std::move(slots)},
      {"raw_intent", intent},
  };
}

}  // namespace thin_agent
