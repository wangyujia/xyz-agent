#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace thin_agent {

class IHttpClient;

/// 外部信息查询客户端：天气与新闻。
/// 返回结果中的城市/状况文案会经 ChatPolicy 做本地化。
class ExternalInfoClient {
 public:
  /// 单次外部查询结果。
  struct FetchResult {
    bool ok = false;              ///< 是否成功拿到可用数据
    int http_status = 0;          ///< HTTP 状态码（真实请求时有效）
    std::string source = "mock";  ///< 数据源标识：mock / http 等
    std::string error;            ///< 失败时的错误码或描述
    nlohmann::json data = nlohmann::json::object();  ///< 结构化结果体
  };

  /// 查询指定城市与日期的天气；lang 控制返回文案语言（zh/en）。
  /// @param http  HTTP 客户端（注入，便于测试 mock）
  static FetchResult fetch_weather(IHttpClient& http,
                                    const std::string& city, const std::string& date,
                                    const std::string& external_provider = "mock",
                                    const std::string& external_weather_url = "",
                                    const std::string& lang = "zh");

  /// 查询指定主题与时间范围的新闻头条。
  /// @param http  HTTP 客户端（注入，便于测试 mock）
  static FetchResult fetch_news(IHttpClient& http,
                                 const std::string& topic, const std::string& time_range,
                                 const std::string& external_provider = "mock");

  /// 查询网页搜索（DuckDuckGo Instant Answer / mock 双模式）。
  /// @param http    HTTP 客户端（注入，便于测试 mock）
  /// @param query   搜索关键词
  /// @param lang    zh 或 en，影响 DDG region 参数
  /// @param provider  mock | duckduckgo（后续可扩展 brave / searxng）
  static FetchResult fetch_search(IHttpClient& http,
                                   const std::string& query,
                                   const std::string& lang = "zh",
                                   const std::string& provider = "duckduckgo");

  /// 将搜索结果的 data JSON 格式化为 LLM 可读文本。
  static std::string format_search_results(const nlohmann::json& data, const std::string& lang = "zh");
};

}  // namespace thin_agent
