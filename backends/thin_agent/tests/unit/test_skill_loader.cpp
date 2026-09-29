// test_skill_loader：v0.53.9 SKILL.md 适配器回归
//
// 覆盖：frontmatter 解析（标准/无frontmatter宽松/fallback目录名/引号剥离）/
// 扫描（嵌套忽略/非SKILL.md忽略/循环防护）+ load_skill 索引/全文/未找到。

#include "../../src/plugin/skills/skill_loader.cpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

using namespace thin_agent;
using namespace thin_agent::skill_loader;

static int g_failures = 0;
#define CHECK(cond, msg) \
  do { if (!(cond)) { ++g_failures; std::printf("FAIL: %s\n", msg); } \
       else { std::printf("PASS: %s\n", msg); } } while (0)

static void write_file(const std::string& p, const std::string& c) {
  // 递归建目录（mkdir 非递归：一层层来）
  std::string acc;
  for (size_t i = 0; i < p.size(); ++i) {
    if (p[i] == '/' && i > 0) ::mkdir(acc.c_str(), 0755);
    acc += p[i];
  }
  std::ofstream(p) << c;
}

static void cleanup_dir(const char* d) {
  if (::system((std::string("rm -rf ") + d).c_str()) != 0) {}  // 清理失败不影响测试
}

int main() {
  cleanup_dir("/tmp/ta_sl_test");

  // 1) 标准 frontmatter
  write_file("/tmp/ta_sl_test/skills/pdf-pro/SKILL.md",
             "---\nname: pdf-processing\ndescription: \"处理 PDF：合并/拆分/提取\"\n---\n\n# PDF\n\n用 pypdf 提取文本。\n");
  // 2) 无 frontmatter（宽松：全文为正文，name=目录名 fallback）
  write_file("/tmp/ta_sl_test/skills/plain/SKILL.md",
             "# 纯正文 skill\n\n直接 markdown。\n");
  // 3) 非 SKILL.md（应忽略）
  write_file("/tmp/ta_sl_test/skills/other/README.md", "# 不是 skill");
  // 4) 单引号 description + 多余字段
  write_file("/tmp/ta_sl_test/skills/excel/SKILL.md",
             "---\nname: excel\nlicense: MIT\ndescription: 'Excel 处理'\n---\n手册正文\n");
  // 5) v0.53.15: UTF-8 BOM 头（Windows 编辑器常见——真审计实测 BOM 使首行
  //    不匹配 frontmatter，name 退化成目录名）
  write_file("/tmp/ta_sl_test/skills/bomdir/SKILL.md",
             "\xEF\xBB\xBF---\nname: bom-skill\ndescription: BOM 头\n---\n正文\n");

  int n = scan_skills("/tmp/ta_sl_test");
  CHECK(n == 4, "扫描: 4 个 SKILL.md（README 忽略）");
  CHECK(g_skills.count("pdf-processing") == 1, "frontmatter: name 解析");
  CHECK(g_skills["pdf-processing"].description == "处理 PDF：合并/拆分/提取",
        "frontmatter: description 引号剥离");
  CHECK(g_skills["pdf-processing"].body.find("pypdf") != std::string::npos,
        "frontmatter: 正文在 body（不含 frontmatter）");
  CHECK(g_skills["pdf-processing"].dir.find("pdf-pro") != std::string::npos,
        "目录: dir 记录（资产根）");
  CHECK(g_skills.count("plain") == 1, "宽松: 无 frontmatter 用目录名");
  CHECK(g_skills.count("bom-skill") == 1, "v0.53.15: BOM 头剥落后 name 正确解析");
  CHECK(g_skills["plain"].body.find("纯正文") != std::string::npos,
        "宽松: 全文为正文");
  CHECK(g_skills.count("excel") == 1 && g_skills["excel"].description == "Excel 处理",
        "单引号: description 剥离+未知字段跳过");

  // 5) load_skill 索引（渐进披露第一层）
  {
    auto r = handle_load_skill(nlohmann::json::object());
    CHECK(r["success"].get<bool>() && r["count"] == 4, "load_skill(): 索引 count=4（含 BOM 例）");
    bool has_hint = r.contains("hint");
    CHECK(has_hint, "load_skill(): 带 hint（引导按需加载）");
  }
  // 6) 全文加载
  {
    auto r = handle_load_skill(nlohmann::json{{"name", "pdf-processing"}});
    CHECK(r["success"].get<bool>(), "load_skill(name): 命中");
    CHECK(r["content"].get<std::string>().find("pypdf") != std::string::npos,
          "load_skill(name): 手册全文");
    CHECK(r["dir"].get<std::string>().find("pdf-pro") != std::string::npos,
          "load_skill(name): dir（LLM 可引用 scripts 资产）");
  }
  // 7) 未找到
  {
    auto r = handle_load_skill(nlohmann::json{{"name", "nonexistent"}});
    CHECK(!r["success"].get<bool>() && r["error"] == "skill_not_found",
          "load_skill(不存在): skill_not_found");
  }
  // 8) 空目录
  {
    ::mkdir("/tmp/ta_sl_empty", 0755);
    int m = scan_skills("/tmp/ta_sl_empty");
    CHECK(m == 0 && g_skills.empty(), "空目录: 0 skill");
  }
  // 9) 不存在的根
  {
    int m = scan_skills("/tmp/ta_sl_nope_xyz");
    CHECK(m == 0, "不存在的根: 0（不崩）");
  }

  cleanup_dir("/tmp/ta_sl_test /tmp/ta_sl_empty");
  if (g_failures == 0) std::printf("ALL PASS\n");
  return g_failures == 0 ? 0 : 1;
}
