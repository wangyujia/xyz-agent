#pragma once
/// v0.52.28: 网关会话映射持久化 — chat_id(:thread_id) → session_id 落盘。
///
/// 背景：ensure_chat_session 的映射在内存（g_chat_sessions），服务重启后
/// 同 chat_id 分配新 sid → 跨重启 resume（v0.52.26 journal 按 sid 查）
/// 找不到旧会话的 run——断点续跑最后一公里。本类把映射落
/// data/gateway_sessions.db，重启加载，新映射增量写。

#include <sqlite3.h>

#include <string>
#include <unordered_map>

namespace thin_agent {

class GatewaySessionMap {
public:
  ~GatewaySessionMap() {
    if (db_) sqlite3_close(db_);
  }

  /// 打开（不存在则创建）+ 建表 + 全量加载到内存。失败返回 false（调用方
  /// 退化为纯内存模式，行为与旧版一致）。
  bool open(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) return false;
    char* e = nullptr;
    if (sqlite3_exec(db_,
            "CREATE TABLE IF NOT EXISTS gw_chat_sessions ("
            " map_key TEXT PRIMARY KEY, sid TEXT NOT NULL);",
            nullptr, nullptr, &e) != SQLITE_OK) {
      if (e) sqlite3_free(e);
      return false;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT map_key, sid FROM gw_chat_sessions;",
                           -1, &st, nullptr) != SQLITE_OK)
      return false;
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char* k = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
      const char* v = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
      if (k && v) cache_[k] = v;
    }
    sqlite3_finalize(st);
    return true;
  }

  /// 查映射（内存缓存优先）。空串=无。
  std::string lookup(const std::string& map_key) const {
    auto it = cache_.find(map_key);
    return it == cache_.end() ? std::string() : it->second;
  }

  /// 写映射（内存+落盘）。失败只影响持久性，内存仍生效。
  void assign(const std::string& map_key, const std::string& sid) {
    cache_[map_key] = sid;
    if (!db_) return;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT OR REPLACE INTO gw_chat_sessions(map_key, sid)"
            " VALUES(?,?);",
            -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, map_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, sid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }

  /// 缓存条数（测试用）。
  size_t size() const { return cache_.size(); }

private:
  sqlite3* db_{nullptr};
  std::unordered_map<std::string, std::string> cache_;
};

}  // namespace thin_agent
