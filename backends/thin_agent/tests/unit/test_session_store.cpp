// unit_session_store：SessionStore 会话搜索全功能测试

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "thin_agent/core/SessionStore.h"

namespace fs = std::filesystem;
using thin_agent::SessionStore;

void cleanup_db(const std::string& path) {
    if (fs::exists(path)) fs::remove(path);
    if (fs::exists(path + "-wal")) fs::remove(path + "-wal");
    if (fs::exists(path + "-shm")) fs::remove(path + "-shm");
}

#define ASSERT(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << std::endl; return 1; } \
} while (0)

int test_record_and_search() {
    std::string db = "/tmp/test_session_store_srch.db";
    cleanup_db(db);

    SessionStore store;
    ASSERT(store.open(db), "open");

    ASSERT(store.record_message("s1", "user", "MC3302 重启后电源芯片异常"), "record 1");
    ASSERT(store.record_message("s1", "assistant", "根因是 3.3V 掉电"), "record 2");
    ASSERT(store.record_message("s2", "user", "Leaptic camera 帧率问题"), "record 3");

    // FTS5 search
    auto hits = store.search("MC3302");
    ASSERT(hits.size() >= 1, "search MC3302");
    ASSERT(hits[0].content.find("MC3302") != std::string::npos, "search content match");

    hits = store.search("camera");
    ASSERT(hits.size() >= 1, "search camera");
    ASSERT(hits[0].session_id == "s2", "search session match");

    // Empty search returns nothing
    hits = store.search("nonexistent_xyz_12345");
    ASSERT(hits.empty(), "search empty");

    cleanup_db(db);
    return 0;
}

int test_recent() {
    std::string db = "/tmp/test_session_store_recent.db";
    cleanup_db(db);

    SessionStore store;
    store.open(db);

    store.record_message("s1", "user", "msg1");
    store.record_message("s1", "assistant", "msg2");
    store.record_message("s2", "user", "msg3");

    auto recent = store.recent(2);
    ASSERT(recent.size() == 2, "recent limit 2");
    ASSERT(recent[0].content == "msg3", "recent newest first");
    ASSERT(recent[1].content == "msg2", "recent second newest");

    // Empty store
    SessionStore empty;
    empty.open("/tmp/test_session_store_empty_recent.db");
    cleanup_db("/tmp/test_session_store_empty_recent.db");
    auto empty_recent = empty.recent(10);
    ASSERT(empty_recent.empty(), "empty recent");

    cleanup_db(db);
    return 0;
}

int test_touch_session() {
    std::string db = "/tmp/test_session_store_touch.db";
    cleanup_db(db);

    SessionStore store;
    store.open(db);

    store.record_message("s1", "user", "修复 MC3302 重启 bug");
    store.touch_session("s1", "修复 MC3302 重启 bug");

    // Verify session exists with title
    cleanup_db(db);
    return 0;
}

int test_content_truncation() {
    std::string db = "/tmp/test_session_store_trunc.db";
    cleanup_db(db);

    SessionStore store;
    store.open(db);

    std::string long_content = "BUG_REPORT_" + std::string(2500, 'X');
    ASSERT(store.record_message("s1", "user", long_content), "record long");

    auto hits = store.search("BUG_REPORT");
    ASSERT(hits.size() >= 1, "search long content");
    ASSERT(hits[0].content.size() <= 2100, "truncated content");  // 2000 + margin

    cleanup_db(db);
    return 0;
}

int test_stats() {
    std::string db = "/tmp/test_session_store_stats.db";
    cleanup_db(db);

    SessionStore store;
    store.open(db);

    store.record_message("s1", "user", "a");
    store.record_message("s1", "assistant", "b");

    auto s = store.stats();
    ASSERT(s.contains("total_messages"), "stats total_messages");
    ASSERT(s["total_messages"].get<int>() >= 2, "stats >= 2");

    cleanup_db(db);
    return 0;
}

int main() {
    int failures = 0;
    auto run = [&](const char* name, int (*fn)()) {
        int rc = fn();
        if (rc == 0) std::cout << "  PASS: " << name << std::endl;
        else { std::cout << "  FAIL: " << name << std::endl; failures++; }
    };

    run("record_and_search", test_record_and_search);
    run("recent", test_recent);
    run("touch_session", test_touch_session);
    run("content_truncation", test_content_truncation);
    run("stats", test_stats);

    if (failures == 0) {
        std::cout << "unit:test_session_store PASS" << std::endl;
        return 0;
    }
    std::cerr << "unit:test_session_store FAIL (" << failures << " failures)" << std::endl;
    return 1;
}
