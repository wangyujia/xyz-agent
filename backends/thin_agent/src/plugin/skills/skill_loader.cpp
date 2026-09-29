// v0.53.9: skill_loader 插件 —— SKILL.md 适配器（Anthropic Agent Skills 规范）。
//
// 市面 skill 市场（Anthropic 官方+社区，数百个）的 skill = 目录+SKILL.md：
// YAML frontmatter（name/description）+ markdown 正文（教 LLM 做事的操作手册）。
// 本插件把该格式接入 thin_agent：
//   ①init2 扫描 <home>/skills/*/SKILL.md（home=THIN_AGENT_HOME|$HOME/.thin_agent）
//   ②渐进披露：load_skill("") 返回索引（name+description），load_skill(name)
//     返回手册全文——LLM 按需加载，不占常驻 prompt
//   ③手册内容指导 LLM 组合既有工具（code_exec 多语言/shell/文件操作）执行
// frontmatter 解析=微型 key: value 解析器（规范只需 name/description 两字段，
// 不引 yaml-cpp 第三方依赖）。
//
// 工具描述声明走 chat_policy.json 的 skills 段（中文文案外置铁律）。

#include <dirent.h>
#include <sys/stat.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "thin_agent/RuntimePaths.h"
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/plugin/PluginContext.h"

namespace thin_agent {
namespace skill_loader {

struct SkillMd {
  std::string dir;         ///< skill 目录（scripts/ 等资产的根）
  std::string name;        ///< frontmatter name（fallback=目录名）
  std::string description; ///< frontmatter description
  std::string body;        ///< markdown 正文（frontmatter 之后全文）
};

static std::unordered_map<std::string, SkillMd> g_skills;
/// v0.53.56: map 互斥——reload(重建)与 load_skill(LLM 并发查询)
/// 竞态=UB;读写双锁
static std::mutex g_skills_mu;  ///< key=name

/// 微型 frontmatter 解析：--- 起 --- 止之间的 `key: value` 行。
/// 仅取 name/description（规范核心字段）；解析失败不致命（fallback 目录名）。
static bool parse_skill_md(const std::string& path, SkillMd& out) {
  std::ifstream f(path);
  if (!f.is_open()) return false;
  std::string line;
  if (!std::getline(f, line)) return false;
  // v0.53.15: 剥 UTF-8 BOM——Windows 编辑器普遍产生（真审计实测 BOM 头
  // SKILL.md 首行变 "\ufeff---" 不匹配 frontmatter，name 退化成目录名）。
  if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
      static_cast<unsigned char>(line[1]) == 0xBB &&
      static_cast<unsigned char>(line[2]) == 0xBF)
    line.erase(0, 3);
  // 首行必须是 ---（允许 \r）
  auto trim = [](std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
  };
  trim(line);
  if (line != "---") {
    // 无 frontmatter：整文件当正文（宽松兼容）
    f.clear(); f.seekg(0);
    std::ostringstream ss; ss << f.rdbuf();
    out.body = ss.str();
    return !out.body.empty();
  }
  bool fm_done = false;
  while (std::getline(f, line)) {
    trim(line);
    if (!fm_done) {
      if (line == "---") { fm_done = true; continue; }
      auto colon = line.find(':');
      if (colon != std::string::npos) {
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        trim(k); trim(v);
        // 去引号
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') &&
            v.back() == v.front()) v = v.substr(1, v.size() - 2);
        if (k == "name") out.name = v;
        else if (k == "description") out.description = v;
      }
    } else {
      out.body += line + "\n";
    }
  }
  return true;
}

/// 扫描 <home>/skills/*/SKILL.md
static int scan_skills(const std::string& home, int depth = 0) {
  { std::lock_guard<std::mutex> lk(g_skills_mu); g_skills.clear(); }
  if (depth > 2) return 0;  // 防循环
  std::string dir = home + "/skills";
  DIR* d = ::opendir(dir.c_str());
  if (!d) return 0;
  int count = 0;
  while (struct dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    std::string sub = dir + "/" + n;
    std::string md = sub + "/SKILL.md";
    struct stat st{};
    if (::stat(md.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
    SkillMd sk;
    sk.dir = sub;
    if (parse_skill_md(md, sk)) {
      if (sk.name.empty()) sk.name = n;  // fallback=目录名
      if (sk.name.size() > 64) sk.name.resize(64);
      std::lock_guard<std::mutex> lk(g_skills_mu);
      g_skills[sk.name] = std::move(sk);
      ++count;
    }
  }
  ::closedir(d);
  return count;
}

nlohmann::json handle_load_skill(const nlohmann::json& params) {
  // null/非对象入参防御（LLM 工具调用参数畸形时不崩）
  if (!params.is_object())
    return {{"success", false}, {"error", "params_must_be_object"}};
  std::string name = params.value("name", "");
  if (name.empty()) {
    // 索引（渐进披露第一层）
    nlohmann::json idx = nlohmann::json::array();
    std::lock_guard<std::mutex> lk(g_skills_mu);  // v0.53.56
    for (const auto& [k, sk] : g_skills)
      idx.push_back({{"name", k},
                     {"description", sk.description.substr(0, 200)},
                     {"dir", sk.dir}});
    return {{"success", true},
            {"count", g_skills.size()},
            {"skills", idx},
            {"hint", "load_skill(name=...) 读取对应技能手册全文"}};
  }
  std::lock_guard<std::mutex> lk(g_skills_mu);  // v0.53.56
  auto it = g_skills.find(name);
  if (it == g_skills.end())
    return {{"success", false}, {"error", "skill_not_found"},
            {"name", name}, {"available", g_skills.size()}};
  const auto& sk = it->second;
  return {{"success", true}, {"name", sk.name},
          {"description", sk.description}, {"dir", sk.dir},
          {"content", sk.body}};
}

/// 扫描根目录解析（供测试注入）：home 参数显式化
static std::string g_scan_root;

int reload(const std::string& root) {
  g_scan_root = root;
  return scan_skills(root);
}

}  // namespace skill_loader
}  // namespace thin_agent

extern "C" const char* thin_agent_plugin_init2(
    thin_agent::SkillRegistry& registry, thin_agent::PluginContext& ctx) {
  using namespace thin_agent::skill_loader;
  // 扫描根：ctx.config("data_dir") 由核心注入（与 cron/data 插件同协议）；
  // 兜底 ~（THIN_AGENT_HOME 优先）
  // skills 根=~/.thin_agent/skills（THIN_AGENT_HOME 优先，profile 模式随 profile）——
  // 与 data/config 平级的用户可放置目录。RuntimePaths header-only 同源。
  std::string home = thin_agent::default_home_dir();
  scan_skills(home);
  registry.register_cpp_handler("load_skill", handle_load_skill);
  return "skill_loader";
}
