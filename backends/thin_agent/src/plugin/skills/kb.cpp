/// libskill_kb — 知识库管理插件
///
/// 提供知识库的写操作能力，注册 cpp_handlers：
///   kb_index  — 构建/重建/增量更新索引
///   kb_status — 查看 KB 状态与统计
///   kb_add    — 手动添加文档到 KB
///
/// 核心 KbSearcher 负责读操作（搜索），插件负责写操作（索引/管理）。
/// 两者通过 WAL 模式共享同一个 codebase.db 文件，读写并发安全。

#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/core/KbSearcher.h"
#include "thin_agent/plugin/PluginContext.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace thin_agent {
namespace kb {

// ══════════════════════════════════════════════════════
// KB 路径
// ══════════════════════════════════════════════════════

std::string g_kb_dir;
std::string g_db_path;

std::string kb_dir() {
  if (!g_kb_dir.empty()) return g_kb_dir;
  const char* home = std::getenv("HOME");
  if (!home || !*home) home = ".";
  g_kb_dir = std::string(home) + "/.thin_agent/kb";
  return g_kb_dir;
}

std::string kb_db_path(const std::string& kb_name) {
  // 查 kb_index.json 注册表
  std::string idx_path = kb_dir() + "/kb_index.json";
  std::ifstream ifs(idx_path);
  if (ifs.good()) {
    try {
      auto j = json::parse(ifs);
      if (j.contains("kbs") && j["kbs"].contains(kb_name)) {
        auto& info = j["kbs"][kb_name];
        std::string p = info.value("path", "");
        if (!p.empty()) {
          if (p[0] == '/') return p;
          return kb_dir() + "/" + p;
        }
      }
    } catch (...) {
    }
  }
  // 回退
  return kb_dir() + "/" + kb_name + ".db";
}

// ══════════════════════════════════════════════════════
// KbIndexer — 文件扫描 + 符号提取 + FTS5 索引
// ══════════════════════════════════════════════════════

class KbIndexer {
 public:
  bool open(const std::string& db_path) {
    std::lock_guard<std::mutex> lk(mu_);
    // v0.52.7: 切库前关旧连接——跨 KB 调用（kb_status→kb_add）每次
    // open 泄漏一个 sqlite handle（长跑服务缓慢泄漏）
    if (db_) {
      if (db_path_ == db_path) return true;  // 同库复用
      sqlite3_close(db_);
      db_ = nullptr;
    }
    db_path_ = db_path;
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
      if (db_) { sqlite3_close(db_); db_ = nullptr; }
      return false;
    }
    // 启用 WAL 模式 —— 与核心 KbSearcher 读写并发
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    return true;
  }

  ~KbIndexer() { std::lock_guard<std::mutex> lk(mu_); if (db_) sqlite3_close(db_); }

  /// 全量重建索引
  json rebuild(const std::vector<std::string>& repos,
               const std::vector<std::string>& extensions) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return {{"success", false}, {"error", "DB not open"}};

    // DROP + CREATE
    char* err = nullptr;
    sqlite3_exec(db_, "DROP TABLE IF EXISTS codebase", nullptr, nullptr, &err);
    if (err) { std::string e(err); sqlite3_free(err); return {{"success", false}, {"error", e}}; }

