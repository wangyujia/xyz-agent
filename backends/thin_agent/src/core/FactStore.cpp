#include "thin_agent/core/FactStore.h"
#include <mutex>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include "thin_agent/log/LogEvent.h"

namespace thin_agent {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
// 内部实现
// ─────────────────────────────────────────────────────────────

struct FactStore::Impl {
  std::mutex mu_;  // v0.53.42: sqlite 单连接非线程安全

    sqlite3* db = nullptr;

    static constexpr int kMaxContentChars = 1000;
    static constexpr int kDefaultHotCharLimit = 4000;

    /// 执行 SQL（无结果）。
    bool exec(const std::string& sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK && err) {
            log_event("FactStore", LogLevel::Error, "sql error",
                      {{"error", std::string(err)}});
            sqlite3_free(err);
        }
        return rc == SQLITE_OK;
    }

    /// 执行 SQL + 获取 rows。（不检查 sqlite3 返回值——调用者检查）
    bool query(const std::string& sql,
               std::function<void(sqlite3_stmt*)> row_cb) {
        sqlite3_stmt* stmt = nullptr;
        // 兼容旧版 sqlite3_prepare（v2 需要 5 个参数）
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            log_event("FactStore", LogLevel::Error, "prepare failed",
                      {{"error", sqlite3_errmsg(db)}});
            return false;
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            row_cb(stmt);
        }
        sqlite3_finalize(stmt);
        return true;
    }

    bool ensure_schema() {
        // 主表
        exec(R"(
            CREATE TABLE IF NOT EXISTS memory_blocks (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                path TEXT NOT NULL UNIQUE,
                content TEXT NOT NULL,
                created_at TEXT DEFAULT (datetime('now')),
                updated_at TEXT DEFAULT (datetime('now'))
            );
        )");

        // FTS5 虚拟表（如果已存在则跳过）
        exec(R"(
            CREATE VIRTUAL TABLE IF NOT EXISTS memory_fts USING fts5(
                content, content='memory_blocks', content_rowid='id'
            );
        )");

        // 触发器（幂等）
        exec(R"(
            CREATE TRIGGER IF NOT EXISTS mem_ai AFTER INSERT ON memory_blocks BEGIN
                INSERT INTO memory_fts(rowid, content) VALUES (new.id, new.content);
            END;
        )");
        exec(R"(
            CREATE TRIGGER IF NOT EXISTS mem_ad AFTER DELETE ON memory_blocks BEGIN
                INSERT INTO memory_fts(memory_fts, rowid, content) VALUES ('delete', old.id, old.content);
            END;
        )");
        exec(R"(
            CREATE TRIGGER IF NOT EXISTS mem_au AFTER UPDATE ON memory_blocks BEGIN
                INSERT INTO memory_fts(memory_fts, rowid, content) VALUES ('delete', old.id, old.content);
                INSERT INTO memory_fts(rowid, content) VALUES (new.id, new.content);
            END;
        )");

        return true;
    }

    std::string safe_str(sqlite3_stmt* stmt, int col) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
        return text ? std::string(text) : std::string();
    }
};

// ─────────────────────────────────────────────────────────────
// 生命周期
// ─────────────────────────────────────────────────────────────

FactStore::FactStore() = default;

FactStore::~FactStore() {
    if (impl_ && impl_->db) {
        sqlite3_close(impl_->db);
    }
}

