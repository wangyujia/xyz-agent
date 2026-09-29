#include "thin_agent/agent/VectorStore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <sqlite3.h>

#include "thin_agent/agent/EmbeddingProvider.h"

namespace thin_agent {
namespace agent {

// ────────────────────────────────────────────────────────────────
// SQLite 辅助
// ────────────────────────────────────────────────────────────────

namespace {

void check_sqlite(int rc, sqlite3* db, const char* step) {
  if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW) return;
  std::string msg = std::string(step) + ": " + sqlite3_errmsg(db);
  throw std::runtime_error(msg);
}

}  // namespace

// ────────────────────────────────────────────────────────────────
// 嵌入向量序列化
// ────────────────────────────────────────────────────────────────

std::string VectorStore::serialize_embedding(const std::vector<float>& vec) {
  std::string blob;
  blob.resize(vec.size() * sizeof(float));
  std::memcpy(blob.data(), vec.data(), blob.size());
  return blob;
}

std::vector<float> VectorStore::deserialize_embedding(const void* blob,
                                                       int len, int dim) {
  std::vector<float> vec(dim, 0.0f);
  if (!blob || len <= 0 || dim <= 0) return vec;
  int count = std::min(len / static_cast<int>(sizeof(float)), dim);
  std::memcpy(vec.data(), blob, count * sizeof(float));
  return vec;
}

// ────────────────────────────────────────────────────────────────
// VectorStore 实现
// ────────────────────────────────────────────────────────────────

VectorStore::VectorStore() = default;

VectorStore::~VectorStore() {
  if (db_) {
    sqlite3_close(static_cast<sqlite3*>(db_));
    db_ = nullptr;
  }
}

std::string VectorStore::now_iso() const {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto tt = system_clock::to_time_t(now);
  std::tm tm = *std::gmtime(&tt);
  std::ostringstream os;
  os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return os.str();
}

bool VectorStore::init(const std::string& db_path, int dim) {
  if (dim < 1) return false;
  dim_ = dim;

  // 创建父目录
  std::filesystem::path p(db_path);
  if (p.has_parent_path()) {
    std::filesystem::create_directories(p.parent_path());
  }

  sqlite3* db = nullptr;
  int rc = sqlite3_open(db_path.c_str(), &db);
  if (rc != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return false;
  }
  db_ = db;

  // WAL 模式：允许并发读 + 单写，避免多测试进程锁冲突
  sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

  const char* sql = R"SQL(
CREATE TABLE IF NOT EXISTS memories (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  content TEXT NOT NULL,
  source TEXT NOT NULL DEFAULT 'conversation',
  session_id TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL,
  importance REAL NOT NULL DEFAULT 1.0,
  embedding BLOB NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_memories_source ON memories(source);
CREATE INDEX IF NOT EXISTS idx_memories_session ON memories(session_id);
CREATE INDEX IF NOT EXISTS idx_memories_created ON memories(created_at);
)SQL";

  char* err = nullptr;
  rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string msg = err ? err : "schema error";
    if (err) sqlite3_free(err);
    return false;
  }

  return true;
}

int64_t VectorStore::insert(const MemoryEntry& entry,
                             const std::vector<float>& embedding) {
  std::lock_guard<std::mutex> lk(mu_);  // v0.53.41: 单连接互斥
  if (static_cast<int>(embedding.size()) != dim_) return -1;

  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;

  const char* sql = R"SQL(
INSERT INTO memories(content, source, session_id, created_at, importance, embedding)
VALUES(?, ?, ?, ?, ?, ?)
)SQL";

  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db,
               "prepare insert");

  std::string ts = entry.created_at.empty() ? now_iso() : entry.created_at;
  sqlite3_bind_text(stmt, 1, entry.content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, entry.source.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, entry.session_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, ts.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt, 5, static_cast<double>(entry.importance));

  std::string blob = serialize_embedding(embedding);
  sqlite3_bind_blob(stmt, 6, blob.data(), static_cast<int>(blob.size()),
                    SQLITE_TRANSIENT);

  check_sqlite(sqlite3_step(stmt), db, "step insert");
  sqlite3_finalize(stmt);

  const int64_t rowid = static_cast<int64_t>(sqlite3_last_insert_rowid(db));
  // v0.53.41: 容量治理——无上限长跑数万行,search 全表 cosine 变慢。
  // 超 kMaxMemories 时删最老的且 importance 最低的(排序:newest+重要优先)。
  static constexpr int64_t kMaxMemories = 5000;
  sqlite3_stmt* pr = nullptr;
  if (sqlite3_prepare_v2(db,
        "DELETE FROM memories WHERE id IN ("
        " SELECT id FROM memories ORDER BY importance DESC, id DESC"
        " LIMIT -1 OFFSET ?)", -1, &pr, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(pr, 1, kMaxMemories);
    sqlite3_step(pr);
    sqlite3_finalize(pr);
  }
  return rowid;
}