    const char* create_sql = R"(
      CREATE VIRTUAL TABLE codebase USING fts5(
        repo, path, filename, extension, symbols, content,
        tokenize='porter unicode61'
      )
    )";
    sqlite3_exec(db_, create_sql, nullptr, nullptr, &err);
    if (err) { std::string e(err); sqlite3_free(err); return {{"success", false}, {"error", e}}; }

    // 扫描
    int count = 0;
    sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);  // v0.27.2: 显式事务
    for (const auto& repo : repos) {
      if (!fs::is_directory(repo)) continue;
      for (auto it = fs::recursive_directory_iterator(repo);
           it != fs::recursive_directory_iterator(); ++it) {
        const auto& p = it->path();
        // 跳过隐藏目录和构建产物
        if (p.filename().string()[0] == '.') { it.disable_recursion_pending(); continue; }
        if (fs::is_directory(p)) {
          std::string dn = p.filename().string();
          if (dn == "build" || dn == "node_modules" || dn == "__pycache__" || dn == ".git") {
            it.disable_recursion_pending();
            continue;
          }
          continue;
        }
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end())
          continue;
        // 读文件
        std::ifstream f(p.string());
        if (!f.good()) continue;
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        if (content.size() > 500000) continue;
        // 提取符号
        std::string syms = extract_symbols(content, ext);
        std::string relpath = fs::relative(p, repo).string();
        // 提取 repo 简称（路径最后一段）
        std::string repo_name = fs::path(repo).filename().string();
        // 插入 FTS5
        sqlite3_stmt* stmt = nullptr;
        const char* ins_sql = "INSERT INTO codebase (repo, path, filename, extension, symbols, content) VALUES (?,?,?,?,?,?)";
        int ins_rc = sqlite3_prepare_v2(db_, ins_sql, -1, &stmt, nullptr);
        if (ins_rc != SQLITE_OK) {
          if (stmt) sqlite3_finalize(stmt);
          // v0.53.42: 事务中途失败必须回滚——此前直接 return 挂着
          // 未提交事务,连接后续操作全部 database is locked
          sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
          return {{"success", false}, {"error", "INSERT prepare failed for: " + relpath}};
        }
        sqlite3_bind_text(stmt, 1, repo_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, relpath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, p.filename().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, ext.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, syms.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, content.c_str(), -1, SQLITE_TRANSIENT);
        int step_rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (step_rc != SQLITE_DONE) {
          sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);  // v0.53.42 同上
          return {{"success", false}, {"error", "INSERT step failed for: " + relpath}};
        }
        if (++count % 500 == 0) {
          sqlite3_exec(db_, "COMMIT; BEGIN TRANSACTION", nullptr, nullptr, nullptr);
        }
      }
    }
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    return {{"success", true}, {"files_indexed", count}};
  }

  /// 增量更新：只索引 mtime 晚于 last_indexed 的文件，同时检测已删除文件
  json incremental(const std::vector<std::string>& repos,
                   const std::vector<std::string>& extensions,
                   const std::string& last_indexed_iso) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return {{"success", false}, {"error", "DB not open"}};

    struct tm tm = {};
    time_t last_ts = 0;
    if (!last_indexed_iso.empty()) {
      strptime(last_indexed_iso.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm);
      last_ts = timegm(&tm);
    }

    int added = 0, removed = 0;
    std::unordered_set<std::string> seen_paths;

    sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);  // v0.27.2: 显式事务
    for (const auto& repo : repos) {
      if (!fs::is_directory(repo)) continue;
      std::string repo_name = fs::path(repo).filename().string();
      for (auto it = fs::recursive_directory_iterator(repo);
           it != fs::recursive_directory_iterator(); ++it) {
        const auto& p = it->path();
        // v0.53.55: 空文件名防御——filename() 可为空串(根/尾斜杠构造),
        /// [0] 越界=UB
        const std::string fname = p.filename().string();
        if (fname.empty() || fname[0] == '.') { it.disable_recursion_pending(); continue; }
        if (fs::is_directory(p)) {
          std::string dn = p.filename().string();
          if (dn == "build" || dn == "node_modules" || dn == "__pycache__" || dn == ".git") {
            it.disable_recursion_pending(); continue;
          }
          continue;
        }
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end()) continue;

        std::string relpath = fs::relative(p, repo).string();
        seen_paths.insert(repo_name + "::" + relpath);

        auto ftime = fs::last_write_time(p);
        // 转换为 time_t：C++17 兼容写法
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        time_t file_ts = std::chrono::system_clock::to_time_t(sctp);
        if (last_ts > 0 && file_ts <= last_ts) continue;

        std::ifstream f(p.string());
        if (!f.good()) continue;
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (content.size() > 500000) continue;

        // 删旧 + 插新
        sqlite3_stmt* del_stmt = nullptr;
        sqlite3_prepare_v2(db_,
          "DELETE FROM codebase WHERE rowid = (SELECT rowid FROM codebase WHERE path = ? AND filename = ? LIMIT 1)",
          -1, &del_stmt, nullptr);
        if (del_stmt) {
          sqlite3_bind_text(del_stmt, 1, relpath.c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_bind_text(del_stmt, 2, p.filename().c_str(), -1, SQLITE_TRANSIENT);
          sqlite3_step(del_stmt); sqlite3_finalize(del_stmt);
        }

        std::string syms = extract_symbols(content, ext);
        sqlite3_stmt* stmt = nullptr;
        int ins_rc = sqlite3_prepare_v2(db_, "INSERT INTO codebase (repo,path,filename,extension,symbols,content) VALUES (?,?,?,?,?,?)", -1, &stmt, nullptr);
        if (ins_rc != SQLITE_OK) {
          // 可能是旧 schema（无 repo 列），建议先全量重建
          if (stmt) sqlite3_finalize(stmt);
          return {{"success", false}, {"error", "Schema mismatch: DB lacks 'repo' column. Run kb_index(mode=\"full\") first to rebuild with new schema."}};
        }
        sqlite3_bind_text(stmt, 1, repo_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, relpath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, p.filename().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, ext.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, syms.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, content.c_str(), -1, SQLITE_TRANSIENT);
        int step_rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
        if (step_rc != SQLITE_DONE) {
          return {{"success", false}, {"error", "INSERT failed for: " + relpath}};
        }
        ++added;
        if (added % 500 == 0) sqlite3_exec(db_, "COMMIT; BEGIN TRANSACTION", nullptr, nullptr, nullptr);
      }
    }

    // 检测已删除文件
    if (last_ts > 0 && !seen_paths.empty()) {
      sqlite3_stmt* stmt = nullptr;
      int rc = sqlite3_prepare_v2(db_, "SELECT rowid, repo, path FROM codebase", -1, &stmt, nullptr);
      if (rc != SQLITE_OK) { if (stmt) sqlite3_finalize(stmt); }
      else if (stmt) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
          std::string key = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)) ?: "")
                          + "::" + std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)) ?: "");
          if (seen_paths.find(key) == seen_paths.end()) {
            char del_sql[64];
            snprintf(del_sql, sizeof(del_sql), "DELETE FROM codebase WHERE rowid = %lld", (long long)sqlite3_column_int64(stmt, 0));
            sqlite3_stmt* del = nullptr;
            sqlite3_prepare_v2(db_, del_sql, -1, &del, nullptr);
            if (del) { sqlite3_step(del); sqlite3_finalize(del); ++removed; }
          }
        }
        sqlite3_finalize(stmt);
      }
    }
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    return {{"success", true}, {"files_added", added}, {"files_removed", removed}};
  }

  /// 添加单篇文档
  json add_document(const std::string& path, const std::string& title,
                    const std::string& content, const std::string& tags) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return {{"success", false}, {"error", "DB not open"}};

    std::string ext = ".md";
    auto dot = path.rfind('.');
    if (dot != std::string::npos) ext = path.substr(dot);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO codebase (repo, path, filename, extension, symbols, content) VALUES (?,?,?,?,?,?)";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
      // v0.45.6: 区分旧 schema（无 repo 列）——给明确修复指引而非裸 "prepare failed"
      std::string msg = "prepare failed";
      sqlite3_stmt* probe = nullptr;
      if (sqlite3_prepare_v2(db_, "SELECT repo FROM codebase LIMIT 1", -1, &probe, nullptr) != SQLITE_OK) {
        msg = "Schema mismatch: DB lacks 'repo' column. Run kb_index(mode=\"full\") first to rebuild with new schema.";
      }
      if (probe) sqlite3_finalize(probe);
      return {{"success", false}, {"error", msg}};
    }

    sqlite3_bind_text(stmt, 1, "", -1, SQLITE_TRANSIENT);  // repo
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, ext.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, tags.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, content.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
      return {{"success", false}, {"error", "insert failed"}};
    }
    return {{"success", true}, {"path", path}, {"title", title}};
  }

  /// DB 状态
  json status() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return {{"error", "DB not open"}};
    auto result = json::object();
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM codebase", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
      if (stmt) sqlite3_finalize(stmt);
      return {{"error", "status query failed"}};
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      result["doc_count"] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return result;
  }

 private:
  static std::string extract_symbols(const std::string& content, const std::string& ext) {
    std::string syms;
    if (ext == ".c" || ext == ".cpp" || ext == ".h" || ext == ".hpp") {
      static const std::regex kFuncRe(R"((\w+)\s*\([^)]*\)\s*\{?)");
      static const std::regex kDefRe(R"((?:class|struct|enum)\s+(\w+)|#define\s+(\w+))");
      std::sregex_iterator end;

      for (std::sregex_iterator it(content.begin(), content.end(), kFuncRe); it != end; ++it) {
        std::string m = (*it)[1].str();
        if (m == "if" || m == "for" || m == "while" || m == "switch" || m == "return" ||
            m == "sizeof" || m == "void" || m == "int" || m == "char" || m == "bool" ||
            m == "float" || m == "double" || m == "long" || m == "short" || m == "unsigned" ||
            m == "signed" || m == "auto" || m == "const" || m == "static" || m == "extern" ||
            m == "inline" || m == "virtual") continue;
        if (syms.find(m) == std::string::npos) { if (!syms.empty()) syms += " "; syms += m; }
      }
      for (std::sregex_iterator it(content.begin(), content.end(), kDefRe); it != end; ++it) {
        std::string m = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
        if (syms.find(m) == std::string::npos) { if (!syms.empty()) syms += " "; syms += m; }
      }
    }
    if (ext == ".py") {
      static const std::regex kPyRe(R"((?:def|class)\s+(\w+))");
      std::sregex_iterator end;
      for (std::sregex_iterator it(content.begin(), content.end(), kPyRe); it != end; ++it) {
        std::string m = (*it)[1].str();
        if (syms.find(m) == std::string::npos) { if (!syms.empty()) syms += " "; syms += m; }
      }
    }
    return syms;
  }

  sqlite3* db_{nullptr};
  std::string db_path_;  ///< v0.52.7: 当前打开的库（同库复用不重开）
  mutable std::mutex mu_;
};

