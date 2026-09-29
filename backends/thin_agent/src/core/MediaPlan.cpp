#include "thin_agent/core/MediaPlan.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace thin_agent {
namespace {

std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

bool contains_utf8(const std::string& hay, const std::string& needle) {
  return !needle.empty() && hay.find(needle) != std::string::npos;
}

bool contains_any_utf8(const std::string& hay, const std::vector<std::string>& needles) {
  for (const auto& n : needles) {
    if (contains_utf8(hay, n)) return true;
  }
  return false;
}

bool strong_command_hint(const std::string& text) {
  return contains_any_utf8(text, {"帮我", "给我", "请", "麻烦", "来张", "来一段"});
}

int parse_chinese_or_digit_number(const std::string& text, size_t pos) {
  static const std::unordered_map<std::string, int> kCn = {
      {"一", 1}, {"二", 2}, {"两", 2}, {"三", 3}, {"四", 4}, {"五", 5},
      {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9}, {"十", 10},
  };
  if (pos >= text.size()) return -1;
  if (std::isdigit(static_cast<unsigned char>(text[pos]))) {
    int v = 0;
    size_t i = pos;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
      v = v * 10 + (text[i] - '0');
      if (v > 99) return -1;
      ++i;
    }
    return v > 0 ? v : -1;
  }
  for (const auto& [k, v] : kCn) {
    if (text.compare(pos, k.size(), k) == 0) return v;
  }
  return -1;
}

int extract_number_before(const std::string& seg, size_t marker_pos) {
  if (marker_pos == 0) return -1;
  size_t i = marker_pos;
  while (i > 0) {
    --i;
    const unsigned char c = static_cast<unsigned char>(seg[i]);
    if (std::isdigit(c)) {
      size_t start = i;
      while (start > 0 && std::isdigit(static_cast<unsigned char>(seg[start - 1]))) --start;
      return parse_chinese_or_digit_number(seg, start);
    }
    if ((c & 0xC0) != 0x80) {
      size_t start = i;
      while (start > 0 && (static_cast<unsigned char>(seg[start]) & 0xC0) == 0x80) --start;
      return parse_chinese_or_digit_number(seg, start);
    }
  }
  return -1;
}

int extract_capture_count(const std::string& seg, int default_count) {
  const std::vector<std::string> markers = {"张照片", "张照", "张图", "张"};
  for (const auto& m : markers) {
    const auto pos = seg.find(m);
    if (pos == std::string::npos) continue;
    const int v = extract_number_before(seg, pos);
    if (v > 0) return v;
  }
  const auto pai = seg.find("拍");
  if (pai != std::string::npos) {
    const int v = parse_chinese_or_digit_number(seg, pai + std::string("拍").size());
    if (v > 0 && contains_any_utf8(seg.substr(pai), {"张", "照", "photo"})) return v;
  }
  return default_count;
}

int extract_duration_sec(const std::string& seg) {
  const std::string lower = to_lower_ascii(seg);
  for (const auto& m : std::vector<std::string>{"秒钟", "秒"}) {
    const auto pos = seg.find(m);
    if (pos == std::string::npos) continue;
    const int v = extract_number_before(seg, pos);
    if (v > 0) return v;
  }
  for (const auto& m : std::vector<std::string>{"sec", "s"}) {
    auto pos = lower.find(m);
    while (pos != std::string::npos) {
      // 避免匹配到 "photos" 等：要求前面是数字
      const int v = extract_number_before(lower, pos);
      if (v > 0) return v;
      pos = lower.find(m, pos + m.size());
    }
  }
  return 0;
}

bool looks_like_capture(const std::string& seg) {
  if (contains_any_utf8(seg, {"拍照", "拍张", "拍一", "拍个", "照片", "咔嚓", "帮我拍", "给我拍", "来张"})) {
    return true;
  }
  const std::string lower = to_lower_ascii(seg);
  if (contains_any_utf8(lower, {"take a photo", "take photo", "snap a photo", "take pictures"})) {
    return true;
  }
  // 不匹配协议词 capture_photo / task-capture-demo，避免测试与 API 名误触发
  const auto pai = seg.find("拍");
  if (pai == std::string::npos) return false;
  const int v = parse_chinese_or_digit_number(seg, pai + std::string("拍").size());
  return v > 0 && contains_any_utf8(seg.substr(pai), {"张", "照", "photo"});
}

