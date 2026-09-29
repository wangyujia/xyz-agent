/// libskill_code_dev.so — 编程技能插件
///
/// 注册的 handler:
///   code_read_file  — 结构化文件读取（带行号、分页）
///   code_patch      — 精准字符串替换
///   code_search     — 结构化代码搜索（文件过滤、格式输出）
///   code_write_file — 结构化文件写入（自动创建目录、返回大小）
///
/// 仅在 --dev 模式下加载。所有 handler 返回统一 JSON 格式:
///   { "success": true/false, "output": "...", "error": "..." }

#include "thin_agent/plugin/PluginInterface.h"
#include "thin_agent/core/FuzzyPatcher.h"
#include "thin_agent/core/V4APatcher.h"
#include "thin_agent/core/SyntaxChecker.h"
#include "thin_agent/core/PathValidator.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#ifndef _WIN32
#include <regex.h>   // v0.49.2: POSIX ERE 预验证（v0.54.16: Windows 无此头，见下方 #ifdef）
#endif
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace thin_agent {
namespace code_dev {

// ── 辅助函数 ──

/// 检查路径是否存在
static bool path_exists(const std::string& path) {
#ifdef _WIN32
  DWORD attr = GetFileAttributesA(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
#endif
}

/// 通配符匹配 (简单实现: * 匹配任意字符)
static bool wildcard_match(const char* pattern, const char* str) {
  if (*pattern == '\0') return *str == '\0';
  if (*pattern == '*')
    return wildcard_match(pattern + 1, str) ||
           (*str && wildcard_match(pattern, str + 1));
  if (*str == '\0') return false;
  if (*pattern == '?' || *pattern == *str)
    return wildcard_match(pattern + 1, str + 1);
  return false;
}

/// 列出目录下匹配 glob 的文件
static std::vector<std::string> list_files(const std::string& dir,
                                            const std::string& glob_pattern) {
  std::vector<std::string> result;
  // 提取纯文件名 glob (去掉路径)
  std::string name_glob = glob_pattern;
  auto slash = glob_pattern.find_last_of("/\\");
  if (slash != std::string::npos) name_glob = glob_pattern.substr(slash + 1);

#ifdef _WIN32
  std::string search = dir + "\\*";
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(search.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return result;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      if (glob_pattern.empty() || wildcard_match(name_glob.c_str(), fd.cFileName))
        result.push_back(dir + "\\" + fd.cFileName);
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = ::opendir(dir.c_str());
  if (!d) return result;
  struct dirent* ent;
  while ((ent = ::readdir(d)) != nullptr) {
    if (ent->d_type != DT_REG) continue;
    if (!glob_pattern.empty() &&
        !wildcard_match(name_glob.c_str(), ent->d_name))
      continue;
    result.push_back(dir + "/" + ent->d_name);
  }
  ::closedir(d);
#endif
  return result;
}

// ── Handler: code_read_file ──
// params: path (必需), offset (行号, 默认 1), limit (行数, 默认 500)
// 返回: { success, output (带行号文本), total_lines, start_line, end_line }

nlohmann::json handle_read_file(const nlohmann::json& params) {
  std::string path = params.value("path", "");
  if (path.empty()) return {{"success", false}, {"error", "path_required"}};

  int offset = 1;
  int limit = 500;
  if (params.contains("offset") && params["offset"].is_number())
    offset = params["offset"].get<int>();
  if (params.contains("limit") && params["limit"].is_number())
    limit = params["limit"].get<int>();

  auto rr = safe_read_file_paged(path, offset, limit);
  if (!rr.success) {
    return {{"success", false}, {"error", rr.error}};
  }
  return {
    {"success", true},
    {"output", rr.output},
    {"total_lines", rr.total_lines},
    {"start_line", rr.start_line},
    {"end_line", rr.end_line}
  };
}

// ── Handler: code_patch ──
// params: path (必需), old_string (必需), new_string (必需), replace_all (可选)
// 返回: { success, output (unified diff 风格摘要), replacements }

nlohmann::json handle_patch(const nlohmann::json& params) {
  std::string path = params.value("path", "");
  std::string old_str = params.value("old_string", "");
  std::string new_str = params.value("new_string", "");
  bool replace_all = params.value("replace_all", false);

  if (path.empty()) return {{"success", false}, {"error", "path_required"}};
  if (old_str.empty()) return {{"success", false}, {"error", "old_string_required"}};

  auto pr = safe_patch_file(path, old_str, new_str, replace_all);
  if (!pr.success) {
    return {{"success", false}, {"error", pr.error}};
  }

  nlohmann::json resp = {
    {"success", true},
    {"output", pr.output},
    {"replacements", pr.replacements},
    {"strategy", pr.strategy}
  };
  if (!pr.syntax_error.empty()) resp["syntax_check"] = pr.syntax_error;
  return resp;
}

// ── Handler: code_search (v0.54.34: 纯 C++ 遍历，去掉 grep/popen) ──
// params:
//   pattern    (必需) — 搜索的正则表达式
//   dir        (可选, 默认 ".") — 搜索目录
//   file_types (可选, 默认 "*") — 逗号分隔的 glob，如 "*.cpp,*.h,*.rs"
//                亦接受 file_glob（单测/兼容别名）
//   limit      (可选, 默认 50) — 最大结果数
//   output_mode (可选, 默认 "content") — "content" / "files_only" / "count"
//   context    (可选, 默认 0) — 匹配行上下文的行数
//
// 返回: { success, output, match_count, [total_files] }

// v0.49.2: POSIX ERE 正则预验证（code_search 用）
// 用 regcomp 编译一次即弃；编译失败 = 语法错误
static bool regex_is_valid_ere(const std::string& pattern) {
#ifdef _WIN32
  // v0.54.16: Windows 无 POSIX `regex.h`（此前无条件 include ⇒ 该插件在 Windows 侧编不过）。
  // 用同目录已 include 的 `std::regex` 做**语法预验证**。语义差异如实记录：ECMAScript 文法比
  // POSIX ERE 更宽（例如支持 \d），因此只会"更宽松"——不会把合法 ERE 判成非法，最坏是让个别
  // 非法 ERE 通过预验证，由下游匹配路径兜住。
  try {
    std::regex re(pattern);
    (void)re;
    return true;
  } catch (const std::regex_error&) {
    return false;
  }
#else
  regex_t re;
  if (regcomp(&re, pattern.c_str(), REG_EXTENDED | REG_NOSUB) != 0) {
    return false;
  }
  regfree(&re);
  return true;
#endif
}

/// 文件名是否匹配任一 glob（空或 "*" = 全匹配）
static bool file_matches_types(const std::string& filename,
                               const std::vector<std::string>& globs) {
  if (globs.empty()) return true;
  for (const auto& g : globs) {
    if (g.empty() || g == "*") return true;
    if (wildcard_match(g.c_str(), filename.c_str())) return true;
  }
  return false;
}

nlohmann::json handle_search(const nlohmann::json& params) {
  std::string pattern = params.value("pattern", "");
  std::string dir = params.value("dir", ".");
  // file_types 优先；兼容单测/旧调用的 file_glob
  std::string file_types = params.value("file_types", "");
  if (file_types.empty() && params.contains("file_glob") &&
      params["file_glob"].is_string()) {
    file_types = params["file_glob"].get<std::string>();
  }
  if (file_types.empty()) file_types = "*";

  int limit = 50;
  if (params.contains("limit") && params["limit"].is_number())
    limit = params["limit"].get<int>();
  else if (params.contains("limit") && params["limit"].is_string()) {
    try { limit = std::stoi(params["limit"].get<std::string>()); } catch (...) {}
  }
  if (limit < 1) limit = 1;
  if (limit > 500) limit = 500;

  std::string output_mode = params.value("output_mode", "content");
  int context = 0;
  if (params.contains("context") && params["context"].is_number())
    context = params["context"].get<int>();
  if (context < 0) context = 0;
  if (context > 5) context = 5;

  if (pattern.empty())
    return {{"success", false}, {"error", "pattern_required"}};
  if (dir.empty()) dir = ".";
  // 纵深：路径不得含 shell 元字符（即使误拼进 shell 也不易注入）
  if (dir.find_first_of(";|&`$<>") != std::string::npos) {
    return {{"success", false}, {"error", "invalid_dir_chars"}};
  }

  // 非法正则硬拒绝（与旧 grep 路径语义一致；不回退字面量）
  if (!regex_is_valid_ere(pattern)) {
    return {{"success", false}, {"error", "bad_regex"}};
  }

  std::regex re;
  try {
    re = std::regex(pattern, std::regex::ECMAScript);
  } catch (const std::regex_error&) {
    return {{"success", false}, {"error", "bad_regex"}};
  }

  std::vector<std::string> globs;
  {
    std::istringstream ss(file_types);
    std::string type;
    while (std::getline(ss, type, ',')) {
      size_t s = type.find_first_not_of(" \t\r\n");
      if (s != std::string::npos) type = type.substr(s);
      size_t e = type.find_last_not_of(" \t\r\n");
      if (e != std::string::npos) type = type.substr(0, e + 1);
      auto slash = type.find_last_of("/\\");
      if (slash != std::string::npos) type = type.substr(slash + 1);
      if (!type.empty()) globs.push_back(type);
    }
  }

  try {
    std::ostringstream out;
    int match_count = 0;
    int files_with_hits = 0;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
      return {{"success", false}, {"error", "search_failed:" + ec.message()}};
    }

    for (; it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
      if (ec) { ec.clear(); continue; }
      if (it.depth() > 16) continue;
      std::error_code sec;
      auto st = it->symlink_status(sec);
      if (sec || !std::filesystem::is_regular_file(st)) continue;

      const auto& path = it->path();
      if (!file_matches_types(path.filename().string(), globs)) continue;

      std::ifstream in(path);
      if (!in) continue;

      // 读入行（单文件上限防炸）
      std::vector<std::string> lines;
      lines.reserve(256);
      {
        std::string line;
        while (std::getline(in, line)) {
          if (line.size() > 8192) line = line.substr(0, 8192) + "...";
          lines.push_back(std::move(line));
          if (lines.size() > 100000) break;
        }
      }

      std::vector<int> hit_idx;
      for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        try {
          if (std::regex_search(lines[i], re)) hit_idx.push_back(i);
        } catch (...) {}
      }
      if (hit_idx.empty()) continue;

      ++files_with_hits;
      if (output_mode == "files_only") {
        out << path.string() << "\n";
        ++match_count;
        if (match_count >= limit) break;
        continue;
      }
      if (output_mode == "count") {
        match_count += static_cast<int>(hit_idx.size());
        continue;
      }

      // content：按命中行输出，可选 ±context
      std::unordered_set<int> emitted;
      for (int hi : hit_idx) {
        int lo = std::max(0, hi - context);
        int hi_end = std::min(static_cast<int>(lines.size()) - 1, hi + context);
        for (int i = lo; i <= hi_end; ++i) {
          if (emitted.count(i)) continue;
          emitted.insert(i);
          out << path.string() << ":" << (i + 1) << ":" << lines[i] << "\n";
        }
        ++match_count;
        if (match_count >= limit) break;
      }
      if (match_count >= limit) break;
    }

    std::string result = out.str();
    if (output_mode == "count") {
      result = "total:" + std::to_string(match_count) + "\n";
    }

    nlohmann::json r;
    r["success"] = true;
    r["output"] = result;
    r["match_count"] = match_count;
    if (files_with_hits > 0) r["total_files"] = files_with_hits;
    if (!file_types.empty() && file_types != "*")
      r["file_types"] = file_types;
    return r;

  } catch (const std::exception& e) {
    return {{"success", false}, {"error", std::string("search_error:") + e.what()}};
  }
}

// ── Handler: code_write_file ──
// params: path (必需), content (必需)
// 返回: { success, output, bytes_written, path }

nlohmann::json handle_write_file(const nlohmann::json& params) {
  std::string path = params.value("path", "");
  std::string content = params.value("content", "");

  if (path.empty())
    return {{"success", false}, {"error", "path_required"}};
  if (content.empty())
    return {{"success", false}, {"error", "content_required"}};

  auto wr = safe_write_file(path, content, {.verify = true, .syntax_check = true});
  if (!wr.success) {
    return {{"success", false}, {"error", wr.error}};
  }

  nlohmann::json resp = {
    {"success", true},
    {"output", "Wrote " + std::to_string(wr.bytes_written) + " bytes to " + wr.normalized_path},
    {"bytes_written", static_cast<int>(wr.bytes_written)},
    {"path", wr.normalized_path}
  };
  if (!wr.syntax_error.empty()) resp["syntax_check"] = wr.syntax_error;
  return resp;
}

// ── Handler: execute_code (v0.40.3) ──
// params: code (必需, Python 脚本源码), timeout_sec (默认 30)
// 返回: { success, exit_code, stdout, stderr, truncated }
// 
// 让模型能批量调用工具：写一段 Python 脚本，循环/条件调用
// read_file/write_file/patch/grep 等，大幅减少 FC 往返次数。

nlohmann::json handle_execute_code(const nlohmann::json& params) {
  std::string code = params.value("code", "");
  int timeout_sec = params.value("timeout_sec", 30);
  if (timeout_sec < 1) timeout_sec = 1;
  if (timeout_sec > 120) timeout_sec = 120;

  if (code.empty())
    return {{"success", false}, {"error", "code_required"}};

  // v0.53.40: 语言=类而非特例(设计原则:白名单=解释器这个类,新语言只改
  // 词表不动代码)。python 继续注入 thin_tools;bash/sh/node 直执行。
  std::string language = params.value("language", "python");
  static const std::map<std::string, std::string> kInterpreters = {
      {"python", "python3"}, {"python3", "python3"},
      {"bash", "bash"}, {"sh", "sh"},
      {"node", "node"}, {"javascript", "node"}};
  auto interp_it = kInterpreters.find(language);
  if (interp_it == kInterpreters.end()) {
    return {{"success", false},
            {"error", "unsupported_language: " + language +
                          " (supported: python/bash/sh/node)"}};
  }
  const std::string interpreter = interp_it->second;

  // v0.41.0: 将 thin_tools 模块写入临时目录供 execute_code 脚本调用
  auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
  std::string tmp_dir = "/tmp/thin_exec_" + std::to_string(ts) + "/";
  std::string thin_tools_path = tmp_dir + "thin_tools.py";
  const char* ext = (interpreter == "node") ? ".js" : ".py";
  std::string tmp_path = tmp_dir + "script" + ext;

  // 创建临时目录
  std::string mkdir_cmd = "mkdir -p " + tmp_dir;
  ::system(mkdir_cmd.c_str());

  // 写入 thin_tools 模块
  {
    std::ofstream ofs(thin_tools_path);
    if (ofs) {
      ofs << R"PY(# thin_tools — execute_code 辅助库 (v0.41.0)
# 在脚本中 from thin_tools import read_file, write_file, search, shell, patch
import os, re, subprocess

def read_file(path, offset=1, limit=500):
    \"\"\"读文件 (行号, 分页)\"\"\"
    if not os.path.isfile(path): return {"success": False, "error": "not_found"}
    with open(path) as f:
        lines = f.readlines()
    total = len(lines)
    end = min(offset + limit - 1, total)
    out = ""
    for i in range(offset - 1, min(end, total)):
        out += f"{i+1}|{lines[i].rstrip()}\\n"
    return {"success": True, "output": out, "total_lines": total, "start_line": offset, "end_line": end}

def write_file(path, content):
    \"\"\"写文件 (自动创建目录)\"\"\"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(content)
    written = os.path.getsize(path)
    return {"success": True, "output": f"Wrote {written} bytes to {path}", "bytes_written": written, "path": path}

def search(pattern, dir=".", file_types="*", limit=50, output_mode="content", context=0):
    \"\"\"搜索文件 (grep 后端)\"\"\"
    cmd = ["grep", "-rn"]
    if file_types and file_types != "*":
        for ft in file_types.split(","):
            ft = ft.strip()
            if ft: cmd.extend(["--include", ft])
    if output_mode == "files_only": cmd.append("-l")
    elif output_mode == "count": cmd.append("-c")
    if context > 0: cmd.extend(["-C", str(context)])
    cmd.append(pattern); cmd.append(dir)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        out = r.stdout
        if limit > 0:
            out = "\\n".join(out.split("\\n")[:limit])
        mc = out.count("\\n")
        return {"success": True, "output": out, "match_count": mc}
    except Exception as e:
        return {"success": False, "error": str(e)}

def shell(command, timeout=30):
    \"\"\"执行 shell 命令\"\"\"
    try:
        r = subprocess.run(command, shell=True, capture_output=True,
                           text=True, timeout=timeout)
        out = r.stdout + r.stderr
        if len(out) > 30000: out = out[:30000] + "\\n...(truncated)"
        return {"success": r.returncode == 0, "exit_code": r.returncode, "stdout": out}
    except subprocess.TimeoutExpired:
        return {"success": False, "error": f"timeout after {timeout}s"}
    except Exception as e:
        return {"success": False, "error": str(e)}

def patch(path, old_string, new_string, replace_all=False):
    \"\"\"字符串替换 (基于 difflib 的模糊匹配)\"\"\"
    if not os.path.isfile(path):
        return {\"success\": False, \"error\": \"not_found\"}
    with open(path) as f: content = f.read()

    # 1) 精确匹配
    idx = content.find(old_string)
    if idx == -1:
        # 2) difflib 模糊匹配 (容忍缩进/空白差异)
        from difflib import SequenceMatcher as SM
        sm = SM(None, old_string, content)
        sm.ratio()  # 计算匹配矩阵
        m = sm.find_longest_match(0, len(old_string), 0, len(content))
        # m.size 占 old_string 的比例超过 80% 就认为是同一段
        r = m.size / max(len(old_string), 1)
        if m.size <= 0 or r < 0.7:
            return {\"success\": False, \"error\": \"not_found\"}
        # 用匹配到的原文做替换
        actual_old = content[m.b:m.b + m.size]
        content = content[:m.b] + new_string + content[m.b + m.size:]
        with open(path, 'w') as f: f.write(content)
        return {\"success\": True, \"output\": f\"fuzzy matched (ratio={r:.2f}), replaced 1 occurrence in {path}\",
                \"replacements\": 1, \"fuzzy_ratio\": round(r, 2)}
    count = 0
    if replace_all:
        content = content.replace(old_string, new_string)
        count = content.count(new_string)
    else:
        content = content[:idx] + new_string + content[idx + len(old_string):]
        count = 1
    with open(path, 'w') as f: f.write(content)
    return {\"success\": True, \"output\": f\"replaced {count} occurrence(s) in {path}\", \"replacements\": count}
)PY";
    }
  }

  // 写入用户脚本，前置导入 thin_tools
  {
    std::ofstream ofs(tmp_path);
    if (!ofs) {
      std::remove(thin_tools_path.c_str());
      return {{"success", false}, {"error", "cannot_create_temp_file"}};
    }
    if (interpreter == "python3")  // v0.53.40: thin_tools 注入仅 python
      ofs << "import sys; sys.path.insert(0, '" << tmp_dir << "')\n";
    ofs << code;
    ofs.close();
  }

  // 执行
  std::string cmd = "timeout " + std::to_string(timeout_sec) +
                    " " + interpreter + " " + tmp_path + " 2>&1";
  std::array<char, 32768> buffer;
  std::string output;
  int exit_code = -1;
  nlohmann::json rj;
  rj["success"] = false;

  auto pipe = popen(cmd.c_str(), "r");
  if (pipe) {
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
      output += buffer.data();
    exit_code = pclose(pipe);
    if (exit_code == -1) exit_code = 1;
  }

  // 全部清理
  std::remove(tmp_path.c_str());
  std::remove(thin_tools_path.c_str());
  std::remove(tmp_dir.c_str());  // 如果为空

  bool truncated = output.size() > 30000;
  if (truncated) output = output.substr(0, 30000) + "\n...(truncated)";

  rj["exit_code"] = exit_code;
  rj["stdout"] = output;
  rj["success"] = (exit_code == 0);
  if (truncated) rj["truncated"] = true;

  return rj;
}

// ── Handler: code_patch_v4a (v0.45.0: V4A 多文件 patch) ──
// params: patch (必需) — V4A 格式文本
//   *** Begin Patch
//   *** Update File: path/to/file
//   @@ hint @@
//   -removed
//   +added
//   *** End Patch
// 返回: { success, applied_files[], errors[] }

nlohmann::json handle_patch_v4a(const nlohmann::json& params) {
  std::string patch_text = params.value("patch", "");
  if (patch_text.empty())
    return {{"success", false}, {"error", "patch_required"}};

  static const thin_agent::V4APatcher v4a;
  auto result = v4a.apply_to_disk(patch_text);

  nlohmann::json resp = {
    {"success", result.ok},
    {"applied_files", result.applied_files},
    {"errors", result.errors},
  };
  if (!result.errors.empty()) resp["error"] = result.errors.front();
  return resp;
}

}  // namespace code_dev
}  // namespace thin_agent

// ── 插件入口 ──

extern "C" const char* thin_agent_plugin_init(
    thin_agent::SkillRegistry& registry) {

  registry.register_cpp_handler("code_read_file", thin_agent::code_dev::handle_read_file);
  registry.register_cpp_handler("code_patch",     thin_agent::code_dev::handle_patch);
  registry.register_cpp_handler("code_patch_v4a", thin_agent::code_dev::handle_patch_v4a);
  registry.register_cpp_handler("code_search",    thin_agent::code_dev::handle_search);
  registry.register_cpp_handler("code_write_file", thin_agent::code_dev::handle_write_file);
  registry.register_cpp_handler("execute_code",   thin_agent::code_dev::handle_execute_code);  // v0.40.3

  return "code_dev";
}
