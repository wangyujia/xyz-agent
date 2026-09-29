#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {

/// 一条会话消息。
struct SessionMessage {
    int64_t id = 0;
    std::string session_id;
    std::string role;       // user / assistant / tool
    std::string content;    // ≤2000 chars
    std::string tool_name;  // 工具名（仅 tool 角色）
    std::string created_at;
};

/// 会话摘要（用于列表展示）。
struct SessionSummary {
    std::string session_id;
    std::string title;       // 首条用户消息截取
    int message_count = 0;
    std::string created_at;
    std::string updated_at;
};

/// 跨会话搜索存储：SQLite + FTS5。
///
/// 每次 handle_chat 结束时自动持久化 user/assistant/tool 消息。
/// LLM 通过 session_search（FTS5 全文搜索）和 session_recent（最近消息）访问。
class SessionStore {
public:
    SessionStore();
    ~SessionStore();

    /// 打开数据库（不存在则创建），自动建表 + FTS5 + 触发器。
    bool open(const std::string& db_path);

    /// 记录一条消息。同时更新 session_index 的 message_count + updated_at。
    bool record_message(const std::string& session_id,
                        const std::string& role,
                        const std::string& content,
                        const std::string& tool_name = "");

    /// 更新会话标题（取首条用户消息）。
    void touch_session(const std::string& session_id,
                       const std::string& title);

    // ── 查询 ────────────────────────────────────────────────

    /// FTS5 全文搜索。返回匹配消息 + 所属会话信息。
    /// @param query  搜索关键词
    /// @param limit  最大返回条数
    std::vector<SessionMessage> search(const std::string& query, int limit = 10);

    /// 最近 N 条消息（跨所有会话，按时间倒序）。
    std::vector<SessionMessage> recent(int limit = 20);

    // ── 统计 ────────────────────────────────────────────────
    nlohmann::json stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace thin_agent
