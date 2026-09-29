#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "thin_agent/core/IntentScorer.h"

namespace thin_agent {

/// 话轮行为：指令 / 提问 / 其它。
enum class DialogueAct {
  Command,
  Question,
  Other,
};

/// 媒体相关计划步骤（拍照 / 录像启停）。
struct MediaPlanStep {
  std::string action;                       ///< capture_photo / start_recording / stop_recording
  nlohmann::json params = nlohmann::json::object();
};

/// 从自然语言解析出的媒体计划（含话轮类型与可选状态查询）。
struct MediaPlan {
  DialogueAct dialogue_act{DialogueAct::Other};
  bool media_related{false};                ///< 文本是否涉及拍照/录像语义
  bool status_query{false};                 ///< 是否在问上次结果/是否完成
  bool capability_question{false};          ///< 「能拍照吗」类能力询问（不执行）
  std::vector<MediaPlanStep> steps;         ///< 可执行步骤；空表示无需执行
  int capture_count{0};                     ///< 计划中拍照总张数（便于观测）
  int record_duration_sec{0};               ///< 计划中录像时长（秒）
  std::string parse_reason;                 ///< 解析说明（调试/trace）
  std::string fuzzy_intent;                 ///< 阶段2：模糊分类命中的 media.* 意图（可空）
  double fuzzy_confidence{0.0};             ///< 对应模糊置信度
};

/// 将 DialogueAct 转为稳定字符串。
std::string dialogue_act_name(DialogueAct act);

/// 轻量话轮分类：疑问 vs 指令（可复用于任意副作用动作门控）。
DialogueAct classify_dialogue_act(const std::string& text);

/// 解析媒体相关口语：连拍张数、录像时长、复合「再/然后」、结果追问。
/// @param dialog 上一轮上下文（用于「完成了吗」续接）；可为空。
MediaPlan parse_media_plan(const std::string& text, const DialogContext* dialog = nullptr);

/// 用 IntentScorer 的 media.* 模糊结果增强计划（短追问 / 歧义句）。
/// 例如上一轮刚拍照后说「完成了吗」→ 升为 status_query。
void refine_media_plan_with_fuzzy(MediaPlan& plan,
                                  const nlohmann::json& fuzzy_intent,
                                  const DialogContext* dialog);

/// 阶段4：媒体相关但本地规则/模糊不足以安全执行（需 LLM Router）。
/// 典型：动作词 + 疑问/犹豫/二选一，或指代不清。
bool is_ambiguous_media_utterance(const std::string& text, const DialogContext* dialog = nullptr);

/// 用云分类/Router JSON 增强媒体计划（dialogue_act / media.* / task_pipeline）。
void refine_media_plan_with_router(MediaPlan& plan,
                                   const nlohmann::json& router_json,
                                   const DialogContext* dialog);

/// 是否需要走多步 pipeline（多步、count>1、或带 duration）。
bool media_plan_needs_pipeline(const MediaPlan& plan);

/// 副作用动作在「提问」话轮下是否应拦截执行。
bool should_block_side_effect_for_act(DialogueAct act);

}  // namespace thin_agent
