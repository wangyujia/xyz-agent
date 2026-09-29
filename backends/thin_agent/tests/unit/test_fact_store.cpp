// unit_fact_store：FactStore 持久记忆存储全功能测试

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "thin_agent/core/FactStore.h"
#include "thin_agent/RuntimePaths.h"

namespace fs = std::filesystem;
using thin_agent::FactStore;

// ── 测试辅助 ──────────────────────────────────────────────

/// 清理测试数据库。
void cleanup_db(const std::string& path) {
    if (fs::exists(path)) fs::remove(path);
    // 清理 WAL/SHM
    if (fs::exists(path + "-wal")) fs::remove(path + "-wal");
    if (fs::exists(path + "-shm")) fs::remove(path + "-shm");
}

/// 基本断言宏。
#define ASSERT(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << std::endl; return 1; } \
} while (0)

// ── 测试用例 ──────────────────────────────────────────────

int test_save_and_find() {
    std::string db = "/tmp/test_fact_store_save.db";
    cleanup_db(db);

    FactStore store;
    ASSERT(store.open(db), "open");

    // save 3 条
    int64_t id1 = store.save("system/prefs", "用户偏好中文交流");
    int64_t id2 = store.save("user/style", "先编译后测试");
    int64_t id3 = store.save("ref/leaptic", "MC3302 重启根因");
    ASSERT(id1 > 0, "save system");
    ASSERT(id2 > 0, "save user");
    ASSERT(id3 > 0, "save ref");

    // find: 无 query → 列表全部
    auto all = store.find();
    ASSERT(all.size() >= 3, "find all");

    // find: 前缀过滤
    auto sys = store.find("", "system");
    ASSERT(sys.size() >= 1, "find system prefix");

    // find: FTS5 搜索
    auto hits = store.find("MC3302");
    ASSERT(hits.size() >= 1, "fts5 search MC3302");
    ASSERT(hits[0].path == "ref/leaptic", "fts5 path match");

    // forget
    ASSERT(store.forget(id1), "forget system");
    auto after = store.find();
    ASSERT(after.size() >= 2, "after forget count");

    cleanup_db(db);
    return 0;
}

int test_upsert() {
    std::string db = "/tmp/test_fact_store_upsert.db";
    cleanup_db(db);

    FactStore store;
    store.open(db);

    int64_t id1 = store.save("system/version", "v0.27.4");
    int64_t id2 = store.save("system/version", "v0.27.5");
    ASSERT(id1 > 0 && id2 > 0, "upsert ids");
    // UPSERT: 同 path → 应只有 1 条
    auto all = store.find("", "system");
    int count = 0;
    for (const auto& f : all) {
        if (f.path == "system/version") {
            count++;
            ASSERT(f.content.find("v0.27.5") != std::string::npos, "upsert content updated");
        }
    }
    ASSERT(count == 1, "upsert count=1");

    cleanup_db(db);
    return 0;
}

int test_hot_snapshot() {
    std::string db = "/tmp/test_fact_store_snap.db";
    cleanup_db(db);

    FactStore store;
    store.open(db);

    store.save("system/prefs", "偏好1");
    store.save("user/style", "偏好2");
    store.save("ref/cold", "冷存储不应出现");

    std::string snap = store.hot_snapshot("");
    ASSERT(snap.find("偏好1") != std::string::npos, "snap has system");
    ASSERT(snap.find("偏好2") != std::string::npos, "snap has user");
    ASSERT(snap.find("冷存储") == std::string::npos, "snap no cold");

    // role 注入
    store.save("role/developer/conv", "开发角色约定");
    snap = store.hot_snapshot("developer");
    ASSERT(snap.find("开发角色约定") != std::string::npos, "snap has role entry");
    // role 不匹配时不应出现
    snap = store.hot_snapshot("worker");
    ASSERT(snap.find("开发角色约定") == std::string::npos, "snap no role for worker");

    cleanup_db(db);
    return 0;
}

int test_snapshot_budget() {
    std::string db = "/tmp/test_fact_store_budget.db";
    cleanup_db(db);

    FactStore store;
    store.open(db);

    // 写入大量数据
    for (int i = 0; i < 50; ++i) {
        std::string content = "这是一条很长很长很长很长很长很长很长很长的记忆条目 #" + std::to_string(i);
        store.save("system/test_" + std::to_string(i), content);
    }

    // 仅 500 chars 预算
    std::string snap = store.hot_snapshot("", 500);
    ASSERT(snap.size() <= 650, "snap within budget (header + entries)");  // header ~100 + entries
    ASSERT(snap.find("more entries") != std::string::npos
           || snap.find("memory_find") != std::string::npos,
           "snap has overflow hint");

    cleanup_db(db);
    return 0;
}