bool FactStore::open(const std::string& db_path) {
    // v0.53.42: open 为单线程初始化路径,不加锁(锁在 impl_ 构造前会 null deref)
    impl_ = std::make_unique<Impl>();

    int rc = sqlite3_open(db_path.c_str(), &impl_->db);
    if (rc != SQLITE_OK) {
        log_event("FactStore", LogLevel::Error, "open failed",
                  {{"error", sqlite3_errmsg(impl_->db)}});
        // v0.53.74: 失败也须 close——sqlite3_open 失败时句柄已分配
        sqlite3_close(impl_->db);
        impl_->db = nullptr;
        return false;
    }

    impl_->exec("PRAGMA journal_mode=WAL;");
    impl_->exec("PRAGMA foreign_keys=ON;");
    impl_->ensure_schema();

    // 自动导入：如果表为空且 MD 文件存在
    bool has_rows = false;
    impl_->query("SELECT COUNT(*) FROM memory_blocks;",
                 [&](sqlite3_stmt* stmt) { has_rows = sqlite3_column_int64(stmt, 0) > 0; });
    if (!has_rows) {
        fs::path md_dir = fs::path(db_path).parent_path() / "memories";
        if (fs::exists(md_dir / "MEMORY.md") || fs::exists(md_dir / "USER.md")) {
            import_from_md(md_dir.string());
        }
    }

    return true;
}

// ─────────────────────────────────────────────────────────────
// CRUD
// ─────────────────────────────────────────────────────────────

int64_t FactStore::save(const std::string& path, const std::string& content) {
  std::lock_guard<std::mutex> lk(impl_->mu_);  // v0.53.42: 单连接互斥(4 worker 并发)
    if (!impl_ || !impl_->db) return -1;
    if (path.empty() || content.empty()) return -1;

    // 内容长度检查
    std::string clean = content;
    if (clean.size() > static_cast<size_t>(Impl::kMaxContentChars)) {
        clean.resize(Impl::kMaxContentChars);
    }

    // 安全校验钩子
    if (write_validator_) {
        std::string reason;
        if (!write_validator_(clean, reason)) {
            log_event("FactStore", LogLevel::Warn, "write rejected",
                      {{"reason", reason}});
            return -1;
        }
    }

    // UPSERT: path 已存在 → UPDATE；不存在 → INSERT
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO memory_blocks(path, content, updated_at) "
        "VALUES(?, ?, datetime('now')) "
        "ON CONFLICT(path) DO UPDATE SET content=excluded.content, updated_at=excluded.updated_at;";

    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        log_event("FactStore", LogLevel::Error, "save prepare",
                  {{"error", sqlite3_errmsg(impl_->db)}});
        return -1;
    }
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, clean.c_str(), -1, SQLITE_TRANSIENT);

    int64_t id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(impl_->db);
    }
    sqlite3_finalize(stmt);
    return id;
}

bool FactStore::forget(int64_t id) {
  std::lock_guard<std::mutex> lk(impl_->mu_);  // v0.53.42: 单连接互斥(4 worker 并发)
    if (!impl_ || !impl_->db || id <= 0) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
                           "DELETE FROM memory_blocks WHERE id=?;",
                           -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, id);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok && sqlite3_changes(impl_->db) > 0;
}

// ─────────────────────────────────────────────────────────────
// 热路径：冷冻快照
// ─────────────────────────────────────────────────────────────

std::string FactStore::hot_snapshot(const std::string& role_name, int char_limit) {
    if (!impl_ || !impl_->db) return "";

    std::string sql =
        "SELECT path, content FROM memory_blocks WHERE "
        "(path LIKE 'system/%' OR path LIKE 'user/%'";
    if (!role_name.empty()) {
        sql += " OR path LIKE ?";
    }
    sql += ") ORDER BY updated_at DESC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return "";
    }
    if (!role_name.empty()) {
        std::string pattern = "role/" + role_name + "/%";
        sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<std::pair<std::string, std::string>> entries;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        entries.emplace_back(impl_->safe_str(stmt, 0), impl_->safe_str(stmt, 1));
    }
    sqlite3_finalize(stmt);

    if (entries.empty()) return "";

    const std::string sep =
        "\n══════════════════════════════════════════════\n";
    std::string header = sep +
        "MEMORY (persisted facts, auto-injected every turn) [" +
        std::to_string(entries.size()) + " entries]" + sep;

    std::string body;
    int budget = char_limit - static_cast<int>(header.size());
    if (budget < 50) budget = 50;  // 至少留 50 chars

    int count = 0;
    for (const auto& [path, content] : entries) {
        std::string line = "- [" + path + "] " + content + "\n";
        if (static_cast<int>(body.size() + line.size()) > budget) {
            body += "\n(" + std::to_string(entries.size() - count) +
                    " more entries — use memory_find to search)\n";
            break;
        }
        body += line;
        count++;
    }

    return header + body + "\nEND MEMORY\n";
}

