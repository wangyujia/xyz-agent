// unit_intent_onnx：ONNX 意图推理全部边界 — 环境变量开关、各意图分类、对话上下文、错误路径。

#include <cstdlib>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "thin_agent/core/IntentOnnx.h"
#include "thin_agent/core/IntentScorer.h"

namespace {

int g_failures = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << '\n';
    ++g_failures;
  }
}

void set_env(const char* k, const char* v) { ::setenv(k, v, 1); }
void unset_env(const char* k) { ::unsetenv(k); }

}  // namespace

int main() {
  using thin_agent::classify_intent_onnx;
  using thin_agent::DialogContext;
  using thin_agent::intent_execute_threshold;
  using thin_agent::intent_onnx_enabled;
  using thin_agent::infer_profile_confidence_onnx;

  // ═══════════════ intent_onnx_enabled ═══════════════
  {
    unset_env("THIN_AGENT_INTENT_ONNX");
    expect(!intent_onnx_enabled(), "env unset → false");
  }
  {
    set_env("THIN_AGENT_INTENT_ONNX", "");
    expect(!intent_onnx_enabled(), "env empty → false");
  }
  {
    set_env("THIN_AGENT_INTENT_ONNX", "0");
    expect(!intent_onnx_enabled(), "env=0 → false");
  }
  {
    set_env("THIN_AGENT_INTENT_ONNX", "false");
    expect(!intent_onnx_enabled(), "env=false → false");
  }
  {
    set_env("THIN_AGENT_INTENT_ONNX", "off");
    expect(!intent_onnx_enabled(), "env=off → false");
  }
  {
    set_env("THIN_AGENT_INTENT_ONNX", "garbage");
    expect(!intent_onnx_enabled(), "env=garbage → false");
  }

  // ═══════════════ classify_intent_onnx: default-off state ═══════════════
  {
    unset_env("THIN_AGENT_INTENT_ONNX");
    auto off = classify_intent_onnx("今天上海天气", nullptr);
    expect(off["intent"] == "unknown", "off: weather → unknown");
    expect(off["confidence"].get<double>() == 0.0, "off: confidence=0");
    expect(off["backend"] == "onnx", "off: backend=onnx");
    expect(off["slots"].is_object(), "off: slots is object");
  }

  // ═══════════════ Enable ONNX ═══════════════
  set_env("THIN_AGENT_INTENT_ONNX", "1");
  expect(intent_onnx_enabled(), "env=1 → true");
  set_env("THIN_AGENT_INTENT_ONNX", "true");
  expect(intent_onnx_enabled(), "env=true → true");
  set_env("THIN_AGENT_INTENT_ONNX", "on");
  expect(intent_onnx_enabled(), "env=on → true");
  set_env("THIN_AGENT_INTENT_ONNX", "1");

  // ═══════════════ classify_intent_onnx: weather (Chinese) ═══════════════
  {
    auto weather = classify_intent_onnx("今天上海天气", nullptr);
    expect(weather["intent"] == "weather", "onnx weather intent");
    expect(weather["backend"] == "onnx", "onnx weather backend");
    expect(weather["slots"]["city"] == "上海", "onnx weather city slot");
    expect(weather["confidence"].get<double>() >= intent_execute_threshold(), "onnx weather confidence");
  }

  // weather with date
  {
    auto w = classify_intent_onnx("明天北京天气怎么样", nullptr);
    expect(w["intent"] == "weather", "onnx weather with date");
    expect(w["slots"]["city"] == "北京", "onnx weather city=北京");
  }

  // weather English (model trained on Chinese, may fallback)
  {
    auto w = classify_intent_onnx("weather in London", nullptr);
    auto intent = w["intent"].get<std::string>();
    expect(intent == "weather" || intent == "unknown", "onnx weather English (weather or unknown)");
  }

  // ═══════════════ classify_intent_onnx: profile ═══════════════
  {
    auto profile = classify_intent_onnx("who r u", nullptr);
    expect(profile["intent"] == "profile", "onnx profile intent");
    expect(profile["confidence"].get<double>() >= 0.75, "onnx profile confidence");
    expect(profile["slots"]["profile_mode"] == "concise", "onnx profile mode=concise");
  }

  // profile detailed
  {
    auto profile = classify_intent_onnx("你是谁，详细介绍一下你的能力", nullptr);
    expect(profile["intent"] == "profile", "onnx profile detailed intent");
    expect(profile["slots"]["profile_mode"] == "detailed", "onnx profile mode=detailed");
  }

  // ═══════════════ classify_intent_onnx: news ═══════════════
  {
    auto news = classify_intent_onnx("今天有什么新闻", nullptr);
    auto intent = news["intent"].get<std::string>();
    expect(intent == "news" || intent == "general", "onnx news intent (news or general)");
    expect(news["backend"] == "onnx", "onnx news backend");
  }

  // ═══════════════ classify_intent_onnx: unknown ═══════════════
  {
    auto unknown = classify_intent_onnx("xyzzy-tier2-classify-demo-token", nullptr);
    expect(unknown["intent"] == "unknown", "onnx unknown intent for tier2 token");
    if (unknown["intent"] == "unknown") {
      expect(unknown["confidence"].get<double>() <= 0.3, "onnx unknown confidence <= 0.3");
    }
  }

  // ═══════════════ classify_intent_onnx: status (accept any — classifier varies) ═══════════════
  {
    auto status = classify_intent_onnx("你用的什么模型", nullptr);
    // The ONNX model may classify this as status, profile, general, or unknown.
    // The key assertion: the function returns valid JSON without crashing.
    expect(status["backend"] == "onnx", "onnx status backend");
    expect(status.contains("intent"), "onnx status has intent");
    expect(status.contains("slots"), "onnx status has slots");
  }

  // ═══════════════ classify_intent_onnx: dialog context (weather) ═══════════════
  {
    DialogContext ctx;
    ctx.last_intent = "weather";
    ctx.last_slots = nlohmann::json{{"city", "上海"}};
    ctx.ttl_turns = 3;

    auto ctx_weather = classify_intent_onnx("今天天气如何？", &ctx);
    expect(ctx_weather["intent"] == "weather", "onnx ctx weather intent");
    expect(ctx_weather["slots"]["city"] == "上海", "onnx ctx weather inherits city");
  }

  // ═══════════════ classify_intent_onnx: dialog context (news) ═══════════════
  {
    DialogContext ctx;
    ctx.last_intent = "news";
    ctx.last_slots = nlohmann::json{{"topic_or_scope", "AI"}};
    ctx.ttl_turns = 2;

    auto ctx_news = classify_intent_onnx("还有什么新闻", &ctx);
    expect(ctx_news["intent"] == "news", "onnx ctx news intent");
    expect(ctx_news["slots"]["topic_or_scope"] == "AI", "onnx ctx news inherits topic");
  }

  // ═══════════════ classify_intent_onnx: memory_history ═══════════════
  {
    auto mh = classify_intent_onnx("查看我的历史记忆", nullptr);
    expect(mh["intent"] == "memory_history", "onnx memory_history intent");
    expect(mh["confidence"].get<double>() >= 0.3, "onnx memory_history confidence");
  }

  // ═══════════════ classify_intent_onnx: memory_recent ═══════════════
  {
    auto mr = classify_intent_onnx("帮我回忆一下短期记忆", nullptr);
    auto intent = mr["intent"].get<std::string>();
    expect(intent == "memory_recent" || intent == "memory_history", "onnx memory intent");
  }

  // ═══════════════ classify_intent_onnx: null dialog ═══════════════
  {
    auto n = classify_intent_onnx("hello world", nullptr);
    expect(n["backend"] == "onnx", "onnx null dialog → has backend");
    expect(n.contains("intent"), "onnx null dialog → has intent");
  }

  // ═══════════════ infer_profile_confidence_onnx: error paths ═══════════════
  {
    std::string err;
    bool ok = infer_profile_confidence_onnx("/dummy.onnx", nullptr, &err);
    expect(!ok, "infer_profile null confidence → false");
    expect(err == "confidence pointer is null", "infer_profile null confidence error msg");
  }

  {
    double conf = 0.0;
    std::string err;
    bool ok = infer_profile_confidence_onnx("/nonexistent/path/intent.onnx", &conf, &err);
    expect(!ok, "infer_profile nonexistent file → false");
    expect(!err.empty(), "infer_profile nonexistent file has error msg");
    expect(conf == 0.0, "infer_profile nonexistent confidence stays 0");
  }

  {
    double conf = 0.0;
    bool ok = infer_profile_confidence_onnx("/nonexistent.onnx", &conf, nullptr);
    expect(!ok, "infer_profile null error ptr → false");
  }

  unset_env("THIN_AGENT_INTENT_ONNX");

  if (g_failures != 0) {
    std::cerr << g_failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "unit:test_intent_onnx PASS\n";
  return 0;
}
