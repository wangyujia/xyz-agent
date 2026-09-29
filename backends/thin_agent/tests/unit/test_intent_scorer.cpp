// unit_intent_scorer：IntentScorer 模糊意图打分与 profile 判定单测。

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "thin_agent/core/ChatPolicy.h"
#include "thin_agent/core/IntentScorer.h"

namespace {

int check(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    return 1;
  }
  return 0;
}

}  // namespace

int main() {
  using thin_agent::classify_intent_fuzzy;
  using thin_agent::intent_clarify_threshold;
  using thin_agent::intent_execute_threshold;
  using thin_agent::is_profile_like_text;
  using thin_agent::is_weather_date_token;
  using thin_agent::normalize_text_for_policy;
  using thin_agent::policy_text;
  using thin_agent::resolve_weather_city_candidate;
  using thin_agent::should_abort_slot_for_intent_switch;
  using thin_agent::top_intent_scores;

  if (int rc = check(intent_execute_threshold() >= intent_clarify_threshold(), "execute >= clarify"); rc) return rc;

  // --- ChatPolicy 槽位辅助（与 IntentScorer 天气槽位共用）---
  if (int rc = check(is_weather_date_token(normalize_text_for_policy("今天")), "今天 is date token"); rc) return rc;
  if (int rc = check(is_weather_date_token(normalize_text_for_policy("明天")), "明天 is date token"); rc) return rc;
  if (int rc = check(!is_weather_date_token(normalize_text_for_policy("上海")), "上海 not date token"); rc) return rc;

  if (int rc = check(resolve_weather_city_candidate("上海天气", normalize_text_for_policy("上海天气")) == "上海",
                      "resolve city shanghai");
      rc)
    return rc;
  if (int rc = check(resolve_weather_city_candidate("深圳的呢", normalize_text_for_policy("深圳的呢")) == "深圳",
                      "resolve city shenzhen followup");
      rc)
    return rc;
  if (int rc = check(resolve_weather_city_candidate("今天天气如何？", normalize_text_for_policy("今天天气如何？")).empty(),
                      "today weather no city slot");
      rc)
    return rc;
  if (int rc = check(resolve_weather_city_candidate("看新闻", normalize_text_for_policy("看新闻")).empty(),
                      "news text not city");
      rc)
    return rc;

  if (int rc = check(should_abort_slot_for_intent_switch(normalize_text_for_policy("看新闻"), "weather"),
                      "abort weather slot for news");
      rc)
    return rc;
  if (int rc = check(!should_abort_slot_for_intent_switch(normalize_text_for_policy("深圳"), "weather"),
                      "shenzhen does not abort weather slot");
      rc)
    return rc;

  // --- profile / anchor 类输入 ---
  if (int rc = check(is_profile_like_text("who are you"), "profile_like who are you"); rc) return rc;
  if (int rc = check(is_profile_like_text("你是谁呀"), "profile_like 你是谁呀"); rc) return rc;
  if (int rc = check(!is_profile_like_text("你的模型是什么"), "not profile_like model query"); rc) return rc;
  if (int rc = check(thin_agent::localize_weather_condition("Thundery outbreaks in nearby", "zh").find("雷") != std::string::npos,
                      "weather zh thundery nearby"); rc)
    return rc;
  if (int rc = check(!is_profile_like_text("查天气"), "not profile_like weather"); rc) return rc;

  auto profile_fuzzy = classify_intent_fuzzy("who are you", nullptr);
  if (int rc = check(profile_fuzzy.value("intent", "") == "profile", "fuzzy profile intent"); rc) return rc;
  if (int rc = check(profile_fuzzy.value("confidence", 0.0) >= intent_execute_threshold(), "fuzzy profile execute conf");
      rc)
    return rc;

  // --- v0.46.0: autonomy 意图（巡检/监控/目标/快照/遗忘/摘要）---
  // 回归：kIntentOrder 曾无 autonomy → 配置 keywords.autonomy 不参与打分
  //（死配置），"监控文件"措辞落 unknown 或（含"状态/运行情况"）被
  // status 吸走 → local 模板。加入评分列表后须分类为 autonomy 且高置信。
  const char* kAutonomyCases[] = {
      "用 monitor_watch_file 监控 /tmp/mon_test.txt 文件变化，然后告诉我状态（先创建这个文件）",
      "用 monitor_watch_file 工具监控文件 /tmp/mon_watch.txt 的变化",
      "执行系统巡检 patrol_now，报告磁盘/内存/进程状态",
      "用 patrol_now 工具做一次系统巡检",
      "调用 monitor_watch_file 监控文件 /tmp/mon_test.txt",
      "监控 /tmp/mon_test.txt 文件变化",
      "查看监控状态",
  };
  for (const auto* t : kAutonomyCases) {
    auto r = classify_intent_fuzzy(t, nullptr);
    std::string label = std::string("autonomy fuzzy: ") + t;
    if (int rc = check(r.value("intent", "") == "autonomy", label.c_str()); rc) return rc;
    if (int rc = check(r.value("confidence", 0.0) >= intent_execute_threshold(), "autonomy conf"); rc) return rc;
  }
  // 回归：status 正常提问不受 autonomy 影响
  auto status_fuzzy = classify_intent_fuzzy("你现在什么状态", nullptr);
  if (int rc = check(status_fuzzy.value("intent", "") == "status", "status unchanged"); rc) return rc;
  // 回归：profile 不受影响
  auto prof2 = classify_intent_fuzzy("你是谁", nullptr);
  if (int rc = check(prof2.value("intent", "") == "profile", "profile unchanged"); rc) return rc;

  auto profile_variant = classify_intent_fuzzy("你是谁呀", nullptr);
  if (int rc = check(profile_variant.value("intent", "") == "profile", "fuzzy profile variant intent"); rc) return rc;
  if (int rc = check(profile_variant.value("confidence", 0.0) >= intent_clarify_threshold(),
                      "fuzzy profile variant above clarify");
      rc)
    return rc;

  // --- weather：无城市时应澄清，不应把「今天」当城市 ---
  auto today_weather = classify_intent_fuzzy("今天天气如何？", nullptr);
  if (int rc = check(today_weather.value("intent", "") == "weather", "fuzzy today weather intent"); rc) return rc;
  if (int rc = check(today_weather.value("slots", nlohmann::json::object()).value("city", "").empty(),
                      "fuzzy today weather empty city");
      rc)
    return rc;
  if (int rc = check(today_weather.value("confidence", 1.0) < intent_execute_threshold(),
                      "fuzzy today weather below execute");
      rc)
    return rc;

  auto sh_weather = classify_intent_fuzzy("上海天气怎么样", nullptr);
  if (int rc = check(sh_weather.value("intent", "") == "weather", "fuzzy shanghai weather intent"); rc) return rc;
  if (int rc = check(sh_weather.value("slots", nlohmann::json::object()).value("city", "") == "上海",
                      "fuzzy shanghai city slot");
      rc)
    return rc;

  // --- policy_text 支持 templates 数组变体 ---
  const std::string test_policy_path =
      "data/test_chat_policy_variants_" + std::to_string(::getpid()) + ".json";
  std::filesystem::create_directories("data");
  {
    std::ofstream out(test_policy_path);
    out << R"POLICY({
  "templates": {
    "chat.messaging_capability_clarify": [
      "[VARIANT_A] 你是想看会话消息、记忆检索，还是新闻查询？",
      "[VARIANT_B] 你要查的是消息记录、记忆，还是新闻？"
    ]
  }
})POLICY";
  }
  ::setenv("THIN_AGENT_CHAT_POLICY_PATH", test_policy_path.c_str(), 1);

  std::set<std::string> seen;
  for (int i = 0; i < 30; ++i) {
    seen.insert(policy_text("chat.messaging_capability_clarify", ""));
  }
  if (int rc = check(seen.count("[VARIANT_A] 你是想看会话消息、记忆检索，还是新闻查询？") > 0,
                      "policy_text variants includes A");
      rc)
    return rc;
  if (int rc = check(seen.count("[VARIANT_B] 你要查的是消息记录、记忆，还是新闻？") > 0,
                      "policy_text variants includes B");
      rc)
    return rc;
  ::unsetenv("THIN_AGENT_CHAT_POLICY_PATH");
  thin_agent::chat_policy_reset();
  std::filesystem::remove(test_policy_path);

  // --- top_intent_scores 排序 ---
  const auto ranked = top_intent_scores("你有哪些能力", 3, nullptr);
  if (int rc = check(!ranked.empty(), "top_intent_scores non-empty"); rc) return rc;
  if (int rc = check(ranked[0].first == "profile", "top intent profile first"); rc) return rc;
  if (int rc = check(ranked[0].second >= intent_clarify_threshold(), "top profile score above clarify"); rc) return rc;

  // --- 多轮 dialog boost ---
  thin_agent::DialogContext ctx;
  ctx.last_intent = "weather";
  ctx.last_route = "local_external_weather";
  ctx.ttl_turns = 3;
  auto followup = classify_intent_fuzzy("深圳的呢", &ctx);
  if (int rc = check(followup.value("intent", "") == "weather", "dialog followup weather"); rc) return rc;
  if (int rc = check(followup.value("slots", nlohmann::json::object()).value("city", "") == "深圳",
                      "dialog followup city");
      rc)
    return rc;

  // --- 阶段2：media 意图族 + 上下文 status ---
  if (int rc = check(thin_agent::is_media_family_intent("media_capture_execute"), "media family execute"); rc)
    return rc;
  if (int rc = check(thin_agent::is_media_family_intent("media_capture_status"), "media family status"); rc)
    return rc;
  if (int rc = check(!thin_agent::is_media_family_intent("weather"), "weather not media family"); rc) return rc;

  auto capture_exec = classify_intent_fuzzy("拍张照片", nullptr);
  if (int rc = check(capture_exec.value("intent", "") == "media_capture_execute", "fuzzy capture execute"); rc)
    return rc;
  if (int rc = check(capture_exec.value("confidence", 0.0) >= intent_clarify_threshold(),
                      "fuzzy capture execute conf");
      rc)
    return rc;

  auto capture_q = classify_intent_fuzzy("拍照完成了吗", nullptr);
  if (int rc = check(capture_q.value("intent", "") == "media_capture_status",
                      "fuzzy 拍照完成了吗 -> status (not execute)");
      rc)
    return rc;

  thin_agent::DialogContext media_ctx;
  media_ctx.last_intent = "media_capture_execute";
  media_ctx.last_route = "local_task_inline";
  media_ctx.last_slots = {{"task_id", "task-2"}, {"action", "capture_photo"}};
  media_ctx.ttl_turns = 3;
  if (int rc = check(thin_agent::dialog_has_media_execute_context(&media_ctx), "dialog media context"); rc)
    return rc;
  auto short_status = classify_intent_fuzzy("完成了吗", &media_ctx);
  if (int rc = check(short_status.value("intent", "") == "media_capture_status",
                      "fuzzy 完成了吗 with dialog -> status");
      rc)
    return rc;
  if (int rc = check(short_status.value("confidence", 0.0) >= intent_execute_threshold(),
                      "fuzzy short status execute-level conf with dialog");
      rc)
    return rc;

  auto compound = classify_intent_fuzzy("给我拍3张照片，再拍一段20秒的视频", nullptr);
  if (int rc = check(compound.value("intent", "") == "media_plan" ||
                          compound.value("intent", "") == "media_capture_execute",
                      "fuzzy compound media_plan or capture");
      rc)
    return rc;

  // --- 阶段3：intent_policy 表驱动门控 ---
  {
    using thin_agent::evaluate_intent_gate;
    using thin_agent::intent_has_side_effect;
    using thin_agent::intent_policy_names;
    using thin_agent::load_intent_gate_policy;

    if (int rc = check(intent_has_side_effect("action_capture"), "action_capture side_effect"); rc) return rc;
    if (int rc = check(intent_has_side_effect("task_capture_inline"), "task_capture side_effect"); rc)
      return rc;
    if (int rc = check(!intent_has_side_effect("media_capture_status"), "status not side_effect"); rc)
      return rc;

    thin_agent::IntentGatePolicy pol;
    if (int rc = check(load_intent_gate_policy("action_capture", &pol) && pol.ok, "load action_capture policy");
        rc)
      return rc;
    if (int rc = check(pol.requires_dialogue_act == "command", "action_capture requires command"); rc)
      return rc;

    auto allow = evaluate_intent_gate("action_capture", "command", false);
    if (int rc = check(allow.allow_execute, "command allows capture"); rc) return rc;

    auto block = evaluate_intent_gate("action_capture", "question", false);
    if (int rc = check(!block.allow_execute, "question blocks capture"); rc) return rc;
    if (int rc = check(block.action == "clarify", "mismatch on_mismatch=clarify"); rc) return rc;

    const auto names = intent_policy_names();
    if (int rc = check(names.size() >= 4, "intent_policy has media entries"); rc) return rc;
  }

  // ── Tier 2 基础设施测试 ──
  // 写临时 policy 文件（含 intent_classifier + intent_map），路径带 PID 防冲突
  const std::string t2_policy_path =
      "data/test_chat_policy_tier2_" + std::to_string(::getpid()) + ".json";
  std::filesystem::create_directories("data");
  {
    std::ofstream out(t2_policy_path);
    out << R"JSON({
  "intent_classifier": {
    "model": "test-flash",
    "prompt_zh": "你是一个意图分类器。分析用户输入，只返回JSON。",
    "prompt_en": "You are an intent classifier. Return JSON only."
  },
  "intent_map": {
    "profile": {"has_local_handler": true},
    "status": {"has_local_handler": true},
    "weather": {"has_local_handler": true},
    "news": {"has_local_handler": true},
    "event": {"has_local_handler": true},
    "general_query": {"has_local_handler": true},
    "complex": {"has_local_handler": false},
    "external": {"has_local_handler": false},
    "unknown": {"has_local_handler": false}
  },
  "nlp": {
    "chat_abbreviations": {"u":"you","r":"are","ur":"your","plz":"please"}
  }
})JSON";
  }
  ::setenv("THIN_AGENT_CHAT_POLICY_PATH", t2_policy_path.c_str(), 1);

  // intent_classifier_prompt: 中英文均非空
  {
    std::string zh_prompt = thin_agent::intent_classifier_prompt("zh");
    if (int rc = check(!zh_prompt.empty(), "intent_classifier_prompt zh non-empty"); rc) return rc;
    if (int rc = check(zh_prompt.find("意图分类器") != std::string::npos, "intent_classifier_prompt zh has 意图分类器"); rc) return rc;

    std::string en_prompt = thin_agent::intent_classifier_prompt("en");
    if (int rc = check(!en_prompt.empty(), "intent_classifier_prompt en non-empty"); rc) return rc;
    if (int rc = check(en_prompt.find("intent classifier") != std::string::npos, "intent_classifier_prompt en has intent classifier"); rc) return rc;

    // 未知语言 fallback 到中文
    std::string xx_prompt = thin_agent::intent_classifier_prompt("fr");
    if (int rc = check(!xx_prompt.empty(), "intent_classifier_prompt fr fallback non-empty"); rc) return rc;
    if (int rc = check(xx_prompt.find("intent classifier") != std::string::npos || xx_prompt.find("意图分类器") != std::string::npos, "intent_classifier_prompt fr has prompt"); rc) return rc;
  }

  // intent_has_local_handler: 有 handler 的返回 true，否则 false
  {
    if (int rc = check(thin_agent::intent_has_local_handler("profile"), "profile has local handler"); rc) return rc;
    if (int rc = check(thin_agent::intent_has_local_handler("status"), "status has local handler"); rc) return rc;
    if (int rc = check(thin_agent::intent_has_local_handler("weather"), "weather has local handler"); rc) return rc;
    if (int rc = check(thin_agent::intent_has_local_handler("news"), "news has local handler"); rc) return rc;
    if (int rc = check(thin_agent::intent_has_local_handler("event"), "event has local handler"); rc) return rc;
    if (int rc = check(thin_agent::intent_has_local_handler("general_query"), "general_query has local handler"); rc) return rc;

    if (int rc = check(!thin_agent::intent_has_local_handler("complex"), "complex no local handler"); rc) return rc;
    if (int rc = check(!thin_agent::intent_has_local_handler("external"), "external no local handler"); rc) return rc;
    if (int rc = check(!thin_agent::intent_has_local_handler("unknown"), "unknown no local handler"); rc) return rc;
    if (int rc = check(!thin_agent::intent_has_local_handler("nonexistent"), "nonexistent no local handler"); rc) return rc;
  }

  // 缩写展开回归：normalize_text_for_policy 正确展开 u/r
  {
    std::string n1 = thin_agent::normalize_text_for_policy("what r u");
    if (int rc = check(n1.find("you") != std::string::npos, "normalize 'what r u' has you"); rc) return rc;
    if (int rc = check(n1.find("are") != std::string::npos, "normalize 'what r u' has are"); rc) return rc;

    std::string n2 = thin_agent::normalize_text_for_policy("who r u");
    if (int rc = check(n2.find("you") != std::string::npos, "normalize 'who r u' has you"); rc) return rc;

    // 不应误展开嵌入词
    std::string n3 = thin_agent::normalize_text_for_policy("unique");
    if (int rc = check(n3.find("you") == std::string::npos, "normalize 'unique' stays unique"); rc) return rc;

    std::string n4 = thin_agent::normalize_text_for_policy("please help plz");
    if (int rc = check(n4.find("please help please") != std::string::npos, "normalize 'plz' -> 'please'"); rc) return rc;

    // 恢复真实 policy（Tier2 测试设置了 THIN_AGENT_CHAT_POLICY_PATH）
    ::unsetenv("THIN_AGENT_CHAT_POLICY_PATH");
    thin_agent::chat_policy_reset();

    // ── search 关键词应命中 ──
    auto sw = thin_agent::policy_keywords("search");
    if (int rc = check(!sw.empty(), "search keywords non-empty"); rc) return rc;
    bool found = false;
    for (const auto& kw : sw) {
      if (std::string("搜索一下").find(kw) != std::string::npos) { found = true; break; }
    }
    if (int rc = check(found, "search keyword match"); rc) return rc;

    // ── classify_intent_fuzzy 应可处理搜索类输入（不崩溃，返回合理意图）──
    auto sf = classify_intent_fuzzy("搜索一下 golang 新特性", nullptr);
    if (int rc = check(!sf.value("intent", "").empty(), "fuzzy search not empty"); rc) return rc;
  }

  std::filesystem::remove(t2_policy_path);

  std::cout << "unit:test_intent_scorer PASS\n";
  return 0;
}