std::vector<SearchResult> VectorStore::search(
    const std::vector<float>& query_embedding, int top_k,
    float min_similarity) {
  std::lock_guard<std::mutex> lk(mu_);  // v0.53.41: 单连接互斥
  std::vector<SearchResult> results;
  if (static_cast<int>(query_embedding.size()) != dim_) return results;
  if (top_k < 1) top_k = 5;

  std::vector<float> q_norm = query_embedding;
  normalize_l2(q_norm);

  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;

  const char* sql =
      "SELECT id, content, source, session_id, created_at, importance, embedding "
      "FROM memories ORDER BY id DESC";

  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db,
               "prepare search");

  // 全量扫描 + 计算相似度
  struct Candidate {
    SearchResult sr;
    float score;
  };
  std::vector<Candidate> candidates;

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MemoryEntry entry;
    entry.id = sqlite3_column_int64(stmt, 0);
    entry.content =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    entry.source =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    entry.session_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    entry.created_at =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    entry.importance =
        static_cast<float>(sqlite3_column_double(stmt, 5));

    const void* emb_blob = sqlite3_column_blob(stmt, 6);
    int emb_len = sqlite3_column_bytes(stmt, 6);
    auto emb_vec = deserialize_embedding(emb_blob, emb_len, dim_);
    normalize_l2(emb_vec);

    float sim = cosine_similarity(q_norm, emb_vec);
    if (sim >= min_similarity) {
      candidates.push_back({{entry, sim}, sim});
    }
  }

  sqlite3_finalize(stmt);

  // 排序（相似度降序）
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              return a.score > b.score;
            });

  // 取 top_k
  for (int i = 0;
       i < top_k && i < static_cast<int>(candidates.size()); ++i) {
    results.push_back(candidates[i].sr);
  }

  return results;
}

bool VectorStore::remove(int64_t id) {
  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;

  const char* sql = "DELETE FROM memories WHERE id=?";
  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db,
               "prepare remove");
  sqlite3_bind_int64(stmt, 1, id);
  check_sqlite(sqlite3_step(stmt), db, "step remove");
  int changes = sqlite3_changes(db);
  sqlite3_finalize(stmt);
  return changes > 0;
}

int VectorStore::remove_by_session(const std::string& session_id) {
  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;

  const char* sql = "DELETE FROM memories WHERE session_id=?";
  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db,
               "prepare remove_by_session");
  sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  check_sqlite(sqlite3_step(stmt), db, "step remove_by_session");
  int changes = sqlite3_changes(db);
  sqlite3_finalize(stmt);
  return changes;
}

int64_t VectorStore::count() const {
  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;
  check_sqlite(
      sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM memories", -1, &stmt,
                         nullptr),
      db, "prepare count");
  int64_t n = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    n = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return n;
}

std::vector<MemoryEntry> VectorStore::recent(int limit) {
  std::vector<MemoryEntry> out;
  if (limit < 1) limit = 20;

  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;

  const char* sql =
      "SELECT id, content, source, session_id, created_at, importance "
      "FROM memories ORDER BY id DESC LIMIT ?";
  check_sqlite(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), db,
               "prepare recent");
  sqlite3_bind_int(stmt, 1, limit);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MemoryEntry e;
    e.id = sqlite3_column_int64(stmt, 0);
    e.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    e.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    e.session_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    e.created_at =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    e.importance = static_cast<float>(sqlite3_column_double(stmt, 5));
    out.push_back(e);
  }
  sqlite3_finalize(stmt);
  return out;
}

nlohmann::json VectorStore::stats() const {
  nlohmann::json s;
  s["total"] = count();

  auto* db = static_cast<sqlite3*>(db_);
  sqlite3_stmt* stmt = nullptr;

  check_sqlite(sqlite3_prepare_v2(
                   db,
                   "SELECT source, COUNT(*) FROM memories GROUP BY source",
                   -1, &stmt, nullptr),
               db, "prepare stats");

  nlohmann::json by_source = nlohmann::json::object();
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    by_source[reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))] =
        sqlite3_column_int64(stmt, 1);
  }
  sqlite3_finalize(stmt);
  s["by_source"] = by_source;
  return s;
}

}  // namespace agent
}  // namespace thin_agent
