#include "thin_agent/agent/Tools.h"

#include <sstream>

#include "thin_agent/core/AgentService.h"
#include "thin_agent/core/ActionExecutor.h"
#include "thin_agent/core/ExternalInfoClient.h"
#include "thin_agent/llm/DemoConfigCompat.h"

namespace thin_agent {
namespace agent {

namespace {

/// 将 ToolCallResult 格式化为 LLM 可读的 observation 文本。
std::string format_tool_result(const ToolCallResult& r) {
  std::ostringstream os;
  os << "[工具: " << r.tool_name << "] ";
  if (r.ok) {
    os << "成功\n结果: " << r.result.dump();
  } else {
    os << "失败\n错误: " << r.error;
  }
  return os.str();
}

/// 从 JSON 安全提取 string 参数。
std::string get_str(const nlohmann::json& params, const std::string& key,
                    const std::string& def = "") {
  if (params.contains(key) && params[key].is_string())
    return params[key].get<std::string>();
  return def;
}

/// 从 JSON 安全提取 int 参数。
int get_int(const nlohmann::json& params, const std::string& key, int def = 0) {
  if (params.contains(key) && params[key].is_number_integer())
    return params[key].get<int>();
  return def;
}

}  // namespace

std::vector<ToolSchema> create_builtin_tools(const ToolContext& ctx) {
  std::vector<ToolSchema> tools;

  // ── 1. status ──────────────────────────────────────────────
  if (ctx.agent_service) {
    ToolSchema status_tool;
    status_tool.name = "status";
    status_tool.description =
        "查询 Agent 的运行状态，包括当前模式、使用的模型、provider、请求统计等信息。";
    status_tool.parameters = {};
    status_tool.execute = [svc = ctx.agent_service](const nlohmann::json&) {
      nlohmann::json result;
      try {
        auto st = svc->agent_tool_status();
        result["ok"] = true;
        result["result"] = st;
      } catch (const std::exception& e) {
        result["ok"] = false;
        result["error"] = std::string("status query failed: ") + e.what();
      }
      return result;
    };
    tools.push_back(std::move(status_tool));
  }

  // ── 2. weather ─────────────────────────────────────────────
  if (ctx.config) {
    ToolSchema weather_tool;
    weather_tool.name = "weather";
    weather_tool.description = "查询指定城市的天气信息，包括温度、湿度、天气状况等。";
    weather_tool.parameters = {
        {"city", "string", "城市名称，如 上海、北京、Shenzhen", true},
    };
    weather_tool.execute = [cfg = ctx.config, http = ctx.http](const nlohmann::json& params) {
      nlohmann::json result;
      std::string city = get_str(params, "city");
      if (city.empty()) {
        result["ok"] = false;
        result["error"] = "city parameter is required";
        return result;
      }
      auto fetch = ExternalInfoClient::fetch_weather(
          *http, city, "", cfg->external_provider, cfg->external_weather_url, "zh");
      result["ok"] = fetch.ok;
      result["result"] = fetch.data;
      if (!fetch.ok) result["error"] = fetch.error;
      return result;
    };
    tools.push_back(std::move(weather_tool));
  }

  // ── 3. news ─────────────────────────────────────────────────
  if (ctx.config) {
    ToolSchema news_tool;
    news_tool.name = "news";
    news_tool.description = "查询新闻头条，可按主题和日期范围过滤。";
    news_tool.parameters = {
        {"topic", "string", "新闻主题，如 科技、体育、AI", true},
        {"date_range", "string", "日期范围，如 today、week（可选）", false},
    };
    news_tool.execute = [cfg = ctx.config, http = ctx.http](const nlohmann::json& params) {
      nlohmann::json result;
      std::string topic = get_str(params, "topic");
      std::string date_range = get_str(params, "date_range", "today");
      if (topic.empty()) {
        result["ok"] = false;
        result["error"] = "topic parameter is required";
        return result;
      }
      auto fetch = ExternalInfoClient::fetch_news(*http, topic, date_range,
                                                   cfg->external_provider);
      result["ok"] = fetch.ok;
      result["result"] = fetch.data;
      if (!fetch.ok) result["error"] = fetch.error;
      return result;
    };
    tools.push_back(std::move(news_tool));
  }

  // ── 4. memory_recent ───────────────────────────────────────
  if (ctx.agent_service && !ctx.session_id.empty()) {
    ToolSchema mem_tool;
    mem_tool.name = "memory_recent";
    mem_tool.description = "获取当前会话的短期对话记忆（最近几轮对话内容）。";
    mem_tool.parameters = {};
    mem_tool.execute = [svc = ctx.agent_service,
                        sid = ctx.session_id](const nlohmann::json&) {
      nlohmann::json result;
      try {
        auto mr = svc->agent_tool_memory_recent(sid);
        result["ok"] = true;
        result["result"] = mr;
      } catch (const std::exception& e) {
        result["ok"] = false;
        result["error"] =
            std::string("memory_recent query failed: ") + e.what();
      }
      return result;
    };
    tools.push_back(std::move(mem_tool));
  }

  // ── 5. memory_search ───────────────────────────────────────
  if (ctx.agent_service) {
    ToolSchema mem_search_tool;
    mem_search_tool.name = "memory_search";
    mem_search_tool.description =
        "搜索长期记忆库，按关键词查找历史对话、任务记录等。";
    mem_search_tool.parameters = {
        {"query", "string", "搜索关键词", true},
        {"limit", "integer", "返回条数上限（默认 10）", false},
    };
    mem_search_tool.execute =
        [svc = ctx.agent_service](const nlohmann::json& params) {
          nlohmann::json result;
          std::string query = get_str(params, "query");
          int limit = get_int(params, "limit", 10);
          if (query.empty()) {
            result["ok"] = false;
            result["error"] = "query parameter is required";
            return result;
          }
          if (limit < 1) limit = 10;
          if (limit > 50) limit = 50;
          try {
            auto ms = svc->agent_tool_memory_search(query, limit);
            result["ok"] = true;
            result["result"] = ms;
          } catch (const std::exception& e) {
            result["ok"] = false;
            result["error"] =
                std::string("memory_search failed: ") + e.what();
          }
          return result;
        };
    tools.push_back(std::move(mem_search_tool));
  }

  // ── 6. capture_photo ───────────────────────────────────────
  if (ctx.action_executor) {
    ToolSchema capture_tool;
    capture_tool.name = "capture_photo";
    capture_tool.description = "拍摄一张照片。";
    capture_tool.dangerous = false;  // 拍照不危险
    capture_tool.parameters = {};
    capture_tool.execute = [exec = ctx.action_executor](const nlohmann::json&) {
      nlohmann::json result;
      Kv kv;
      auto r = exec->execute("capture_photo", kv, 10000);
      result["ok"] = (r.code == 0);
      result["result"]["code"] = r.code;
      result["result"]["message"] = r.message;
      if (r.code != 0) result["error"] = r.message;
      for (const auto& [k, v] : r.data) {
        result["result"][k] = v;
      }
      return result;
    };
    tools.push_back(std::move(capture_tool));
  }

  // ── 7. start_recording ─────────────────────────────────────
  if (ctx.action_executor) {
    ToolSchema rec_tool;
    rec_tool.name = "start_recording";
    rec_tool.description = "开始录像。";
    rec_tool.parameters = {
        {"duration_sec", "integer", "录像时长（秒），默认 10 秒", false},
    };
    rec_tool.execute = [exec = ctx.action_executor](const nlohmann::json& params) {
      nlohmann::json result;
      int dur = get_int(params, "duration_sec", 10);
      if (dur < 1) dur = 10;
      if (dur > 120) dur = 120;
      Kv kv;
      kv["duration_sec"] = std::to_string(dur);
      auto r = exec->execute("start_recording", kv, 15000);
      result["ok"] = (r.code == 0);
      result["result"]["code"] = r.code;
      result["result"]["message"] = r.message;
      result["result"]["duration_sec"] = dur;
      if (r.code != 0) result["error"] = r.message;
      return result;
    };
    tools.push_back(std::move(rec_tool));
  }

  // ── 8. stop_recording ──────────────────────────────────────
  if (ctx.action_executor) {
    ToolSchema stop_tool;
    stop_tool.name = "stop_recording";
    stop_tool.description = "停止当前录像。";
    stop_tool.parameters = {};
    stop_tool.execute = [exec = ctx.action_executor](const nlohmann::json&) {
      nlohmann::json result;
      Kv kv;
      auto r = exec->execute("stop_recording", kv, 10000);
      result["ok"] = (r.code == 0);
      result["result"]["code"] = r.code;
      result["result"]["message"] = r.message;
      if (r.code != 0) result["error"] = r.message;
      return result;
    };
    tools.push_back(std::move(stop_tool));
  }

  return tools;
}

}  // namespace agent
}  // namespace thin_agent
