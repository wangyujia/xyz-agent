#pragma once

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {

/// 多轮对话上下文快照，供模糊意图打分加权使用。
struct DialogContext {
  std::string last_intent;                 ///< 上一轮意图
  std::string last_route;                  ///< 上一轮路由
  nlohmann::json last_slots = nlohmann::json::object();  ///< 上一轮槽位
  int ttl_turns{0};                        ///< 上下文剩余有效轮数
};

/// 基于 chat_policy 锚点与关键词的加权模糊意图分类。
/// 返回 JSON：intent、confidence、backend、slots，可选 scores。
nlohmann::json classify_intent_fuzzy(const std::string& text,
                                     const DialogContext* dialog = nullptr);

/// 返回 Top-N 意图及其分数，用于云策略 payload 与调试。
std::vector<std::pair<std::string, double>> top_intent_scores(const std::string& text,
                                                              int top_n = 3,
                                                              const DialogContext* dialog = nullptr);

/// 意图直接执行阈值（来自 chat_policy.intent_thresholds.execute）。
double intent_execute_threshold();

/// 意图澄清阈值（来自 chat_policy.intent_thresholds.clarify）。
double intent_clarify_threshold();

/// 判断是否为主体介绍类文本（结合 profile 关键词与 detail_markers）。
bool is_profile_like_text(const std::string& text);

/// 是否为媒体意图族（media_capture_* / media_recording_* / media_plan）。
bool is_media_family_intent(const std::string& intent);

/// 上一轮对话是否处于媒体执行上下文（供 status 续问加权）。
bool dialog_has_media_execute_context(const DialogContext* dialog);

}  // namespace thin_agent