/// 全局 indexer（单例，插件生命期内复用）
KbIndexer* g_indexer = nullptr;

// ══════════════════════════════════════════════════════
// kb_index.json 管理
// ══════════════════════════════════════════════════════

json load_kb_index() {
  std::string path = kb_dir() + "/kb_index.json";
  std::ifstream f(path);
  if (!f.good()) return {{"kbs", json::object()}};
  try { return json::parse(f); } catch (...) { return {{"kbs", json::object()}}; }
}

bool save_kb_index(const json& j) {
  std::string path = kb_dir() + "/kb_index.json";
  std::ofstream f(path);
  if (!f.good()) return false;
  f << j.dump(2) << "\n";
  return true;
}

}  // namespace kb
}  // namespace thin_agent

// ══════════════════════════════════════════════════════
// 插件入口
// ══════════════════════════════════════════════════════

extern "C" const char* thin_agent_plugin_init(
    thin_agent::SkillRegistry& registry) {

  using thin_agent::kb::kb_dir;
  using thin_agent::kb::kb_db_path;
  using thin_agent::kb::load_kb_index;
  using thin_agent::kb::save_kb_index;
  using thin_agent::kb::g_indexer;
  using thin_agent::kb::KbIndexer;

  // 确保 kb 目录存在
  fs::create_directories(kb_dir());

  g_indexer = new KbIndexer();
  std::string codebase_path = kb_db_path("codebase");
  if (!g_indexer->open(codebase_path)) {
    // 如果 codebase.db 不存在，在首次 kb_index 时自动创建
  }

  // ── kb_index: 构建/更新索引 ──
  registry.register_cpp_handler("kb_index",
    [](const json& params) -> json {
      std::string kb_name = params.value("kb", "codebase");
      std::string mode = params.value("mode", "incremental");

      auto idx = load_kb_index();
      if (!idx.contains("kbs") || !idx["kbs"].contains(kb_name)) {
        return {{"success", false}, {"error", "KB not found: " + kb_name + ". Use kb_create to create it first."}};
      }

      auto& info = idx["kbs"][kb_name];
      if (info.value("type", "") != "repo_scan") {
        return {{"success", false}, {"error", "KB " + kb_name + " is manual (not repo_scan). Use kb_add to add documents."}};
      }

      // 获取 repo 列表和扩展名
      std::vector<std::string> repos;
      std::vector<std::string> exts;
      if (info.contains("repos")) {
        for (auto& r : info["repos"]) repos.push_back(r.get<std::string>());
      }
      if (info.contains("extensions")) {
        for (auto& e : info["extensions"]) exts.push_back(e.get<std::string>());
      }

      std::string db_path = kb_db_path(kb_name);
      if (!g_indexer->open(db_path)) {
        return {{"success", false}, {"error", "Cannot open KB: " + db_path}};
      }

      json result;
      if (mode == "full") {
        result = g_indexer->rebuild(repos, exts);
      } else {
        // v0.27.1: 增量更新 — mtime 检测 + 删除检测
        std::string last_ts = info.value("last_indexed", "");
        result = g_indexer->incremental(repos, exts, last_ts);
      }

      // 更新 kb_index.json
      if (result.value("success", false)) {
        time_t now = time(nullptr);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
        info["last_indexed"] = buf;
        // v0.27.2: 全量用 files_indexed，增量查询 DB 实际数量
        if (result.contains("files_indexed")) {
          info["doc_count"] = result["files_indexed"];
        } else {
          int added = result.value("files_added", 0);
          int removed = result.value("files_removed", 0);
          int old_count = info.value("doc_count", 0);
          info["doc_count"] = old_count + added - removed;
        }
        if (!idx.contains("kbs")) idx["kbs"] = json::object();
        idx["kbs"][kb_name] = info;
        save_kb_index(idx);
      }

      return result;
    });

  // ── kb_status: 查看 KB 状态 ──
  registry.register_cpp_handler("kb_status",
    [](const json& params) -> json {
      std::string kb_name = params.value("kb", "codebase");

      auto idx = load_kb_index();
      if (idx.contains("kbs") && idx["kbs"].contains(kb_name)) {
        auto info = idx["kbs"][kb_name];
        info["kb"] = kb_name;
        return info;
      }

      // 没有注册表 —— 直接查 DB
      std::string db_path = kb_db_path(kb_name);
      if (!g_indexer->open(db_path)) {
        return {{"kb", kb_name}, {"error", "KB not found or cannot be opened"}};
      }
      json status = g_indexer->status();
      status["kb"] = kb_name;
      status["path"] = db_path;
      return status;
    });

  // ── kb_add: 添加手动文档 ──
  // v0.53.0: kb_search 从核心迁入（KbSearcher 实例留核心——L5339 自动
  // KB 注入共用；经 ctx.config("kb").searcher 地址注入）。
  {
    int64_t p = 0;
    // ctx 由 init2 传入；init（旧签名）下 p=0 → not available
    extern thin_agent::PluginContext* thin_agent_kb_g_ctx;
    if (thin_agent_kb_g_ctx) {
      auto svc = thin_agent_kb_g_ctx->config("kb");
      if (svc.is_object()) p = svc.value("searcher", static_cast<int64_t>(0));
    }
    if (p) {
      auto* searcher = reinterpret_cast<thin_agent::KbSearcher*>(p);
      registry.register_cpp_handler("kb_search",
        [searcher, anchor = thin_agent_kb_g_ctx](const json& params) -> json {
          // v0.53.55: 存活锚(R35/R39 同款)——注入方析构后 searcher
          /// 悬垂;is_alive 注册表校验失效即拒
          if (!thin_agent::PluginContext::is_alive(anchor)) {
            return {{"success", false}, {"error", "kb searcher unavailable"}};
          }
          std::string kb_name = params.value("kb", "codebase");
          if (!searcher->open_by_name(kb_name))
            return {{"success", false}, {"error", "KB not found: " + kb_name}};
          std::string query = params.value("query", params.value("q", ""));
          int limit = params.value("limit", 10);
          return searcher->search(query, limit);
        });
    }
  }

  registry.register_cpp_handler("kb_add",
    [](const json& params) -> json {
      std::string kb_name = params.value("kb", "codebase");
      std::string title = params.value("title", "");
      std::string content = params.value("content", "");
      std::string tags = params.value("tags", "");
      std::string path = params.value("path", kb_name + "/" + title);

      if (title.empty()) return {{"success", false}, {"error", "title required"}};
      if (content.empty()) return {{"success", false}, {"error", "content required"}};

      std::string db_path = kb_db_path(kb_name);
      if (!g_indexer->open(db_path)) {
        // KB 不存在 → 自动创建
        fs::create_directories(kb_dir());
        // 创建 FTS5 表
        sqlite3* db = nullptr;
        sqlite3_open(db_path.c_str(), &db);
        if (!db) return {{"success", false}, {"error", "Cannot create KB"}};
        sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
        const char* create_sql = R"(
          CREATE VIRTUAL TABLE IF NOT EXISTS codebase USING fts5(
            repo, path, filename, extension, symbols, content,
            tokenize='porter unicode61'
          )
        )";
        char* err = nullptr;
        sqlite3_exec(db, create_sql, nullptr, nullptr, &err);
        if (err) {
          std::string e(err);
          sqlite3_free(err);
          sqlite3_close(db);
          return {{"success", false}, {"error", e}};
        }
        sqlite3_close(db);
        // 重新打开
        if (!g_indexer->open(db_path)) {
          return {{"success", false}, {"error", "Cannot open newly created KB"}};
        }
        // 注册
        auto idx = load_kb_index();
        if (!idx.contains("kbs")) idx["kbs"] = json::object();
        time_t now = time(nullptr);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
        idx["kbs"][kb_name] = {
          {"path", kb_name + ".db"},
          {"type", "manual"},
          {"created_at", buf},
          {"doc_count", 0}
        };
        save_kb_index(idx);
      }

      return g_indexer->add_document(path, title, content, tags);
    });

  return "kb";
}

// v0.53.0: v2 入口（context 注入）
thin_agent::PluginContext* thin_agent_kb_g_ctx = nullptr;
extern "C" const char* thin_agent_plugin_init2(
    thin_agent::SkillRegistry& registry, thin_agent::PluginContext& ctx) {
  thin_agent_kb_g_ctx = &ctx;
  return thin_agent_plugin_init(registry);
}
