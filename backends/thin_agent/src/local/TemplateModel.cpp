#include "thin_agent/local/TemplateModel.h"

#include <algorithm>
#include <cctype>

#include "thin_agent/core/ChatPolicy.h"

namespace thin_agent {
namespace local {

TemplateModel::TemplateModel() {}

bool TemplateModel::load() {
  load_templates();
  loaded_ = true;
  return true;
}

void TemplateModel::load_templates() {
  // 从 chat_policy.json 加载 offline_general 模板
  // 格式约定：chat_policy.offline_general.rules = [{keywords, reply}, ...]
  try {
    const auto& policy = chat_policy();

    if (policy.contains("offline_general")) {
      const auto& og = policy["offline_general"];

      // 兜底回复
      if (og.contains("unknown") && og["unknown"].is_string()) {
        fallback_reply_ = og["unknown"].get<std::string>();
      }

      // 规则列表
      if (og.contains("rules") && og["rules"].is_array()) {
        for (const auto& r : og["rules"]) {
          if (!r.is_object()) continue;
          TemplateRule rule;
          if (r.contains("keywords") && r["keywords"].is_array()) {
            for (const auto& kw : r["keywords"]) {
              if (kw.is_string()) rule.keywords.push_back(kw.get<std::string>());
            }
          }
          if (r.contains("reply") && r["reply"].is_string()) {
            rule.reply = r["reply"].get<std::string>();
          }
          if (!rule.keywords.empty() && !rule.reply.empty()) {
            rules_.push_back(std::move(rule));
          }
        }
      }
    }
  } catch (...) {
    // chat_policy 不存在时使用硬编码兜底
  }

  // 硬编码兜底（确保至少有一个回复）
  if (fallback_reply_.empty()) {
    fallback_reply_ = "Offline mode. Cannot process this request. Try: status, weather, memory, help.";
  }

  // 硬编码基础规则（确保常见请求有回复）
  if (rules_.empty()) {
    rules_ = {
        {{"ni hao", "hi", "hello"},
         "Hello! I am running in offline mode. Try: status, weather, memory, help."},
        {{"help", "?", "bang zhu", "gong neng"},
         "Offline features: status, weather [city], recent memory, history memory, capture photo, recording."},
        {{"thanks", "xie xie", "thx"},
         "You are welcome!"},
        {{"bye", "zai jian"},
         "Goodbye!"},
    };
  }
}

std::string TemplateModel::match_keyword(const std::string& input_lower) const {
  for (const auto& rule : rules_) {
    for (const auto& kw : rule.keywords) {
      if (input_lower.find(kw) != std::string::npos) {
        return rule.reply;
      }
    }
  }
  return "";
}

std::string TemplateModel::infer(const std::string& input,
                                  int /*max_tokens*/,
                                  const std::string& /*capability*/) {
  if (!loaded_) load();

  // 转小写
  std::string lower;
  lower.reserve(input.size());
  for (unsigned char c : input) {
    lower.push_back(static_cast<char>(std::tolower(c)));
  }

  auto reply = match_keyword(lower);
  if (!reply.empty()) return reply;

  // 无关键词命中 → 返回兜底
  return fallback_reply_;
}

}  // namespace local
}  // namespace thin_agent
