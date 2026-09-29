#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "thin_agent/core/ChatPolicy.h"
#include "thin_agent/core/ExternalInfoClient.h"
#include "thin_agent/llm/DemoConfigCompat.h"

namespace thin_agent {

class IHttpClient;

/// 外部意图 fetch + 渲染的输入上下文。
struct ExternalHandlerContext {
  const DemoConfigCompat& cfg;
  const IntentSpec& spec;
  std::string text;          ///< 原始用户输入（翻译采样）
  std::string query_lang;
  std::string tpl_lang;
  std::string primary;       ///< 主槽位值（city / topic）
  nlohmann::json slots = nlohmann::json::object();
  /// 文本列表翻译：texts → 目标语；失败返回空向量
  using TranslateFn = std::function<std::vector<std::string>(
      const std::vector<std::string>& texts,
      const std::string& target_lang,
      const std::string& sample)>;
  TranslateFn translate;

  /// HTTP 客户端（注入，用于 weather/news HTTP 请求）。
  IHttpClient* http = nullptr;
};

/// 外部意图统一输出（成功 / fetch 失败 共用）。
struct ExternalHandlerOutput {
  bool fetch_ok{false};
  bool field_translated{false};
  std::string summary;
  ExternalInfoClient::FetchResult ext;
  nlohmann::json tool_call = nlohmann::json::object();
  /// post_execute=weather_advice 时的待确认槽；否则为空对象
  nlohmann::json advice_slots = nlohmann::json::object();
};

/// 按 IntentSpec.fetcher 分发：weather / news。未知 fetcher 返回 fetch_ok=false。
ExternalHandlerOutput run_external_intent_handler(const ExternalHandlerContext& ctx);

}  // namespace thin_agent