bool looks_like_recording(const std::string& seg) {
  if (contains_any_utf8(seg, {"录制", "录像", "视频", "拍一段", "录一段", "录个", "开始录"})) {
    return true;
  }
  const std::string lower = to_lower_ascii(seg);
  return contains_any_utf8(lower, {"start recording", "record a video", "record video", "shoot a video"});
}

bool looks_like_status_query(const std::string& text) {
  return contains_any_utf8(text, {"完成了", "好了吗", "好了没", "成功了", "拍好了", "录好了", "怎么样了",
                                  "如何了", "有没有拍", "拍到了吗", "结果呢", "结果了吗", "结束了吗", "好了么"});
}

bool looks_like_capability_question(const std::string& text) {
  if (contains_any_utf8(text, {"能拍照", "可以拍照", "会拍照", "能不能拍", "可不可以拍", "支持拍照", "能录",
                               "可以录制", "会录像"})) {
    return true;
  }
  const std::string lower = to_lower_ascii(text);
  // 英文能力询问（避免 "can you take photo?" 被误判为 status/execute）
  return contains_any_utf8(lower, {"can you take photo", "can u take photo", "can you take a photo",
                                   "can u take a photo", "can you take photos", "can u take photos",
                                   "can you record", "can u record", "able to take photo",
                                   "do you support photo", "do you support taking"});
}

bool dialog_has_media_context(const DialogContext* dialog) {
  if (!dialog) return false;
  if (dialog->last_intent.find("capture") != std::string::npos) return true;
  if (dialog->last_intent.find("recording") != std::string::npos) return true;
  if (dialog->last_route.find("task") != std::string::npos) return true;
  if (dialog->last_route.find("action") != std::string::npos) return true;
  if (dialog->last_route.find("pipeline") != std::string::npos) return true;
  if (dialog->last_slots.is_object() &&
      (dialog->last_slots.contains("task_id") || dialog->last_slots.contains("action") ||
       dialog->last_slots.contains("pipeline"))) {
    return true;
  }
  return false;
}

std::vector<std::string> split_media_segments(const std::string& text) {
  std::vector<std::string> segs;
  std::string cur;
  auto flush = [&]() {
    size_t b = 0;
    while (b < cur.size() && (cur[b] == ' ' || cur[b] == '\t')) ++b;
    size_t e = cur.size();
    while (e > b && (cur[e - 1] == ' ' || cur[e - 1] == '\t')) --e;
    if (e > b) segs.push_back(cur.substr(b, e - b));
    cur.clear();
  };

  const std::vector<std::string> seps = {"，", ",", "。", "；", ";", "再", "然后", "接着", "并且", "之后"};
  size_t i = 0;
  while (i < text.size()) {
    bool hit = false;
    for (const auto& sep : seps) {
      if (text.compare(i, sep.size(), sep) == 0) {
        flush();
        i += sep.size();
        hit = true;
        break;
      }
    }
    if (hit) continue;
    cur.push_back(text[i]);
    ++i;
  }
  flush();
  if (segs.empty()) segs.push_back(text);
  return segs;
}

int clamp_count(int v) {
  if (v < 1) return 1;
  if (v > 9) return 9;
  return v;
}

int clamp_duration(int v) {
  if (v < 1) return 0;
  if (v > 120) return 120;
  return v;
}

MediaPlanStep make_capture_step(int count) {
  MediaPlanStep s;
  s.action = "capture_photo";
  const int c = clamp_count(count);
  if (c > 1) s.params["count"] = c;
  return s;
}

std::vector<MediaPlanStep> make_recording_steps(int duration_sec) {
  std::vector<MediaPlanStep> out;
  MediaPlanStep start;
  start.action = "start_recording";
  start.params["mode"] = "video";
  const int d = clamp_duration(duration_sec);
  if (d > 0) start.params["duration_sec"] = d;
  out.push_back(std::move(start));
  MediaPlanStep stop;
  stop.action = "stop_recording";
  out.push_back(std::move(stop));
  return out;
}

