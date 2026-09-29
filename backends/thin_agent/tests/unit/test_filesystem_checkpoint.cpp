/// FilesystemCheckpoint unit tests — pure C++, no system() calls

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

#include "thin_agent/agent/FilesystemCheckpoint.h"

using namespace thin_agent::agent;

// Helpers
static void rmdir_r(const std::string& path) {
  // simple recursive rmdir using rm -rf via popen
  std::string cmd = "rm -rf " + path;
  FILE* f = ::popen(cmd.c_str(), "r");
  if (f) ::pclose(f);
}

static std::string mkdtemp_dir(const std::string& prefix) {
  std::string tmpl = "/tmp/" + prefix + "_XXXXXX";
  char* buf = strdup(tmpl.c_str());
  char* r = ::mkdtemp(buf);
  std::string result = r ? r : "";
  free(buf);
  return result;
}

static void write_file(const std::string& dir, const std::string& rel_path,
                       const std::string& content) {
  std::string full = dir + "/" + rel_path;
  size_t slash = full.rfind('/');
  if (slash != std::string::npos) {
    std::string parent = full.substr(0, slash);
    ::mkdir(parent.c_str(), 0755);
  }
  std::ofstream ofs(full);
  if (!ofs) { std::cerr << "CANNOT WRITE: " << full << "\n"; return; }
  ofs << content;
  ofs.close();
}

static std::string read_file(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs) return "";
  return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

static bool test_save_basic() {
  auto dir = mkdtemp_dir("fsckpt_save");
  if (dir.empty()) { std::cerr << "FAIL: mkdtemp\n"; return false; }
  write_file(dir, "hello.txt", "hello world");
  write_file(dir, "sub/foo.cpp", "int main() { return 0; }");

  std::string store = mkdtemp_dir("fsckpt_store");
  if (store.empty()) { rmdir_r(dir); return false; }

  FilesystemCheckpoint fckpt(store);
  std::string ckpt_id = fckpt.save(dir, {}, "test");
  if (ckpt_id.empty()) { std::cerr << "FAIL: save returned empty\n"; rmdir_r(dir); rmdir_r(store); return false; }

  auto snaps = fckpt.list();
  if (snaps.size() != 1) { std::cerr << "FAIL: expected 1 snapshot\n"; rmdir_r(dir); rmdir_r(store); return false; }
  if (snaps[0].files.size() != 2) { std::cerr << "FAIL: expected 2 files\n"; rmdir_r(dir); rmdir_r(store); return false; }

  rmdir_r(dir); rmdir_r(store);
  return true;
}

static bool test_rollback_restore() {
  auto dir = mkdtemp_dir("fsckpt_rollback");
  if (dir.empty()) return false;
  write_file(dir, "original.txt", "original content");

  auto store = mkdtemp_dir("fsckpt_store_rb");
  if (store.empty()) { rmdir_r(dir); return false; }

  FilesystemCheckpoint fckpt(store);
  std::string ckpt_id = fckpt.save(dir, {}, "pre_change");
  if (ckpt_id.empty()) { rmdir_r(dir); rmdir_r(store); return false; }

  write_file(dir, "original.txt", "modified content");

  auto result = fckpt.rollback(ckpt_id, false);
  if (!result.ok) { std::cerr << "FAIL: rollback not ok: " << result.error << "\n"; rmdir_r(dir); rmdir_r(store); return false; }
  if (result.restored.size() != 1) { std::cerr << "FAIL: expected 1 restored\n"; rmdir_r(dir); rmdir_r(store); return false; }

  std::string restored_text = read_file(dir + "/original.txt");
  if (restored_text != "original content") { std::cerr << "FAIL: content not restored: '" << restored_text << "'\n"; rmdir_r(dir); rmdir_r(store); return false; }

  rmdir_r(dir); rmdir_r(store);
  return true;
}

static bool test_rollback_dry_run() {
  auto dir = mkdtemp_dir("fsckpt_dryrun");
  if (dir.empty()) return false;
  write_file(dir, "file.txt", "v1");

  auto store = mkdtemp_dir("fsckpt_store_dry");
  if (store.empty()) { rmdir_r(dir); return false; }

  FilesystemCheckpoint fckpt(store);
  std::string ckpt_id = fckpt.save(dir, {}, "pre");
  write_file(dir, "file.txt", "v2");

  auto result = fckpt.rollback(ckpt_id, true);
  if (!result.ok) { std::cerr << "FAIL: dry_run not ok\n"; rmdir_r(dir); rmdir_r(store); return false; }
  if (result.restored.size() != 1) { std::cerr << "FAIL: expected 1 restored in dry_run\n"; rmdir_r(dir); rmdir_r(store); return false; }

  std::string text = read_file(dir + "/file.txt");
  if (text != "v2") { std::cerr << "FAIL: dry_run modified file\n"; rmdir_r(dir); rmdir_r(store); return false; }

  rmdir_r(dir); rmdir_r(store);
  return true;
}

