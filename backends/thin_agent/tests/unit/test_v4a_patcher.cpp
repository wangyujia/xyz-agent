// unit_v4a_patcher：V4APatcher 多文件 patch + 原子语义测试

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "thin_agent/core/V4APatcher.h"

static int g_failures = 0;
static void check(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAIL: " << msg << '\n'; ++g_failures; }
}

static std::string read_file(const std::string& p) {
  std::ifstream in(p);
  if (!in.is_open()) return "";
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

int main() {
  using thin_agent::V4APatcher;

  // 临时目录
  const char* dir = "/tmp/v4a_test";
  std::system("rm -rf /tmp/v4a_test && mkdir -p /tmp/v4a_test");

  // ── 1. parse：两个文件块 ──
  {
    V4APatcher p;
    std::vector<V4APatcher::FilePatch> files;
    std::string txt =
        "*** Begin Patch\n"
        "*** Update File: /tmp/v4a_test/a.cpp\n"
        " int x = 1;\n"
        "-int y = 2;\n"
        "+int y = 42;\n"
        " int z = 3;\n"
        "*** Update File: /tmp/v4a_test/b.h\n"
        "-#define OLD 1\n"
        "+#define NEW 2\n"
        "*** End Patch\n";
    bool ok = p.parse(txt, files);
    check(ok, "parse ok");
    check(files.size() == 2, "parse 2 files");
    check(files[0].path == "/tmp/v4a_test/a.cpp", "file a path");
    check(files[1].path == "/tmp/v4a_test/b.h", "file b path");
    check(files[0].hunks.size() == 1, "file a 1 hunk");
    if (!files.empty() && !files[0].hunks.empty()) {
      auto& h = files[0].hunks[0];
      check(h.old_text.find("int y = 2;") != std::string::npos,
            "hunk old has removed line");
      check(h.new_text.find("int y = 42;") != std::string::npos,
            "hunk new has added line");
      check(h.old_text.find("int x = 1;") != std::string::npos,
            "hunk old has context");
    }
  }

  // ── 2. apply_to_disk：两文件同时改 ──
  {
    std::ofstream("/tmp/v4a_test/a.cpp") << "int x = 1;\nint y = 2;\nint z = 3;\n";
    std::ofstream("/tmp/v4a_test/b.h") << "#define OLD 1\n";
    V4APatcher p;
    std::string txt =
        "*** Begin Patch\n"
        "*** Update File: /tmp/v4a_test/a.cpp\n"
        " int x = 1;\n"
        "-int y = 2;\n"
        "+int y = 42;\n"
        " int z = 3;\n"
        "*** Update File: /tmp/v4a_test/b.h\n"
        "-#define OLD 1\n"
        "+#define NEW 2\n"
        "*** End Patch\n";
    auto r = p.apply_to_disk(txt);
    check(r.ok, "apply ok");
    check(r.applied_files.size() == 2, "applied 2 files");
    check(read_file("/tmp/v4a_test/a.cpp").find("int y = 42;") != std::string::npos,
          "a.cpp content updated");
    check(read_file("/tmp/v4a_test/b.h").find("#define NEW 2") != std::string::npos,
          "b.h content updated");
  }

  // ── 3. 原子语义：一个文件缺失 → 全部不写盘 ──
  {
    std::ofstream("/tmp/v4a_test/only.cpp") << "int keep = 1;\n";
    V4APatcher p;
    std::string txt =
        "*** Begin Patch\n"
        "*** Update File: /tmp/v4a_test/only.cpp\n"
        "-int keep = 1;\n"
        "+int changed = 2;\n"
        "*** Update File: /tmp/v4a_test/missing.cpp\n"
        "-nope\n"
        "+yep\n"
        "*** End Patch\n";
    auto r = p.apply_to_disk(txt);
    check(!r.ok, "atomic fail on missing file");
    check(r.errors.size() == 1, "one error recorded");
    check(read_file("/tmp/v4a_test/only.cpp").find("int keep = 1;") != std::string::npos,
          "only.cpp NOT modified (atomic)");
  }

  // ── 4. 模糊匹配：缩进差异仍可定位 ──
  {
    std::ofstream("/tmp/v4a_test/indent.cpp") << "void f() {\n    int a = 1;\n}\n";
    V4APatcher p;
    std::string txt =
        "*** Begin Patch\n"
        "*** Update File: /tmp/v4a_test/indent.cpp\n"
        " void f() {\n"
        "-  int a = 1;\n"
        "+  int a = 999;\n"
        " }\n"
        "*** End Patch\n";
    auto r = p.apply_to_disk(txt);
    check(r.ok, "fuzzy indent apply ok");
    check(read_file("/tmp/v4a_test/indent.cpp").find("int a = 999;") != std::string::npos,
          "fuzzy indent replaced");
  }

  std::system("rm -rf /tmp/v4a_test");

  if (g_failures == 0) {
    std::cout << "unit_v4a_patcher: ALL PASS\n";
    return 0;
  }
  std::cerr << g_failures << " failure(s)\n";
  return 1;
}