void append_recording(MediaPlan& plan, int duration_sec) {
  auto steps = make_recording_steps(duration_sec);
  plan.record_duration_sec = std::max(plan.record_duration_sec, clamp_duration(duration_sec));
  for (auto& s : steps) plan.steps.push_back(std::move(s));
}

}  // namespace

std::string dialogue_act_name(DialogueAct act) {
  switch (act) {
    case DialogueAct::Command:
      return "command";
    case DialogueAct::Question:
      return "question";
    default:
      return "other";
  }
}

DialogueAct classify_dialogue_act(const std::string& text) {
  const bool qmark = contains_any_utf8(text, {"吗", "呢", "么", "？", "?"});
  const bool status = looks_like_status_query(text);
  const bool capability = looks_like_capability_question(text);
  const bool strong = strong_command_hint(text);
  const bool soft_choice = contains_any_utf8(
      text, {"要不要", "是不是", "还是", "或者", "可不可以", "能不能", "先不", "再说", "等等"});

  if (capability) return DialogueAct::Question;
  if (status && !strong) return DialogueAct::Question;
  if (soft_choice && !strong) return DialogueAct::Question;
  if (qmark && !strong) return DialogueAct::Question;
  if (looks_like_capture(text) || looks_like_recording(text) || strong) return DialogueAct::Command;
  if (qmark) return DialogueAct::Question;
  return DialogueAct::Other;
}

MediaPlan parse_media_plan(const std::string& text, const DialogContext* dialog) {
  MediaPlan plan;
  plan.dialogue_act = classify_dialogue_act(text);

  const bool capture_like = looks_like_capture(text) ||
                            (contains_utf8(text, "拍") && contains_any_utf8(text, {"张", "照", "photo"}));
  const bool record_like = looks_like_recording(text);
  const bool status_like = looks_like_status_query(text);
  const bool capability = looks_like_capability_question(text);
  const bool dialog_media = dialog_has_media_context(dialog);

  plan.media_related = capture_like || record_like || capability || (status_like && dialog_media) ||
                       (status_like && (contains_utf8(text, "拍") || contains_utf8(text, "录")));
  if (!plan.media_related) {
    plan.parse_reason = "not_media";
    return plan;
  }

  if (capability) {
    plan.capability_question = true;
    plan.dialogue_act = DialogueAct::Question;
    plan.parse_reason = "capability_question";
    return plan;
  }

  // 仅明确状态词或有媒体执行上下文的追问才升为 status；「要不要拍照」「也许拍一张吧？」走歧义/澄清
  if (plan.dialogue_act == DialogueAct::Question && (status_like || dialog_media)) {
    if (status_like ||
        (dialog_media && contains_any_utf8(text, {"吗", "呢", "么", "？", "?", "完成", "好了", "结果"}))) {
      plan.status_query = true;
      plan.parse_reason = "status_query";
      return plan;
    }
  }

  if (should_block_side_effect_for_act(plan.dialogue_act)) {
    plan.parse_reason = "question_no_execute";
    return plan;
  }

  const auto segs = split_media_segments(text);
  for (const auto& seg : segs) {
    const bool seg_capture =
        looks_like_capture(seg) || (contains_utf8(seg, "拍") && contains_any_utf8(seg, {"张", "照", "photo"}));
    const bool seg_record = looks_like_recording(seg);

    if (seg_record && contains_any_utf8(seg, {"视频", "录像", "录制", "recording", "一段"})) {
      append_recording(plan, extract_duration_sec(seg));
      continue;
    }
    if (seg_capture) {
      const int count = extract_capture_count(seg, 1);
      plan.steps.push_back(make_capture_step(count));
      plan.capture_count += clamp_count(count);
      continue;
    }
    if (seg_record) {
      append_recording(plan, extract_duration_sec(seg));
    }
  }

  if (plan.steps.empty()) {
    if (record_like && contains_any_utf8(text, {"视频", "录像", "录制", "recording"})) {
      append_recording(plan, extract_duration_sec(text));
    } else if (capture_like) {
      const int count = extract_capture_count(text, 1);
      plan.steps.push_back(make_capture_step(count));
      plan.capture_count = clamp_count(count);
    }
  }

  plan.parse_reason = plan.steps.empty() ? "media_no_steps" : "media_plan";
  return plan;
}