int test_empty_snapshot() {
    std::string db = "/tmp/test_fact_store_empty.db";
    cleanup_db(db);

    FactStore store;
    store.open(db);
    std::string snap = store.hot_snapshot("");
    ASSERT(snap.empty(), "empty snapshot");

    cleanup_db(db);
    return 0;
}

int test_content_truncation() {
    std::string db = "/tmp/test_fact_store_trunc.db";
    cleanup_db(db);

    FactStore store;
    store.open(db);

    // 超长内容（>1000 chars）
    std::string long_content(1200, 'X');
    int64_t id = store.save("system/long", long_content);
    ASSERT(id > 0, "save long content");

    auto hits = store.find("", "system");
    ASSERT(hits.size() == 1, "find long");
    ASSERT(hits[0].content.size() <= 1050, "truncated content");  // ~1000 + margin

    cleanup_db(db);
    return 0;
}

int test_export_import() {
    std::string db = "/tmp/test_fact_store_export.db";
    std::string md_dir = "/tmp/test_memories";
    cleanup_db(db);
    fs::remove_all(md_dir);

    FactStore store;
    store.open(db);

    store.save("system/prefs", "测试偏好");
    store.save("user/style", "用户风格");
    store.save("ref/something", "冷数据");

    // 导出
    ASSERT(store.export_to_md(md_dir), "export");
    ASSERT(fs::exists(md_dir + "/MEMORY.md"), "MEMORY.md exists");
    ASSERT(fs::exists(md_dir + "/USER.md"), "USER.md exists");

    // 验证内容
    {
        std::ifstream mem(md_dir + "/MEMORY.md");
        std::string mem_content((std::istreambuf_iterator<char>(mem)),
                                std::istreambuf_iterator<char>());
        ASSERT(mem_content.find("测试偏好") != std::string::npos, "MEMORY has system entry");
        ASSERT(mem_content.find("冷数据") != std::string::npos, "MEMORY has ref entry");
    }
    {
        std::ifstream user(md_dir + "/USER.md");
        std::string user_content((std::istreambuf_iterator<char>(user)),
                                 std::istreambuf_iterator<char>());
        ASSERT(user_content.find("用户风格") != std::string::npos, "USER has user entry");
    }

    // 导入到新数据库
    std::string db2 = "/tmp/test_fact_store_import.db";
    cleanup_db(db2);
    FactStore store2;
    store2.open(db2);
    ASSERT(store2.import_from_md(md_dir), "import");

    auto all2 = store2.find();
    ASSERT(all2.size() >= 2, "import count");  // 至少 user 和 system 导入

    cleanup_db(db);
    cleanup_db(db2);
    fs::remove_all(md_dir);
    return 0;
}

int test_stats() {
    std::string db = "/tmp/test_fact_store_stats.db";
    cleanup_db(db);

    FactStore store;
    store.open(db);

    store.save("system/a", "1");
    store.save("user/a", "2");
    store.save("ref/a", "3");

    auto s = store.stats();
    ASSERT(s.contains("total_entries"), "stats has total");
    ASSERT(s["total_entries"].get<int>() >= 3, "stats total >= 3");
    ASSERT(s.contains("hot_entries"), "stats has hot");
    ASSERT(s["hot_entries"].get<int>() >= 2, "stats hot >= 2");
    ASSERT(s.contains("cold_entries"), "stats has cold");
    ASSERT(s["cold_entries"].get<int>() >= 1, "stats cold >= 1");

    cleanup_db(db);
    return 0;
}

// ── 主入口 ────────────────────────────────────────────────

int main() {
    int failures = 0;

    auto run = [&](const char* name, int (*fn)()) {
        int rc = fn();
        if (rc == 0) {
            std::cout << "  PASS: " << name << std::endl;
        } else {
            std::cout << "  FAIL: " << name << " (rc=" << rc << ")" << std::endl;
            failures++;
        }
    };

    run("save_and_find", test_save_and_find);
    run("upsert", test_upsert);
    run("hot_snapshot", test_hot_snapshot);
    run("snapshot_budget", test_snapshot_budget);
    run("empty_snapshot", test_empty_snapshot);
    run("content_truncation", test_content_truncation);
    run("export_import", test_export_import);
    run("stats", test_stats);

    if (failures == 0) {
        std::cout << "unit:test_fact_store PASS" << std::endl;
        return 0;
    }
    std::cerr << "unit:test_fact_store FAIL (" << failures << " failures)" << std::endl;
    return 1;
}
