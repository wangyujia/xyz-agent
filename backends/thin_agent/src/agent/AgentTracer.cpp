#include "thin_agent/agent/AgentTracer.h"

#include <sqlite3.h>
#include <sstream>

namespace thin_agent {
namespace agent {

nlohmann::json TraceSpan::to_json() const {
  nlohmann::json j;
  j["span_id"] = span_id;
  j["parent_id"] = parent_id;
  j["name"] = name;
  j["status"] = status;
  j["start_us"] = start_us;
  j["end_us"] = end_us;
  j["duration_us"] = duration_us;

  if (!model_name.empty()) j["model"] = model_name;
  if (input_tokens > 0) j["input_tokens"] = input_tokens;
  if (output_tokens > 0) j["output_tokens"] = output_tokens;

  if (!tool_name.empty()) {
    j["tool"] = tool_name;
    if (!tool_params.is_null()) j["params"] = tool_params;
    if (!tool_result.is_null()) j["result"] = tool_result;
  }

  return j;
}

int64_t AgentTracer::now_us() const {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

AgentTracer& AgentTracer::instance() {
  static AgentTracer tracer;
  return tracer;
}

void AgentTracer::begin_session(const std::string& session_id) {
  // v0.53.42: 全局单例被多 worker 并发使用——spans_ 无锁 push_back 是 UB
  std::lock_guard<std::mutex> lk(mu_);
  session_id_ = session_id;
  spans_.clear();
  next_id_ = 1;
}

std::string AgentTracer::start_span(const std::string& name,
                                     const std::string& parent_id) {
  std::lock_guard<std::mutex> lk(mu_);
  TraceSpan span;
  span.span_id = "span-" + std::to_string(next_id_++);
  span.parent_id = parent_id;
  span.name = name;
  span.start_us = now_us();
  span.status = "running";
  spans_.push_back(span);
  return span.span_id;
}

void AgentTracer::end_span(const std::string& span_id,
                            const std::string& status) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& s : spans_) {
    if (s.span_id == span_id) {
      s.end_us = now_us();
      s.duration_us = s.end_us - s.start_us;
      s.status = status;
      return;
    }
  }
}

void AgentTracer::record_llm(const std::string& span_id,
                              int input_tokens, int output_tokens,
                              const std::string& model) {
  for (auto& s : spans_) {
    if (s.span_id == span_id) {
      s.input_tokens = input_tokens;
      s.output_tokens = output_tokens;
      if (!model.empty()) s.model_name = model;
      return;
    }
  }
}

void AgentTracer::record_tool(const std::string& span_id,
                               const std::string& tool_name,
                               const nlohmann::json& params,
                               const nlohmann::json& result) {
  for (auto& s : spans_) {
    if (s.span_id == span_id) {
      s.tool_name = tool_name;
      s.tool_params = params;
      s.tool_result = result;
      return;
    }
  }
}

nlohmann::json AgentTracer::get_trace() const {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& s : spans_) arr.push_back(s.to_json());
  return {{"session_id", session_id_}, {"spans", arr}};
}

nlohmann::json AgentTracer::summary() const {
  int total_llm = 0, total_tool = 0;
  int64_t total_input_tokens = 0, total_output_tokens = 0;
  int64_t total_latency_us = 0;
  int errors = 0;

  for (const auto& s : spans_) {
    if (s.name == "llm_call") {
      total_llm++;
      total_input_tokens += s.input_tokens;
      total_output_tokens += s.output_tokens;
    } else if (s.name == "tool_call") {
      total_tool++;
    }
    if (!s.parent_id.empty()) {
      // 叶子节点累计
    }
    if (s.status != "ok") errors++;
  }

  for (const auto& s : spans_) {
    if (s.parent_id.empty()) total_latency_us += s.duration_us;
  }

  return {
      {"session_id", session_id_},
      {"total_spans", spans_.size()},
      {"llm_calls", total_llm},
      {"tool_calls", total_tool},
      {"total_input_tokens", total_input_tokens},
      {"total_output_tokens", total_output_tokens},
      {"total_latency_ms", total_latency_us / 1000},
      {"errors", errors},
  };
}

void AgentTracer::clear() {
  spans_.clear();
  next_id_ = 1;
}

bool AgentTracer::save_to_db(const std::string& db_path) {
  sqlite3* db = nullptr;
  if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) return false;
  if (!db) return false;

  sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

  const char* create_sql =
      "CREATE TABLE IF NOT EXISTS traces ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  session_id TEXT NOT NULL,"
      "  trace_json TEXT NOT NULL,"
      "  summary_json TEXT NOT NULL,"
      "  num_spans INTEGER DEFAULT 0,"
      "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_traces_session ON traces(session_id);"
      "CREATE INDEX IF NOT EXISTS idx_traces_created ON traces(created_at DESC);";

  char* err = nullptr;
  if (sqlite3_exec(db, create_sql, nullptr, nullptr, &err) != SQLITE_OK) {
    sqlite3_free(err);
    sqlite3_close(db);
    return false;
  }

  std::string trace_str = get_trace().dump();
  std::string summary_str = summary().dump();
  int num = static_cast<int>(spans_.size());

  const char* insert_sql =
      "INSERT INTO traces (session_id, trace_json, summary_json, num_spans) "
      "VALUES (?, ?, ?, ?);";

  sqlite3_stmt* stmt = nullptr;
  bool ok = false;
  if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, session_id_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, trace_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, summary_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, num);
    ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
  }

  sqlite3_close(db);
  return ok;
}

nlohmann::json AgentTracer::list_recent(int limit, const std::string& db_path) {
  sqlite3* db = nullptr;
  if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) return nlohmann::json::array();
  if (!db) return nlohmann::json::array();

  const char* query =
      "SELECT session_id, summary_json, num_spans, created_at "
      "FROM traces ORDER BY id DESC LIMIT ?;";

  nlohmann::json results = nlohmann::json::array();
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      nlohmann::json row;
      row["session_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      row["created_at"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
      row["num_spans"] = sqlite3_column_int(stmt, 2);

      const char* summary_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
      if (summary_text) {
        try { row["summary"] = nlohmann::json::parse(summary_text); }
        catch (...) { row["summary"] = "parse_error"; }
      }
      results.push_back(row);
    }
    sqlite3_finalize(stmt);
  }

  sqlite3_close(db);
  return results;
}

}  // namespace agent
}  // namespace thin_agent