void refine_media_plan_with_fuzzy(MediaPlan& plan,
                                  const nlohmann::json& fuzzy_intent,
                                  const DialogContext* dialog) {
  const std::string intent = fuzzy_intent.value("intent", "");
  const double conf = fuzzy_intent.value("confidence", 0.0);
  if (!is_media_family_intent(intent) || conf < intent_clarify_threshold()) return;

  plan.fuzzy_intent = intent;
  plan.fuzzy_confidence = conf;
  plan.media_related = true;

  // 能力询问优先，不被 status/execute 模糊结果覆盖
  if (plan.capability_question) return;

  if (intent == "media_capture_status") {
    plan.dialogue_act = DialogueAct::Question;
    plan.status_query = true;
    plan.steps.clear();
    plan.parse_reason = "fuzzy_media_capture_status";
    return;
  }

  if (intent == "media_capture_execute" || intent == "media_recording_execute" || intent == "media_plan") {
    if (plan.status_query && dialog_has_media_execute_context(dialog) &&
        plan.parse_reason == "status_query") {
      return;
    }
    if (should_block_side_effect_for_act(plan.dialogue_act) && plan.steps.empty() &&
        !plan.status_query) {
      return;
    }
    if (plan.steps.empty() && intent == "media_capture_execute") {
      plan.dialogue_act = DialogueAct::Command;
      plan.status_query = false;
      plan.parse_reason = plan.parse_reason == "not_media" ? "fuzzy_media_capture_execute" : plan.parse_reason;
    } else if (plan.steps.empty() && intent == "media_recording_execute") {
      plan.dialogue_act = DialogueAct::Command;
      plan.parse_reason = plan.parse_reason == "not_media" ? "fuzzy_media_recording_execute" : plan.parse_reason;
    } else if (intent == "media_plan" && media_plan_needs_pipeline(plan)) {
      plan.dialogue_act = DialogueAct::Command;
      plan.parse_reason = "fuzzy_media_plan";
    } else if (!plan.steps.empty()) {
      plan.dialogue_act = DialogueAct::Command;
      if (plan.parse_reason == "media_plan" || plan.parse_reason == "media_no_steps") {
        plan.parse_reason = "media_plan+fuzzy:" + intent;
      }
    }
  }
}

bool is_ambiguous_media_utterance(const std::string& text, const DialogContext* dialog) {
  const MediaPlan plan = parse_media_plan(text, dialog);
  if (plan.capability_question) return false;
  if (plan.status_query) return false;

  auto has = [&](const std::string& n) { return text.find(n) != std::string::npos; };
  const bool media_kw = has("拍照") || has("拍张") || has("拍一") || has("照片") || has("录制") ||
                        has("录像") || has("视频") || has("帮我拍") || has("给我拍") ||
                        (has("拍") && (has("张") || has("照")));
  if (!media_kw && !plan.media_related) return false;

  const bool q = has("吗") || has("呢") || has("么") || has("？") || has("?") || has("是不是") ||
                 has("要不要") || has("还是") || has("或者") || has("先不") || has("等等") || has("再说") ||
                 has("可不可以") || has("能不能");
  const bool hedge = has("也许") || has("可能") || has("好像") || has("感觉") || has("考虑") || has("想想") ||
                     has("犹豫");
  const bool choice = has("还是") || has("或者");
  const bool soft_q = (has("吧") || has("呗")) && (has("？") || has("?"));

  if (media_kw && (q || hedge || choice || soft_q)) return true;
  if (plan.media_related && plan.dialogue_act == DialogueAct::Question && !plan.status_query &&
      !plan.capability_question) {
    return true;
  }
  return false;
}

