#include "thin_agent/core/IntentOnnx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <unordered_map>

#include "onnxruntime_cxx_api.h"

#include "thin_agent/core/ChatPolicy.h"
#include "thin_agent/core/DialogSlotRecall.h"
#include "thin_agent/core/IntentScorer.h"

// IntentOnnx：ONNX Runtime 轻量意图推理（profile stub + 多意图特征分类器）。

namespace thin_agent {
namespace {

constexpr int kFeatureDim = 64;
constexpr int kLegacyDim = 16;
constexpr int kNgramBins = 48;
constexpr int kNumClasses = 12;
// 8 salt seeds — match intent_features.py HASH_SALTS
constexpr int kHashSalt[] = {17, 31, 53, 79, 97, 113, 131, 157};

constexpr std::array<const char*, kNumClasses> kIntentLabels = {
    "unknown",      "profile",         "weather",     "news",
    "status",       "general",         "memory_recent", "event_recent",
    "memory_history", "media_capture", "media_review", "media_share",
};

/// 在候选目录中查找 ONNX 模型文件路径。
std::string resolve_intent_model_path(const char* filename) {
  static const std::vector<std::string> roots = {
      "models/intent/",
      "../models/intent/",
      "/root/code/thin_agent/models/intent/",
  };
  for (const auto& root : roots) {
    const std::string p = root + filename;
    if (std::filesystem::exists(p)) return p;
  }
  return roots.front() + filename;
}

/// 关键词命中得分：完全匹配 1.0，前两字部分匹配 0.35。
double keyword_hit_score(const std::string& norm, const char* key) {
  double best = 0.0;
  for (const auto& kw : policy_keywords(key)) {
    if (kw.empty()) continue;
    const std::string k = normalize_text_for_policy(kw);
    if (k.empty()) continue;
    if (norm.find(k) != std::string::npos) best = std::max(best, 1.0);
    else if (k.size() >= 2 && norm.find(k.substr(0, 2)) != std::string::npos) best = std::max(best, 0.35);
  }
  static const std::unordered_map<std::string, std::vector<const char*>> kBuiltin = {
      {"weather", {"天气", "weather", "天况", "气温"}},
      {"weather_extra", {"查天气", "看天气"}},
      {"news", {"新闻", "头条", "快讯", "news"}},
      {"profile", {"你是谁", "介绍", "who", "profile", "yourself", "ability", "who r u"}},
      {"profile_short", {"who r u"}},
      {"profile_detail", {"detail ability"}},
      {"status", {"状态", "模型", "status", "model"}},
      {"model_status", {"什么模型", "用的什么"}},
      {"memory_recent", {"短期记忆", "memory_recent", "会话记忆"}},
      {"memory_history", {"历史记忆", "memory_history"}},
      {"memory_search", {"记忆检索", "memory_search"}},
      {"general", {"查一下", "搜一下", "帮我查"}},
      {"event", {"事件", "event"}},
  };
  const auto it = kBuiltin.find(key);
  if (it != kBuiltin.end()) {
    for (const char* kw : it->second) {
      const std::string k = normalize_text_for_policy(kw);
      if (k.empty()) continue;
      if (norm.find(k) != std::string::npos) best = std::max(best, 1.0);
      else if (k.size() >= 2 && norm.find(k.substr(0, 2)) != std::string::npos) best = std::max(best, 0.35);
    }
  }
  return best;
}

/// Hash char 1/2/3-grams into kNgramBins buckets — mirrors intent_features.py extract_ngram_bins.
/// Uses FNV-1a 32-bit (deterministic, identical to Python fnv1a_32).
namespace {
/// FNV-1a 32 位哈希，与 Python intent_features.py 保持一致。
uint32_t fnv1a_32(const std::string& s) {
  uint32_t h = 0x811C9DC5u;
  for (unsigned char ch : s) {
    h ^= static_cast<uint32_t>(ch);
    h *= 0x01000193u;
  }
  return h;
}
}  // namespace

std::vector<float> extract_ngram_bins(const std::string& norm) {
  std::vector<float> bins(static_cast<size_t>(kNgramBins), 0.0f);
  // strip spaces for ngram extraction
  std::string compact;
  for (char ch : norm) {
    if (ch != ' ') compact.push_back(ch);
  }
  if (compact.empty()) return bins;
  for (int n = 1; n <= 3; ++n) {
    for (size_t i = 0; i + static_cast<size_t>(n) <= compact.size(); ++i) {
      std::string ng = compact.substr(i, static_cast<size_t>(n));
      size_t h = static_cast<size_t>(fnv1a_32(ng));
      for (int salt : kHashSalt) {
        size_t idx = (h ^ static_cast<size_t>(salt)) % static_cast<size_t>(kNgramBins);
        bins[idx] += 1.0f;
      }
    }
  }
  float mx = 0.0f;
  for (float v : bins) { if (v > mx) mx = v; }
  if (mx <= 0.0f) mx = 1.0f;
  for (float& v : bins) v = std::min(v / mx, 1.0f);
  return bins;
}

/// 构建 64 维意图特征向量：关键词命中 + 对话上下文 + n-gram 哈希桶。
std::vector<float> extract_intent_features(const std::string& text, const DialogContext* dialog) {
  const std::string norm = normalize_text_for_policy(text);
  std::vector<float> feat(static_cast<size_t>(kFeatureDim), 0.0f);
  feat[0] = static_cast<float>(std::max({keyword_hit_score(norm, "weather"),
                                          keyword_hit_score(norm, "weather_extra")}));
  feat[1] = static_cast<float>(keyword_hit_score(norm, "news"));
  feat[2] = static_cast<float>(std::max({keyword_hit_score(norm, "profile"),
                                         keyword_hit_score(norm, "profile_short"),
                                         keyword_hit_score(norm, "profile_detail")}));
  feat[3] = static_cast<float>(std::max({keyword_hit_score(norm, "status"),
                                         keyword_hit_score(norm, "model_status")}));
  feat[4] = static_cast<float>(std::max({keyword_hit_score(norm, "memory_recent"),
                                         keyword_hit_score(norm, "memory_history"),
                                         keyword_hit_score(norm, "memory_search")}));
  feat[5] = static_cast<float>(keyword_hit_score(norm, "general"));
  feat[6] = has_referential_context_markers(text) ? 1.0f : 0.0f;
  if (dialog && dialog->ttl_turns > 0) {
    if (dialog->last_intent == "weather") feat[7] = 1.0f;
    if (dialog->last_intent == "news") feat[8] = 1.0f;
  }
  feat[9] = detect_city_slot(norm).empty() ? 0.0f : 1.0f;
  feat[10] = detect_news_topic_slot(norm).empty() ? 0.0f : 1.0f;
  feat[11] = static_cast<float>(std::min(norm.size(), size_t{48})) / 48.0f;
  feat[12] = (norm.find_first_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos) ? 1.0f : 0.0f;
  feat[13] = static_cast<float>(keyword_hit_score(norm, "event"));
  feat[14] = static_cast<float>(keyword_hit_score(norm, "memory_recent"));
  feat[15] = 1.0f;
  // ngram hash bins (dims 16-63) — mirrors intent_features.py extract_ngram_bins
  {
    auto bins = extract_ngram_bins(norm);
    for (int i = 0; i < kNgramBins; ++i) {
      feat[static_cast<size_t>(kLegacyDim + i)] = bins[static_cast<size_t>(i)];
    }
  }
  return feat;
}

/// 按 ONNX 预测意图填充槽位（与 IntentScorer 规则侧对齐）。
nlohmann::json build_slots_for_onnx_intent(const std::string& intent,
                                           const std::string& text,
                                           const std::string& norm) {
  nlohmann::json slots = nlohmann::json::object();
  if (intent == "weather") {
    const std::string city = resolve_weather_city_candidate(text, norm);
    if (!city.empty()) slots["city"] = city;
    const std::string date = resolve_weather_date_slot(norm);
    if (!date.empty()) slots["date"] = date;
  } else if (intent == "news") {
    const std::string topic = detect_news_topic_slot(norm);
    if (!topic.empty()) slots["topic_or_scope"] = topic;
    const std::string tr = resolve_news_time_range_slot(norm);
    if (!tr.empty()) slots["time_range"] = tr;
  } else if (intent == "profile") {
    slots["profile_mode"] = "concise";
    for (const auto& m : profile_detail_markers()) {
      if (norm.find(m) != std::string::npos) {
        slots["profile_mode"] = "detailed";
        break;
      }
    }
  }
  return slots;
}

/// ONNX 多意图会话单例持有者（懒加载 Session）。
struct OnnxIntentSession {
  std::unique_ptr<Ort::Env> env;
  std::unique_ptr<Ort::Session> session;
  std::string model_path;
};

OnnxIntentSession& multiclass_session() {
  static OnnxIntentSession holder;
  return holder;
}

/// 懒加载 intent_multiclass.onnx 会话；路径变化时重建。
bool ensure_multiclass_session(std::string* error) {
  auto& holder = multiclass_session();
  const std::string path = resolve_intent_model_path("intent_multiclass.onnx");
  if (holder.session && holder.model_path == path) return true;
  try {
    holder.env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "thin_agent_intent_mc");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
    holder.session = std::make_unique<Ort::Session>(*holder.env, path.c_str(), opts);
    holder.model_path = path;
    return true;
  } catch (const std::exception& e) {
    holder.session.reset();
    if (error) *error = e.what();
    return false;
  }
}

/// 运行多分类 ONNX 推理，输出 logits 向量。
bool run_multiclass_onnx(const std::vector<float>& features,
                         std::vector<float>* logits,
                         std::string* error) {
  if (!ensure_multiclass_session(error)) return false;
  auto& holder = multiclass_session();
  try {
    std::vector<int64_t> shape = {1, static_cast<int64_t>(features.size())};
    Ort::MemoryInfo mi = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        mi, const_cast<float*>(features.data()), features.size(), shape.data(), shape.size());

    const char* input_names[] = {"features"};
    const char* output_names[] = {"logits"};
    auto outputs = holder.session->Run(Ort::RunOptions{nullptr},
                                       input_names,
                                       &input_tensor,
                                       1,
                                       output_names,
                                       1);
    if (outputs.empty() || !outputs[0].IsTensor()) {
      if (error) *error = "onnx multiclass output missing";
      return false;
    }
    const float* out = outputs[0].GetTensorData<float>();
    const auto info = outputs[0].GetTensorTypeAndShapeInfo();
    const size_t count = info.GetElementCount();
    if (!out || count < static_cast<size_t>(kNumClasses)) {
      if (error) *error = "onnx multiclass logits size mismatch";
      return false;
    }
    logits->assign(out, out + kNumClasses);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

/// 返回 logits 中最大值的索引。
int argmax(const std::vector<float>& logits) {
  return static_cast<int>(std::max_element(logits.begin(), logits.end()) - logits.begin());
}

/// 计算指定类别的 softmax 概率。
float softmax_max(const std::vector<float>& logits, int idx) {
  float max_logit = *std::max_element(logits.begin(), logits.end());
  double sum = 0.0;
  for (float v : logits) sum += std::exp(static_cast<double>(v - max_logit));
  if (sum <= 0.0) return 0.0f;
  return static_cast<float>(std::exp(static_cast<double>(logits[static_cast<size_t>(idx)] - max_logit)) / sum);
}

}  // namespace

bool intent_onnx_enabled() {
  const char* env = std::getenv("THIN_AGENT_INTENT_ONNX");
  if (!env || !*env) return false;
  const std::string v(env);
  return v == "1" || v == "true" || v == "on";
}

bool infer_profile_confidence_onnx(const std::string& model_path,
                                   double* confidence,
                                   std::string* error) {
  if (!confidence) {
    if (error) *error = "confidence pointer is null";
    return false;
  }
  try {
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "thin_agent_intent");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_DISABLE_ALL);

