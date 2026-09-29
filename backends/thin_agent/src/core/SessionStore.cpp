#include "thin_agent/core/SessionStore.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <iostream>

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include "thin_agent/log/LogEvent.h"

namespace thin_agent {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
// 内部实现
// ─────────────────────────────────────────────────────────────

struct SessionStore::Impl {
    sqlite3* db = nullptr;
    // v0.53.42: 单连接非线程安全——record_message 被 4 worker 并发调
    ///(VectorStore T1 同款)。读方法(search/recent)同样互斥。
    std::mutex mu;

    static constexpr int kMaxContentChars = 2000;

    bool exec(const std::string& sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK && err) {
            log_event("SessionStore", LogLevel::Error, "sql error",
                      {{"error", std::string(err)}});
            sqlite3_free(err);
        }
        return rc == SQLITE_OK;
    }

    std::string safe_str(sqlite3_stmt* stmt, int col) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
        return text ? std::string(text) : std::string();
    }

    bool ensure_schema() {
        exec(R"(
            CREATE TABLE IF NOT EXISTS session_index (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                session_id TEXT NOT NULL UNIQUE,
                title TEXT DEFAULT '',
                message_count INTEGER DEFAULT 0,
                created_at TEXT DEFAULT (datetime('now')),
                updated_at TEXT DEFAULT (datetime('now'))
            );
        )");
        exec(R"(
            CREATE TABLE IF NOT EXISTS session_messages (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                session_id TEXT NOT NULL,
                role TEXT NOT NULL,
                content TEXT NOT NULL,
                tool_name TEXT DEFAULT '',
                created_at TEXT DEFAULT (datetime('now'))
            );
        )");
        exec(R"(
            CREATE VIRTUAL TABLE IF NOT EXISTS session_fts USING fts5(
                content, content='session_messages', content_rowid='id'
            );
        )");
        exec(R"(
            CREATE TRIGGER IF NOT EXISTS sft_ai AFTER INSERT ON session_messages BEGIN
                INSERT INTO session_fts(rowid, content) VALUES (new.id, new.content);
            END;
        )");
        exec(R"(
            CREATE TRIGGER IF NOT EXISTS sft_ad AFTER DELETE ON session_messages BEGIN
                INSERT INTO session_fts(session_fts, rowid, content) VALUES ('delete', old.id, old.content);
            END;
        )");
        return true;
    }
};

// ─────────────────────────────────────────────────────────────
// 生命周期
// ─────────────────────────────────────────────────────────────

SessionStore::SessionStore() = default;

SessionStore::~SessionStore() {
    if (impl_ && impl_->db) {
        sqlite3_close(impl_->db);
    }
}

bool SessionStore::open(const std::string& db_path) {
    impl_ = std::make_unique<Impl>();

    int rc = sqlite3_open(db_path.c_str(), &impl_->db);
    if (rc != SQLITE_OK) {
        log_event("SessionStore", LogLevel::Error, "open failed",
                  {{"error", sqlite3_errmsg(impl_->db)}});
        // v0.53.74: 失败也须 close——sqlite3_open 失败时句柄已分配
        sqlite3_close(impl_->db);
        impl_->db = nullptr;
        return false;
    }

    impl_->exec("PRAGMA journal_mode=WAL;");
    impl_->ensure_schema();
    return true;
}

// ─────────────────────────────────────────────────────────────
// 写入
// ─────────────────────────────────────────────────────────────

bool SessionStore::record_message(const std::string& session_id,
                                   const std::string& role,
                                   const std::string& content,
                                   const std::string& tool_name) {
    std::lock_guard<std::mutex> lk(impl_->mu);  // v0.53.42: 单连接互斥
    if (!impl_ || !impl_->db) return false;
    if (session_id.empty() || content.empty()) return false;

    // 内容截断
    std::string clean = content;
    if (clean.size() > static_cast<size_t>(Impl::kMaxContentChars)) {
        clean.resize(Impl::kMaxContentChars);
    }

    // 插入消息
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO session_messages(session_id, role, content, tool_name) "
        "VALUES(?, ?, ?, ?);";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, clean.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, tool_name.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    if (!ok) return false;

    // 更新 session_index(v0.53.71: session_id 参数化——此前直拼)
    sqlite3_stmt* idx = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
            "INSERT INTO session_index(session_id, message_count, updated_at) "
            "VALUES(?1, 1, datetime('now')) "
            "ON CONFLICT(session_id) DO UPDATE SET "
            "message_count = message_count + 1, updated_at = datetime('now');",
            -1, &idx, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(idx, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(idx);
      sqlite3_finalize(idx);
    }

    return true;
}

