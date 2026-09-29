// unit_media_plan：话轮分类、连拍/时长槽位、复合计划解析。

#include <iostream>

#include "thin_agent/core/MediaPlan.h"
#include "thin_agent/core/IntentScorer.h"

namespace {

int expect(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    return 1;
  }
  return 0;
}

}  // namespace

int main() {
  using thin_agent::DialogueAct;
  using thin_agent::DialogContext;
  using thin_agent::classify_dialogue_act;
  using thin_agent::media_plan_needs_pipeline;
  using thin_agent::parse_media_plan;
  using thin_agent::should_block_side_effect_for_act;

  if (expect(classify_dialogue_act("拍张照片") == DialogueAct::Command, "拍张照片=command")) return 1;
  if (expect(classify_dialogue_act("拍照完成了吗") == DialogueAct::Question, "拍照完成了吗=question")) return 1;
  if (expect(should_block_side_effect_for_act(DialogueAct::Question), "question blocks side effect")) return 1;

  {
    const auto p = parse_media_plan("拍照完成了吗", nullptr);
    if (expect(p.status_query, "status_query without dialog still for 拍照完成了吗")) return 1;
    if (expect(p.steps.empty(), "status query has no execute steps")) return 1;
  }

  {
    DialogContext ctx;
    ctx.last_intent = "task_capture";
    ctx.last_route = "local_task_inline";
    ctx.last_slots = {{"task_id", "task-2"}, {"action", "capture_photo"}};
    ctx.ttl_turns = 3;
    const auto p = parse_media_plan("完成了吗", &ctx);
    if (expect(p.status_query, "完成了吗 with dialog = status_query")) return 1;
  }

  {
    const auto p = parse_media_plan("拍5张照片", nullptr);
    if (expect(p.dialogue_act == DialogueAct::Command, "拍5张=command")) return 1;
    if (expect(p.steps.size() == 1, "拍5张 one step")) return 1;
    if (expect(p.steps[0].action == "capture_photo", "拍5张 action")) return 1;
    if (expect(p.steps[0].params.value("count", 0) == 5, "拍5张 count=5")) return 1;
    if (expect(media_plan_needs_pipeline(p), "拍5张 needs pipeline")) return 1;
  }

  {
    const auto p = parse_media_plan("给我拍3张照片，再拍一段20秒的视频", nullptr);
    if (expect(p.dialogue_act == DialogueAct::Command, "复合句=command")) return 1;
    if (expect(p.steps.size() == 3, "复合句 3 steps (capture+start+stop)")) return 1;
    if (expect(p.steps[0].action == "capture_photo", "step0 capture")) return 1;
    if (expect(p.steps[0].params.value("count", 0) == 3, "step0 count=3")) return 1;
    if (expect(p.steps[1].action == "start_recording", "step1 start")) return 1;
    if (expect(p.steps[1].params.value("duration_sec", 0) == 20, "step1 duration=20")) return 1;
    if (expect(p.steps[2].action == "stop_recording", "step2 stop")) return 1;
    if (expect(media_plan_needs_pipeline(p), "复合句 needs pipeline")) return 1;
  }

  {
    const auto p = parse_media_plan("能拍照吗", nullptr);
    if (expect(p.capability_question, "能拍照吗=capability")) return 1;
    if (expect(p.steps.empty(), "capability no steps")) return 1;
  }

  {
    const auto p1 = parse_media_plan("can u take photo?", nullptr);
    if (expect(p1.capability_question, "can u take photo?=capability")) return 1;
    if (expect(!p1.status_query, "can u take photo? not status")) return 1;
    const auto p2 = parse_media_plan("can you take photos?", nullptr);
    if (expect(p2.capability_question, "can you take photos?=capability")) return 1;
  }

  {
    const auto p = parse_media_plan("拍张照片", nullptr);
    if (expect(!media_plan_needs_pipeline(p), "单张不需要 pipeline")) return 1;
  }

  // 阶段2：fuzzy refine 短追问
  {
    DialogContext ctx;
    ctx.last_intent = "media_capture_execute";
    ctx.last_route = "local_task_pipeline";
    ctx.last_slots = {{"task_id", "task-9"}, {"action", "capture_photo"}};
    ctx.ttl_turns = 3;
    auto plan = parse_media_plan("完成了吗", &ctx);
    const auto fuzzy = thin_agent::classify_intent_fuzzy("完成了吗", &ctx);
    thin_agent::refine_media_plan_with_fuzzy(plan, fuzzy, &ctx);
    if (expect(plan.status_query, "refine 完成了吗 -> status_query")) return 1;
    if (expect(plan.fuzzy_intent == "media_capture_status", "refine fuzzy_intent status")) return 1;
    if (expect(plan.steps.empty(), "refine status no steps")) return 1;
  }

  // 阶段4：歧义检测 + Router refine
  {
    if (expect(thin_agent::is_ambiguous_media_utterance("要不要拍照", nullptr), "要不要拍照 ambiguous"))
      return 1;
    if (expect(thin_agent::is_ambiguous_media_utterance("拍照还是录像", nullptr), "拍照还是录像 ambiguous"))
      return 1;
    if (expect(!thin_agent::is_ambiguous_media_utterance("拍张照片", nullptr), "拍张照片 not ambiguous"))
      return 1;
    if (expect(!thin_agent::is_ambiguous_media_utterance("拍照完成了吗", nullptr),
               "拍照完成了吗 not ambiguous (status)"))
      return 1;

    auto soft = parse_media_plan("要不要拍照", nullptr);
    if (expect(soft.dialogue_act == DialogueAct::Question, "要不要拍照=question")) return 1;
    if (expect(soft.steps.empty() || soft.parse_reason == "question_no_execute",
               "要不要拍照 no execute steps"))
      return 1;

    thin_agent::MediaPlan routed;
    routed = parse_media_plan("要不要拍照", nullptr);
    nlohmann::json router = {
        {"ok", true},
        {"intent", "media_capture_execute"},
        {"confidence", 0.91},
        {"dialogue_act", "command"},
        {"slots", nlohmann::json::object()},
    };
    thin_agent::refine_media_plan_with_router(routed, router, nullptr);
    if (expect(routed.dialogue_act == DialogueAct::Command, "router sets command")) return 1;
    if (expect(!routed.status_query, "router execute not status")) return 1;
    if (expect(!routed.steps.empty() && routed.steps[0].action == "capture_photo",
               "router adds capture step"))
      return 1;

    thin_agent::MediaPlan status_routed = parse_media_plan("要不要拍照", nullptr);
    nlohmann::json router_status = {
        {"ok", true},
        {"intent", "media_capture_status"},
        {"confidence", 0.88},
        {"dialogue_act", "question"},
    };
    thin_agent::refine_media_plan_with_router(status_routed, router_status, nullptr);
    if (expect(status_routed.status_query, "router status_query")) return 1;
    if (expect(status_routed.steps.empty(), "router status no steps")) return 1;
  }

  std::cout << "unit:test_media_plan PASS\n";
  return 0;
}
