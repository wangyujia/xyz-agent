/// unit_code_dev：编程技能插件测试
///
/// 测试 thin_agent::code_dev 的 4 个 handler

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "thin_agent/plugin/PluginInterface.h"
#include "thin_agent/core/FuzzyPatcher.h"
#include "../../src/plugin/skills/code_dev.cpp"

#define ASSERT(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << std::endl; return 1; } \
} while (0)
#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { std::cerr << "FAIL: " << msg << " (" << (a) << " != " << (b) << ")" << std::endl; return 1; } \
} while (0)

#define TEST(name) int test_##name()

using namespace nlohmann;

static const std::string g_tmp_dir = "/tmp/ta_code_dev_test";

static void setup_dir() {
  system(("mkdir -p " + g_tmp_dir).c_str());
  system(("rm -rf " + g_tmp_dir + "/*").c_str());
}

static void teardown_dir() {
  system(("rm -rf " + g_tmp_dir).c_str());
}

static std::string make_file(const std::string& name, const std::string& content) {
  std::string path = g_tmp_dir + "/" + name;
  std::ofstream f(path);
  f << content;
  f.close();
  return path;
}

// ── code_read_file ──

TEST(read_file_success) {
  setup_dir();
  std::string path = make_file("test.txt", "line1\nline2\nline3\n");
  json params = {{"path", path}};
  auto r = thin_agent::code_dev::handle_read_file(params);
  ASSERT(r["success"].get<bool>(), "should succeed");
  ASSERT_EQ(r["total_lines"].get<int>(), 3, "3 lines");
  ASSERT(r["output"].get<std::string>().find("1|line1") != std::string::npos, "has line1");
  ASSERT(r["output"].get<std::string>().find("2|line2") != std::string::npos, "has line2");
  teardown_dir();
  std::cout << "pass: read_file_success" << std::endl;
  return 0;
}