void refine_media_plan_with_router(MediaPlan& plan,
                                   const nlohmann::json& router_json,
                                   const DialogContext* dialog) {
  if (!router_json.is_object()) return;
  const bool ok = router_json.value("ok", false) || router_json.contains("intent");
  if (!ok && !router_json.contains("dialogue_act")) return;

  std::string intent = router_json.value("intent", "");
  if (intent == "capture_photo" || intent == "task_capture" || intent == "action_capture") {
    intent = "media_capture_execute";
  } else if (intent == "query_capture" || intent == "capture_status") {
    intent = "media_capture_status";
  } else if (intent == "start_recording" || intent == "recording") {
    intent = "media_recording_execute";
  }

  if (router_json.contains("dialogue_act") && router_json["dialogue_act"].is_string()) {
    const std::string act = router_json["dialogue_act"].get<std::string>();
    if (act == "question") plan.dialogue_act = DialogueAct::Question;
    else if (act == "command") plan.dialogue_act = DialogueAct::Command;
  }

  plan.fuzzy_intent = intent.empty() ? plan.fuzzy_intent : intent;
  if (router_json.contains("confidence") && router_json["confidence"].is_number()) {
    plan.fuzzy_confidence = router_json["confidence"].get<double>();
  }

  if (intent == "media_capture_status" ||
      (plan.dialogue_act == DialogueAct::Question && is_media_family_intent(intent))) {
    plan.media_related = true;
    plan.status_query = true;
    plan.capability_question = false;
    plan.steps.clear();
    plan.parse_reason = "router_media_capture_status";
    return;
  }

  if (intent == "media_capture_execute" || intent == "media_recording_execute" || intent == "media_plan") {
    plan.media_related = true;
    plan.dialogue_act = DialogueAct::Command;
    plan.status_query = false;
    plan.parse_reason = "router_" + intent;
    if (plan.steps.empty() && intent == "media_capture_execute") {
      plan.steps.push_back(make_capture_step(1));
      plan.capture_count = 1;
    } else if (plan.steps.empty() && intent == "media_recording_execute") {
      append_recording(plan, 0);
    }
  }

  if (router_json.contains("task_pipeline") && router_json["task_pipeline"].is_array()) {
    plan.steps.clear();
    plan.capture_count = 0;
    plan.record_duration_sec = 0;
    for (const auto& step : router_json["task_pipeline"]) {
      if (!step.is_object() || !step.contains("action") || !step["action"].is_string()) continue;
      MediaPlanStep s;
      s.action = step["action"].get<std::string>();
      if (step.contains("params") && step["params"].is_object()) s.params = step["params"];
      if (s.action == "capture_photo") {
        int c = 1;
        if (s.params.contains("count") && s.params["count"].is_number_integer()) {
          c = s.params["count"].get<int>();
        }
        if (c < 1) c = 1;
        if (c > 9) c = 9;
        plan.capture_count += c;
        if (c > 1) s.params["count"] = c;
      }
      if (s.action == "start_recording" && s.params.contains("duration_sec") &&
          s.params["duration_sec"].is_number_integer()) {
        int d = s.params["duration_sec"].get<int>();
        if (d < 1) d = 0;
        if (d > 120) d = 120;
        plan.record_duration_sec = std::max(plan.record_duration_sec, d);
      }
      plan.steps.push_back(std::move(s));
    }
    if (!plan.steps.empty()) {
      plan.media_related = true;
      plan.dialogue_act = DialogueAct::Command;
      plan.parse_reason = "router_task_pipeline";
    }
  }

  (void)dialog;
}

bool media_plan_needs_pipeline(const MediaPlan& plan) {
  if (plan.steps.size() > 1) return true;
  if (plan.capture_count > 1) return true;
  if (plan.record_duration_sec > 0) return true;
  if (plan.steps.size() == 1) {
    const auto& p = plan.steps[0].params;
    if (p.contains("count") && p["count"].is_number_integer() && p["count"].get<int>() > 1) return true;
    if (p.contains("duration_sec")) return true;
  }
  return false;
}

bool should_block_side_effect_for_act(DialogueAct act) {
  return act == DialogueAct::Question;
}

}  // namespace thin_agent