void SessionStore::touch_session(const std::string& session_id,
                                  const std::string& title) {
    std::lock_guard<std::mutex> lk(impl_->mu);  // v0.53.42: 单连接互斥
    if (!impl_ || !impl_->db) return;
    // v0.53.71: 参数化——此前 title(=首条用户消息,LLM 对话原文完全
    /// 可控)/session_id 直拼 UPDATE,含单引号即语法错/语义注入
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
            "UPDATE session_index SET title = ?1 "
            "WHERE session_id = ?2 AND (title IS NULL OR title = '');",
            -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, title.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(st);
      sqlite3_finalize(st);
    }
}

// ─────────────────────────────────────────────────────────────
// 查询
// ─────────────────────────────────────────────────────────────

std::vector<SessionMessage> SessionStore::search(const std::string& query, int limit) {
    std::lock_guard<std::mutex> lk(impl_->mu);  // v0.53.42: 单连接互斥
    std::vector<SessionMessage> results;
    if (!impl_ || !impl_->db || query.empty()) return results;
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT m.id, m.session_id, m.role, m.content, m.tool_name, m.created_at "
        "FROM session_messages m "
        "JOIN session_fts fts ON m.id = fts.rowid "
        "WHERE session_fts MATCH ? "
        "ORDER BY rank LIMIT ?;";

    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }
    // v0.53.53: FTS5 MATCH 语法防御(双查询策略)——裸绑用户串时,
    /// 含 C++/引号/NEAR/连字符等触发 fts5 语法错误→step 报错→静默
    /// 空结果(搜"什么是C++"必空);纯短语包引号则子词搜不到。
    /// 正解:先按原样绑定(合法 FTS 语法=词查询语义,兼容旧版),
    /// step 返回 SQLITE_ERROR(语法错)时降级整串短语查询(内部
    /// 双引号翻倍转义)
    sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    int step_rc = sqlite3_step(stmt);
    if (step_rc == SQLITE_ERROR || step_rc == SQLITE_MISUSE) {
        // 语法错→降级短语查询
        sqlite3_finalize(stmt);
        std::string phrase = "\"";
        for (char c : query) {
            if (c == '"') phrase += "\"\"";
            else phrase += c;
        }
        phrase += "\"";
        if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return results;
        }
        sqlite3_bind_text(stmt, 1, phrase.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, limit);
        step_rc = sqlite3_step(stmt);
    }
    while (step_rc == SQLITE_ROW) {
        SessionMessage m;
        m.id = sqlite3_column_int64(stmt, 0);
        m.session_id = impl_->safe_str(stmt, 1);
        m.role = impl_->safe_str(stmt, 2);
        m.content = impl_->safe_str(stmt, 3);
        m.tool_name = impl_->safe_str(stmt, 4);
        m.created_at = impl_->safe_str(stmt, 5);
        results.push_back(m);
        step_rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<SessionMessage> SessionStore::recent(int limit) {
    std::lock_guard<std::mutex> lk(impl_->mu);  // v0.53.42: 单连接互斥
    std::vector<SessionMessage> results;
    if (!impl_ || !impl_->db) return results;
    if (limit < 1) limit = 1;
    if (limit > 200) limit = 200;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT id, session_id, role, content, tool_name, created_at "
        "FROM session_messages "
        "ORDER BY id DESC LIMIT ?;";

    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return results;
    }
    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SessionMessage m;
        m.id = sqlite3_column_int64(stmt, 0);
        m.session_id = impl_->safe_str(stmt, 1);
        m.role = impl_->safe_str(stmt, 2);
        m.content = impl_->safe_str(stmt, 3);
        m.tool_name = impl_->safe_str(stmt, 4);
        m.created_at = impl_->safe_str(stmt, 5);
        results.push_back(m);
    }
    sqlite3_finalize(stmt);
    return results;
}

// ─────────────────────────────────────────────────────────────
// 统计
// ─────────────────────────────────────────────────────────────

nlohmann::json SessionStore::stats() const {
    nlohmann::json s;
    if (!impl_ || !impl_->db) return s;

    int64_t count = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(impl_->db,
                               "SELECT COUNT(*) FROM session_messages;",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                count = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }
    s["total_messages"] = count;

    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(impl_->db,
                               "SELECT COUNT(*) FROM session_index;",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                s["total_sessions"] = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }

    return s;
}

}  // namespace thin_agent