TEST(read_file_empty_path) {
  json params = {{"path", ""}};
  auto r = thin_agent::code_dev::handle_read_file(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty path");
  std::cout << "pass: read_file_empty_path" << std::endl;
  return 0;
}

TEST(read_file_not_found) {
  json params = {{"path", "/nonexistent/file_xyz_123.txt"}};
  auto r = thin_agent::code_dev::handle_read_file(params);
  ASSERT(!r["success"].get<bool>(), "should fail on missing file");
  std::cout << "pass: read_file_not_found" << std::endl;
  return 0;
}

TEST(read_file_offset_limit) {
  setup_dir();
  std::string path = make_file("lines.txt",
    "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n");
  json params = {{"path", path}, {"offset", 4}, {"limit", 3}};
  auto r = thin_agent::code_dev::handle_read_file(params);
  ASSERT(r["success"].get<bool>(), "should succeed");
  ASSERT_EQ(r["start_line"].get<int>(), 4, "start at 4");
  ASSERT_EQ(r["end_line"].get<int>(), 6, "end at 6");
  ASSERT(r["output"].get<std::string>().find("4|4") != std::string::npos, "has line4");
  ASSERT(r["output"].get<std::string>().find("7|7") == std::string::npos, "no line7");
  teardown_dir();
  std::cout << "pass: read_file_offset_limit" << std::endl;
  return 0;
}

// ── code_write_file ──

TEST(write_file_success) {
  setup_dir();
  std::string path = g_tmp_dir + "/out.txt";
  json params = {{"path", path}, {"content", "hello world"}};
  auto r = thin_agent::code_dev::handle_write_file(params);
  ASSERT(r["success"].get<bool>(), "should succeed");
  ASSERT_EQ(r["bytes_written"].get<int>(), 11, "11 bytes");
  // verify file exists
  std::ifstream in(path);
  ASSERT(in.is_open(), "file should exist");
  std::string content;
  std::getline(in, content);
  ASSERT_EQ(content, "hello world", "file content");
  teardown_dir();
  std::cout << "pass: write_file_success" << std::endl;
  return 0;
}

TEST(write_file_empty_path) {
  json params = {{"path", ""}, {"content", "x"}};
  auto r = thin_agent::code_dev::handle_write_file(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty path");
  std::cout << "pass: write_file_empty_path" << std::endl;
  return 0;
}

TEST(write_file_empty_content) {
  json params = {{"path", "/tmp/test.txt"}, {"content", ""}};
  auto r = thin_agent::code_dev::handle_write_file(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty content");
  std::cout << "pass: write_file_empty_content" << std::endl;
  return 0;
}

// ── code_patch ──

TEST(patch_exact_replace) {
  setup_dir();
  std::string path = make_file("patch.txt", "hello world\nfoo bar\n");
  json params = {{"path", path}, {"old_string", "hello world"}, {"new_string", "hi there"}};
  auto r = thin_agent::code_dev::handle_patch(params);
  ASSERT(r["success"].get<bool>(), "should succeed");
  ASSERT(r["replacements"].get<int>() >= 1, "at least 1 replacement");
  // verify file
  std::ifstream in(path);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ASSERT(content.find("hi there") != std::string::npos, "has new text");
  ASSERT(content.find("hello world") == std::string::npos, "old text gone");
  teardown_dir();
  std::cout << "pass: patch_exact_replace" << std::endl;
  return 0;
}

TEST(patch_empty_path) {
  json params = {{"path", ""}, {"old_string", "x"}, {"new_string", "y"}};
  auto r = thin_agent::code_dev::handle_patch(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty path");
  std::cout << "pass: patch_empty_path" << std::endl;
  return 0;
}

TEST(patch_empty_old_string) {
  json params = {{"path", "/tmp/x"}, {"old_string", ""}, {"new_string", "y"}};
  auto r = thin_agent::code_dev::handle_patch(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty old_string");
  std::cout << "pass: patch_empty_old_string" << std::endl;
  return 0;
}

// ── code_search ──

TEST(search_basic) {
  setup_dir();
  make_file("a.cpp", "int main() {\n  return 0;\n}\n");
  make_file("b.h", "#pragma once\n");
  json params = {{"pattern", "main"}, {"dir", g_tmp_dir}, {"file_glob", "*.cpp"}};
  auto r = thin_agent::code_dev::handle_search(params);
  ASSERT(r["success"].get<bool>(), "should succeed");
  ASSERT(r["match_count"].get<int>() >= 1, "found main");
  ASSERT(r["output"].get<std::string>().find("main") != std::string::npos, "output has match");
  teardown_dir();
  std::cout << "pass: search_basic" << std::endl;
  return 0;
}

TEST(search_empty_pattern) {
  json params = {{"pattern", ""}};
  auto r = thin_agent::code_dev::handle_search(params);
  ASSERT(!r["success"].get<bool>(), "should fail on empty pattern");
  std::cout << "pass: search_empty_pattern" << std::endl;
  return 0;
}

TEST(search_bad_regex) {
  json params = {{"pattern", "[unclosed"}};
  auto r = thin_agent::code_dev::handle_search(params);
  ASSERT(!r["success"].get<bool>(), "should fail on bad regex");
  std::cout << "pass: search_bad_regex" << std::endl;
  return 0;
}

TEST(search_no_shell_inject) {
  // v0.54.34: pattern 含引号/分号不得落盘副作用（旧实现拼 grep 可注入）
  setup_dir();
  make_file("safe.cpp", "int safe_token_v05434 = 1;\n");
  const std::string marker = g_tmp_dir + "/inject_side_effect.flag";
  std::remove(marker.c_str());
  json params = {
    {"pattern", "x' /tmp; touch " + marker + "; echo '"},
    {"dir", g_tmp_dir},
    {"file_types", "*.cpp"}
  };
  auto r = thin_agent::code_dev::handle_search(params);
  // 非法/怪异 pattern：要么 bad_regex，要么纯搜索成功且无副作用
  ASSERT(std::filesystem::exists(marker) == false, "must not shell-inject");
  (void)r;
  teardown_dir();
  std::cout << "pass: search_no_shell_inject" << std::endl;
  return 0;
}

TEST(search_files_only) {
  setup_dir();
  make_file("a.cpp", "foo_bar_unique\n");
  make_file("b.h", "foo_bar_unique\n");
  json params = {{"pattern", "foo_bar_unique"}, {"dir", g_tmp_dir},
                 {"file_types", "*.cpp"}, {"output_mode", "files_only"}};
  auto r = thin_agent::code_dev::handle_search(params);
  ASSERT(r["success"].get<bool>(), "files_only ok");
  ASSERT(r["match_count"].get<int>() >= 1, "at least one file");
  ASSERT(r["output"].get<std::string>().find("a.cpp") != std::string::npos, "lists a.cpp");
  ASSERT(r["output"].get<std::string>().find("b.h") == std::string::npos, "filtered b.h");
  teardown_dir();
  std::cout << "pass: search_files_only" << std::endl;
  return 0;
}

int main() {
  int fails = 0;
  #define RUN(name) do { \
    std::cout << "test: " #name "..." << std::endl; \
    if (test_##name() != 0) ++fails; \
  } while (0)

  RUN(read_file_success);
  RUN(read_file_empty_path);
  RUN(read_file_not_found);
  RUN(read_file_offset_limit);
  RUN(write_file_success);
  RUN(write_file_empty_path);
  RUN(write_file_empty_content);
  RUN(patch_exact_replace);
  RUN(patch_empty_path);
  RUN(patch_empty_old_string);
  RUN(search_basic);
  RUN(search_empty_pattern);
  RUN(search_bad_regex);
  RUN(search_no_shell_inject);
  RUN(search_files_only);

  teardown_dir();
  std::cout << (fails == 0 ? "ALL PASS" : "SOME FAILED") << " (" << fails << " failures)" << std::endl;
  return fails;
}
