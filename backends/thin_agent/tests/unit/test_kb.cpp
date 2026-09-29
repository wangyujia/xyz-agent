/// unit_kb：知识库插件测试
///
/// 测试 thin_agent::kb 的 helper 函数
/// Lambda handler 间接通过 helper 测试覆盖

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

// Include kb plugin to test helpers
#include "../../src/plugin/skills/kb.cpp"

#define ASSERT(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << std::endl; return 1; } \
} while (0)
#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { std::cerr << "FAIL: " << msg << " (" << (a) << " != " << (b) << ")" << std::endl; return 1; } \
} while (0)

#define TEST(name) int test_##name()

using namespace nlohmann;
namespace kb = thin_agent::kb;

static std::string g_tmp_dir = "/tmp/ta_kb_test";

static void setup() {
  system(("rm -rf " + g_tmp_dir).c_str());
  system(("mkdir -p " + g_tmp_dir).c_str());
  // point HOME to tmp dir so kb_dir() resolves there
  setenv("HOME", g_tmp_dir.c_str(), 1);
  // Reset global state
  kb::g_kb_dir.clear();
}

static void teardown() {
  system(("rm -rf " + g_tmp_dir).c_str());
}

// ── kb_dir ──

TEST(kb_dir_default) {
  setup();
  std::string d = kb::kb_dir();
  ASSERT(d.find(g_tmp_dir) != std::string::npos, "dir contains tmp path");
  ASSERT(d.find(".thin_agent/kb") != std::string::npos, "dir ends with .thin_agent/kb");
  teardown();
  std::cout << "pass: kb_dir_default" << std::endl;
  return 0;
}

TEST(kb_dir_cached) {
  setup();
  std::string d1 = kb::kb_dir();
  std::string d2 = kb::kb_dir();
  ASSERT_EQ(d1, d2, "cached dir should be same");
  teardown();
  std::cout << "pass: kb_dir_cached" << std::endl;
  return 0;
}

// ── kb_db_path ──

TEST(kb_db_path_no_index) {
  setup();
  std::string path = kb::kb_db_path("codebase");
  // Without kb_index.json, it falls back to default
  ASSERT(!path.empty(), "path should not be empty");
  ASSERT(path.find("codebase.db") != std::string::npos, "path should contain codebase.db");
  teardown();
  std::cout << "pass: kb_db_path_no_index" << std::endl;
  return 0;
}

// ── load_kb_index ──

TEST(load_kb_index_empty) {
  setup();
  // Without kb_index.json file
  auto idx = kb::load_kb_index();
  ASSERT(idx.is_object(), "should return object");
  std::cout << "pass: load_kb_index_empty" << std::endl;
  teardown();
  return 0;
}

TEST(load_kb_index_with_data) {
  setup();
  // Create kb_index.json
  system(("mkdir -p " + g_tmp_dir + "/.thin_agent/kb").c_str());
  std::ofstream f(g_tmp_dir + "/.thin_agent/kb/kb_index.json");
  f << R"({"kbs":{"codebase":{"type":"repo_scan","repos":["/src"],"extensions":[".cpp"]}}})";
  f.close();

  auto idx = kb::load_kb_index();
  ASSERT(idx.contains("kbs"), "should have kbs key");
  ASSERT(idx["kbs"].contains("codebase"), "should have codebase kb");
  ASSERT_EQ(idx["kbs"]["codebase"]["type"].get<std::string>(), "repo_scan", "type should be repo_scan");
  teardown();
  std::cout << "pass: load_kb_index_with_data" << std::endl;
  return 0;
}

// ── save_kb_index ──

TEST(save_and_load_roundtrip) {
  setup();
  system(("mkdir -p " + g_tmp_dir + "/.thin_agent/kb").c_str());

  json idx;
  idx["kbs"] = json::object();
  idx["kbs"]["test_kb"] = {
    {"type", "manual"},
    {"doc_count", 5}
  };

  bool saved = kb::save_kb_index(idx);
  ASSERT(saved, "save should succeed");

  auto loaded = kb::load_kb_index();
  ASSERT(loaded["kbs"].contains("test_kb"), "loaded should have test_kb");
  ASSERT_EQ(loaded["kbs"]["test_kb"]["doc_count"].get<int>(), 5, "doc_count should be 5");

  teardown();
  std::cout << "pass: save_and_load_roundtrip" << std::endl;
  return 0;
}

int main() {
  int fails = 0;
  #define RUN(name) do { \
    std::cout << "test: " #name "..." << std::endl; \
    if (test_##name() != 0) ++fails; \
  } while (0)

  RUN(kb_dir_default);
  RUN(kb_dir_cached);
  RUN(kb_db_path_no_index);
  RUN(load_kb_index_empty);
  RUN(load_kb_index_with_data);
  RUN(save_and_load_roundtrip);

  teardown();
  std::cout << (fails == 0 ? "ALL PASS" : "SOME FAILED") << " (" << fails << " failures)" << std::endl;
  return fails;
}
