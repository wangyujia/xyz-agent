#pragma once

#include <chrono>
#include <mutex>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {
namespace agent {

/// 追踪 Span：记录一次操作（LLM 调用 / 工具调用 / Agent 轮次）。
struct TraceSpan {
  std::string span_id;       ///< 唯一标识
  std::string parent_id;     ///< 父 span ID（顶层为空）
  std::string name;          ///< 操作名称（"llm_call", "tool_call", "agent_turn"）
  std::string status;        ///< "ok" / "error" / "timeout"
  int64_t start_us;          ///< 开始时间（epoch microseconds）
  int64_t end_us;            ///< 结束时间
  int64_t duration_us;       ///< 耗时

  // LLM 专用
  int input_tokens = 0;
  int output_tokens = 0;
  std::string model_name;

  // 工具专用
  std::string tool_name;
  nlohmann::json tool_params;
  nlohmann::json tool_result;

  // 序列化
  nlohmann::json to_json() const;
};

/// Agent 追踪器：记录完整推理过程的 span tree。
class AgentTracer {
 public:
  static AgentTracer& instance();

  /// 开始追踪一个 session。
  void begin_session(const std::string& session_id);

  /// 开始一个 span。返回 span_id。
  std::string start_span(const std::string& name,
                         const std::string& parent_id = "");

  /// 结束一个 span。
  void end_span(const std::string& span_id, const std::string& status = "ok");

  /// 记录 LLM 调用信息（token 用量等）。
  void record_llm(const std::string& span_id,
                  int input_tokens, int output_tokens,
                  const std::string& model = "");

  /// 记录工具调用信息。
  void record_tool(const std::string& span_id,
                   const std::string& tool_name,
                   const nlohmann::json& params,
                   const nlohmann::json& result);

  /// 获取当前 session 的完整 trace。
  nlohmann::json get_trace() const;

  /// 获取摘要统计。
  nlohmann::json summary() const;

  /// 清空当前 session 的 trace。
  void clear();

  /// ── 持久化 ──

  /// 保存当前 trace 到 SQLite（自动建表）。
  /// @param db_path  SQLite 文件路径（默认 traces.db）
  bool save_to_db(const std::string& db_path = "traces.db");

  /// 列出最近 N 条 trace 的摘要。
  static nlohmann::json list_recent(int limit = 20,
                                    const std::string& db_path = "traces.db");

 private:
  AgentTracer() = default;
  std::string session_id_;
  std::vector<TraceSpan> spans_;
  std::mutex mu_;  ///< v0.53.42: 全局单例多 worker 并发——spans_ 访问互斥
  int next_id_ = 1;

  int64_t now_us() const;
};

}  // namespace agent
}  // namespace thin_agent
