#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "thin_agent/core/IntentScorer.h"

namespace thin_agent {

/// 从 dialog_state.last_slots 读取指定槽位（已做 null 安全处理）。
std::string recalled_slot_from_dialog(const DialogContext& dialog_ctx, const std::string& slot_key);

/// 上一轮是否为指定意图且 TTL 仍有效。
bool dialog_context_matches_intent(const DialogContext& dialog_ctx, const std::string& intent);

/// 是否应将 dialog 中的槽位补到当前空槽（指代句或延续上一轮同 intent）。
bool should_apply_dialog_slot(const std::string& intent,
                              const std::string& slot_key,
                              const std::string& text,
                              const DialogContext& dialog_ctx,
                              const std::string& current_value);

/// 询问「刚才/最后显示的是哪里的天气」类指代句。
bool is_weather_location_recall_query(const std::string& text);

/// 询问「刚才看的是什么新闻/什么方向」类指代句。
bool is_news_topic_recall_query(const std::string& text);

/// 指代/上下文追问标记（如何、的呢、刚才等）。
bool has_referential_context_markers(const std::string& text);

}  // namespace thin_agent