// ─────────────────────────────────────────────────────────────
// 冷路径：FTS5 搜索
// ─────────────────────────────────────────────────────────────

std::vector<Fact> FactStore::find(const std::string& query,
                                   const std::string& prefix,
                                   int limit) {
    std::vector<Fact> results;
    if (!impl_ || !impl_->db) return results;
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;

    if (query.empty()) {
        // 无搜索词 → 列表模式
        std::string sql = "SELECT id, path, content, created_at, updated_at FROM memory_blocks";
        if (!prefix.empty()) {
            sql += " WHERE path LIKE ?";
        }
        sql += " ORDER BY updated_at DESC LIMIT ?;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return results;
        }
        int idx = 1;
        if (!prefix.empty()) {
            sqlite3_bind_text(stmt, idx++, (prefix + "%").c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int(stmt, idx, limit);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Fact f;
            f.id = sqlite3_column_int64(stmt, 0);
            f.path = impl_->safe_str(stmt, 1);
            f.content = impl_->safe_str(stmt, 2);
            f.created_at = impl_->safe_str(stmt, 3);
            f.updated_at = impl_->safe_str(stmt, 4);
            results.push_back(f);
        }
        sqlite3_finalize(stmt);
    } else {
        // FTS5 搜索
        std::string sql =
            "SELECT m.id, m.path, m.content, m.created_at, m.updated_at "
            "FROM memory_blocks m "
            "JOIN memory_fts fts ON m.id = fts.rowid "
            "WHERE memory_fts MATCH ?";
        if (!prefix.empty()) {
            sql += " AND m.path LIKE ?";
        }
        sql += " ORDER BY rank LIMIT ?;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return results;
        }
        int idx = 1;
        sqlite3_bind_text(stmt, idx++, query.c_str(), -1, SQLITE_TRANSIENT);
        if (!prefix.empty()) {
            sqlite3_bind_text(stmt, idx++, (prefix + "%").c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int(stmt, idx, limit);

        // v0.53.55: FTS5 语法防御(与 SessionStore GG1 同款)——裸绑
        /// 用户串含 C++/引号/NEAR 即语法错→静默空;step 错误时降级
        /// 整串短语查询(内部双引号翻倍转义)
        int step_rc = sqlite3_step(stmt);
        if (step_rc == SQLITE_ERROR || step_rc == SQLITE_MISUSE) {
            sqlite3_finalize(stmt);
            std::string phrase = "\"";
            for (char c : query) {
                if (c == '"') phrase += "\"\"";
                else phrase += c;
            }
            phrase += "\"";
            if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
                return results;
            }
            idx = 1;
            sqlite3_bind_text(stmt, idx++, phrase.c_str(), -1, SQLITE_TRANSIENT);
            if (!prefix.empty()) {
                sqlite3_bind_text(stmt, idx++, (prefix + "%").c_str(), -1, SQLITE_TRANSIENT);
            }
            sqlite3_bind_int(stmt, idx, limit);
            step_rc = sqlite3_step(stmt);
        }
        while (step_rc == SQLITE_ROW) {
            Fact f;
            f.id = sqlite3_column_int64(stmt, 0);
            f.path = impl_->safe_str(stmt, 1);
            f.content = impl_->safe_str(stmt, 2);
            f.created_at = impl_->safe_str(stmt, 3);
            f.updated_at = impl_->safe_str(stmt, 4);
            results.push_back(f);
            step_rc = sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }

    return results;
}

// ─────────────────────────────────────────────────────────────
// MD 备份（Hermes 兼容 § 分隔格式）
// ─────────────────────────────────────────────────────────────

bool FactStore::export_to_md(const std::string& dir) {
    if (!impl_ || !impl_->db) return false;

    fs::create_directories(dir);

    auto write_entries = [&](const std::string& filename) -> bool {
        std::vector<std::string> entries;
        std::string simple_sql;
        if (filename == "MEMORY.md") {
            simple_sql = "SELECT content FROM memory_blocks WHERE path LIKE 'system/%' OR path LIKE 'ref/%' ORDER BY updated_at;";
        } else {
            simple_sql = "SELECT content FROM memory_blocks WHERE path LIKE 'user/%' ORDER BY updated_at;";
        }
        impl_->query(simple_sql, [&](sqlite3_stmt* stmt) {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (t) entries.push_back(std::string(t));
        });

        std::ofstream out(fs::path(dir) / filename);
        if (!out.is_open()) return false;
        for (size_t i = 0; i < entries.size(); ++i) {
            out << entries[i];
            if (i + 1 < entries.size()) out << "\n§\n";
        }
        out << "\n";
        return !entries.empty() || true;  // empty file is still valid
    };

    return write_entries("MEMORY.md") &&
           write_entries("USER.md");
}

bool FactStore::import_from_md(const std::string& dir) {
    if (!impl_ || !impl_->db) return false;

    auto read_entries = [](const std::string& filepath) -> std::vector<std::string> {
        std::vector<std::string> entries;
        std::ifstream in(filepath);
        if (!in.is_open()) return entries;

        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        if (content.empty()) return entries;

        // 按 § 分隔
        size_t pos = 0;
        std::string delimiter = "\n§\n";
        while (pos < content.size()) {
            size_t next = content.find(delimiter, pos);
            std::string entry;
            if (next == std::string::npos) {
                entry = content.substr(pos);
                pos = content.size();
            } else {
                entry = content.substr(pos, next - pos);
                pos = next + delimiter.size();
            }
            // trim trailing whitespace
            while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r')) {
                entry.pop_back();
            }
            if (!entry.empty()) entries.push_back(entry);
        }
        return entries;
    };

    // 自动推断 path 前缀
    auto import_file = [&](const std::string& filename, const std::string& default_prefix) {
        auto entries = read_entries((fs::path(dir) / filename).string());
        for (size_t i = 0; i < entries.size(); ++i) {
            // path = prefix + 序号，确保唯一
            std::string path = default_prefix + "_md_" + std::to_string(i + 1);
            save(path, entries[i]);
        }
    };

    import_file("MEMORY.md", "system/md_import");
    import_file("USER.md", "user/md_import");

    return true;
}

// ─────────────────────────────────────────────────────────────
// 统计
// ─────────────────────────────────────────────────────────────

nlohmann::json FactStore::stats() const {
    nlohmann::json s;
    if (!impl_ || !impl_->db) return s;

    impl_->query("SELECT COUNT(*) FROM memory_blocks;",
                 [&](sqlite3_stmt* stmt) {
                     s["total_entries"] = sqlite3_column_int64(stmt, 0);
                 });

    impl_->query(
        "SELECT COUNT(*) FROM memory_blocks WHERE path LIKE 'system/%' OR path LIKE 'user/%';",
        [&](sqlite3_stmt* stmt) {
            s["hot_entries"] = sqlite3_column_int64(stmt, 0);
        });

    impl_->query(
        "SELECT COUNT(*) FROM memory_blocks WHERE path LIKE 'ref/%';",
        [&](sqlite3_stmt* stmt) {
            s["cold_entries"] = sqlite3_column_int64(stmt, 0);
        });

    return s;
}

}  // namespace thin_agent