static bool test_diff() {
  auto dir = mkdtemp_dir("fsckpt_diff");
  if (dir.empty()) return false;
  write_file(dir, "src/main.cpp", "old");
  write_file(dir, "README.md", "readme");

  auto store = mkdtemp_dir("fsckpt_store_diff");
  if (store.empty()) { rmdir_r(dir); return false; }

  FilesystemCheckpoint fckpt(store);
  std::string ckpt_id = fckpt.save(dir, {}, "pre");

  write_file(dir, "src/main.cpp", "new");
  write_file(dir, "NEW.txt", "new file");

  auto d = fckpt.diff(ckpt_id);
  if (!d.contains("summary")) { std::cerr << "FAIL: diff missing summary\n"; rmdir_r(dir); rmdir_r(store); return false; }
  int modified = d["summary"]["modified"];
  int added = d["summary"]["added"];
  if (modified != 1) { std::cerr << "FAIL: expected 1 modified\n"; rmdir_r(dir); rmdir_r(store); return false; }
  if (added != 1) { std::cerr << "FAIL: expected 1 added\n"; rmdir_r(dir); rmdir_r(store); return false; }

  rmdir_r(dir); rmdir_r(store);
  return true;
}

static bool test_exclude_rules() {
  auto dir = mkdtemp_dir("fsckpt_exclude");
  if (dir.empty()) return false;
  write_file(dir, "src/main.cpp", "code");
  write_file(dir, "build/output.o", "binary");
  write_file(dir, ".git/config", "git config");
  write_file(dir, ".env", "SECRET=xxx");

  auto store = mkdtemp_dir("fsckpt_store_excl");
  if (store.empty()) { rmdir_r(dir); return false; }

  FilesystemCheckpoint fckpt(store);
  std::string ckpt_id = fckpt.save(dir, {}, "test");
  if (ckpt_id.empty()) { rmdir_r(dir); rmdir_r(store); return false; }

  auto snaps = fckpt.list();
  int count = snaps[0].files.size();
  if (count != 1) {
    std::cerr << "FAIL: expected 1 file, got " << count << "\n";
    for (const auto& f : snaps[0].files) std::cerr << "  included: " << f.path << "\n";
    rmdir_r(dir); rmdir_r(store); return false;
  }
  if (snaps[0].files[0].path != "src/main.cpp") {
    std::cerr << "FAIL: wrong file included: " << snaps[0].files[0].path << "\n";
    rmdir_r(dir); rmdir_r(store); return false;
  }

  rmdir_r(dir); rmdir_r(store);
  return true;
}

static bool test_auto_save_rate_limit() {
  auto dir = mkdtemp_dir("fsckpt_autosave");
  if (dir.empty()) return false;
  write_file(dir, "f.txt", "data");

  auto store = mkdtemp_dir("fsckpt_store_as");
  if (store.empty()) { rmdir_r(dir); return false; }

  FilesystemCheckpoint fckpt(store);
  fckpt.reset_turn();
  fckpt.mark_dirty();

  std::string ckpt1 = fckpt.auto_save(dir, "turn1");
  if (ckpt1.empty()) { std::cerr << "FAIL: first auto_save should work\n"; rmdir_r(dir); rmdir_r(store); return false; }

  std::string ckpt2 = fckpt.auto_save(dir, "turn1_again");
  if (!ckpt2.empty()) { std::cerr << "FAIL: second auto_save should be empty\n"; rmdir_r(dir); rmdir_r(store); return false; }

  fckpt.reset_turn();
  fckpt.mark_dirty();
  std::string ckpt3 = fckpt.auto_save(dir, "turn2");
  if (ckpt3.empty()) { std::cerr << "FAIL: auto_save after reset should work\n"; rmdir_r(dir); rmdir_r(store); return false; }

  rmdir_r(dir); rmdir_r(store);
  return true;
}

static bool test_empty_dir() {
  auto dir = mkdtemp_dir("fsckpt_empty");
  if (dir.empty()) return false;

  auto store = mkdtemp_dir("fsckpt_store_empty");
  if (store.empty()) { rmdir_r(dir); return false; }

  FilesystemCheckpoint fckpt(store);
  std::string ckpt_id = fckpt.save(dir, {}, "empty");
  if (ckpt_id.empty()) { std::cerr << "FAIL: save on empty dir failed\n"; rmdir_r(dir); rmdir_r(store); return false; }

  auto snaps = fckpt.list();
  if (snaps[0].files.size() != 0) { std::cerr << "FAIL: empty dir should have 0 files\n"; rmdir_r(dir); rmdir_r(store); return false; }

  rmdir_r(dir); rmdir_r(store);
  return true;
}