    Ort::Session session(env, model_path.c_str(), opts);

    std::vector<int64_t> input_ids(8, 1);
    std::vector<int64_t> input_shape = {1, 8};

    Ort::MemoryInfo mi = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input_tensor = Ort::Value::CreateTensor<int64_t>(
        mi, input_ids.data(), input_ids.size(), input_shape.data(), input_shape.size());

    const char* input_names[] = {"input_ids"};
    const char* output_names[] = {"logits"};

    auto output_tensors = session.Run(Ort::RunOptions{nullptr},
                                      input_names,
                                      &input_tensor,
                                      1,
                                      output_names,
                                      1);

    if (output_tensors.empty() || !output_tensors[0].IsTensor()) {
      if (error) *error = "onnx output tensor missing";
      return false;
    }

    const float* out = output_tensors[0].GetTensorData<float>();
    if (!out) {
      if (error) *error = "onnx output data null";
      return false;
    }

    *confidence = static_cast<double>(out[0]);
    if (*confidence < 0.0) *confidence = 0.0;
    if (*confidence > 1.0) *confidence = 1.0;
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

nlohmann::json classify_intent_onnx(const std::string& text, const DialogContext* dialog) {
  if (!intent_onnx_enabled()) {
    return {{"intent", "unknown"}, {"confidence", 0.0}, {"backend", "onnx"}, {"slots", nlohmann::json::object()}};
  }

  const std::string norm = normalize_text_for_policy(text);
  const auto features = extract_intent_features(text, dialog);
  std::vector<float> logits;
  std::string err;
  if (!run_multiclass_onnx(features, &logits, &err)) {
    return {{"intent", "unknown"},
            {"confidence", 0.0},
            {"backend", "onnx"},
            {"slots", nlohmann::json::object()},
            {"model_error", err}};
  }

  const int best = argmax(logits);
  std::string intent = kIntentLabels[static_cast<size_t>(best)];
  float confidence = softmax_max(logits, best);

  // 对话延续：指代追问优先保持上一轮 weather/news（区别于 location/topic recall 句）
  if (dialog && dialog->ttl_turns > 0 && has_referential_context_markers(text)) {
    if (dialog->last_intent == "weather" && !is_weather_location_recall_query(text) &&
        (keyword_hit_score(norm, "weather") > 0.0 || keyword_hit_score(norm, "weather_extra") > 0.0)) {
      intent = "weather";
      confidence = std::max(confidence, static_cast<float>(intent_execute_threshold()));
    } else if (dialog->last_intent == "news" && !is_news_topic_recall_query(text) &&
               keyword_hit_score(norm, "news") > 0.0) {
      intent = "news";
      confidence = std::max(confidence, 0.83f);
    }
  }

  if (intent == "unknown") confidence = std::min(confidence, 0.2f);

  nlohmann::json slots = build_slots_for_onnx_intent(intent, text, norm);
  if (intent == "weather" && slots.value("city", "").empty() && dialog && dialog->ttl_turns > 0 &&
      dialog->last_intent == "weather" && dialog->last_slots.is_object()) {
    const auto& ls = dialog->last_slots;
    if (ls.contains("city") && ls["city"].is_string()) slots["city"] = ls["city"];
  }
  if (intent == "news" && slots.value("topic_or_scope", "").empty() && dialog && dialog->ttl_turns > 0 &&
      dialog->last_intent == "news" && dialog->last_slots.is_object()) {
    const auto& ls = dialog->last_slots;
    if (ls.contains("topic_or_scope") && ls["topic_or_scope"].is_string()) {
      slots["topic_or_scope"] = ls["topic_or_scope"];
    }
  }

  if (intent == "weather" && !slots.value("city", "").empty()) {
    confidence = std::max(confidence, static_cast<float>(intent_execute_threshold()));
  } else if (intent == "news" && !slots.value("topic_or_scope", "").empty()) {
    confidence = std::max(confidence, 0.83f);
  } else if (intent == "profile") {
    confidence = std::max(confidence, 0.86f);
  }

  return {
      {"intent", intent},
      {"confidence", confidence},
      {"backend", "onnx"},
      {"slots", std::move(slots)},
  };
}

}  // namespace thin_agent