// ── v0.54.23: pre-FC 去重的**竞态守卫** ──
// 背景：v0.54.20 引入 mtime+size 去重后存在窗口——文件在"上次哈希之后、同一 mtime 粒度格之内"被改写
// 且**大小不变**（粒度 >=1s 的文件系统上两次写入会观察到相同 mtime）⇒ 复用旧 sha256 ⇒ 回滚内容错误。
// 守卫：仅当该 mtime **明显早于**上一次快照保存时刻（2s 余量）才允许复用。
static bool test_dedup_racy_guard() {
  auto dir = mkdtemp_dir("fsckpt_racy");
  if (dir.empty()) return false;
  write_file(dir, "dup.txt", "AAAA");
  const std::string f = dir + "/dup.txt";
  const auto fresh_mtime = std::filesystem::last_write_time(f);   // 新鲜 mtime ~ now（窗口内状态）

  auto store = mkdtemp_dir("fsckpt_store_racy");
  if (store.empty()) { rmdir_r(dir); return false; }
  FilesystemCheckpoint fckpt(store);

  const std::string c1 = fckpt.save(dir, {}, "s1");
  if (c1.empty()) { rmdir_r(dir); rmdir_r(store); return false; }

  // 改写为**同大小**不同内容，并把 mtime 强制回原值（模拟 mtime 粒度碰撞）
  write_file(dir, "dup.txt", "BBBB");
  std::filesystem::last_write_time(f, fresh_mtime);

  const std::string c2 = fckpt.save(dir, {}, "s2");
  if (c2.empty()) { rmdir_r(dir); rmdir_r(store); return false; }

  std::string h1, h2;
  for (const auto& sn : fckpt.list()) {
    for (const auto& e : sn.files) {
      if (e.path != "dup.txt") continue;
      if (sn.checkpoint_id == c1) h1 = e.sha256;
      if (sn.checkpoint_id == c2) h2 = e.sha256;
    }
  }
  bool ok = true;
  if (c1 == c2) {
    // v0.54.23 一并回归：旧实现只用 now_ms 生成 id ⇒ 同毫秒两次 save 拿到同一 id，
    // 第二次覆盖第一次的 manifest（本用例正是靠这条断言抓出来的）
    std::cerr << "FAIL: racy_guard — 两个快照 id 相同（" << c1 << "），后者会覆盖前者\n";
    ok = false;
  } else if (h1.empty() || h2.empty()) {
    std::cerr << "FAIL: racy_guard — 快照里找不到 dup.txt\n";
    ok = false;
  } else if (h1 == h2) {
    // 内容确实变了（AAAA→BBBB）⇒ sha256 必须不同；相同即复用了陈旧哈希（回滚会还原成 AAAA）
    std::cerr << "FAIL: racy_guard — 复用了陈旧 sha256（" << h1 << "）\n";
    ok = false;
  }
  rmdir_r(dir); rmdir_r(store);
  return ok;
}

// 反向用例：文件确实未变且 mtime 明显早于上次快照 ⇒ 去重**必须生效**（否则 v0.54.20 的性能修复形同虚设）
static bool test_dedup_reuses_unchanged() {
  auto dir = mkdtemp_dir("fsckpt_dedup");
  if (dir.empty()) return false;
  write_file(dir, "keep.txt", "unchanged-content");
  std::filesystem::last_write_time(
      dir + "/keep.txt",
      std::filesystem::file_time_type::clock::now() - std::chrono::seconds(10));

  auto store = mkdtemp_dir("fsckpt_store_dedup");
  if (store.empty()) { rmdir_r(dir); return false; }
  FilesystemCheckpoint fckpt(store);
  if (fckpt.save(dir, {}, "s1").empty() || fckpt.save(dir, {}, "s2").empty()) {
    rmdir_r(dir); rmdir_r(store); return false;
  }
  const int reused = fckpt.last_reused_count();
  const bool ok = reused >= 1;
  if (!ok) std::cerr << "FAIL: dedup_reuse — 未发生复用（reused=" << reused << "）\n";
  else std::cout << "  (去重复用 " << reused << " 个文件)\n";
  rmdir_r(dir); rmdir_r(store);
  return ok;
}

int main() {
  int failures = 0;
  auto run = [&](const char* name, bool (*fn)()) {
    std::cout << "  " << name << " ... ";
    bool ok = false;
    try { ok = fn(); } catch (const std::exception& e) {
      std::cerr << "EXCEPTION: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "UNKNOWN EXCEPTION\n";
    }
    if (ok) { std::cout << "PASS\n"; } else { std::cout << "FAIL\n"; ++failures; }
  };

  run("save_basic", test_save_basic);
  run("rollback_restore", test_rollback_restore);
  run("rollback_dry_run", test_rollback_dry_run);
  run("diff", test_diff);
  run("exclude_rules", test_exclude_rules);
  run("auto_save_rate_limit", test_auto_save_rate_limit);
  run("empty_dir", test_empty_dir);
  run("dedup_racy_guard", test_dedup_racy_guard);
  run("dedup_reuses_unchanged", test_dedup_reuses_unchanged);

  std::cout << "\n=== " << (failures == 0 ? "ALL PASSED" : "FAILURES: " + std::to_string(failures)) << " ===\n";
  return failures > 0 ? 1 : 0;
}
