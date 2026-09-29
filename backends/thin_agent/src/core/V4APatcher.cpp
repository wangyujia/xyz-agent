#include "thin_agent/core/V4APatcher.h"

#include "thin_agent/core/PathValidator.h"  // v0.49.1: 补路径校验旁路

#include <cstring>
#include <fstream>
#include <sstream>

namespace thin_agent {

namespace {

// 去掉行尾 \r\n / \n
std::string strip_eol(const std::string& s) {
  std::string r = s;
  while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.pop_back();
  return r;
}

}  // anonymous namespace

bool V4APatcher::parse(const std::string& patch_text,
                       std::vector<FilePatch>& out) const {
  out.clear();
  std::istringstream ss(patch_text);
  std::string line;
  bool in_patch = false;
  FilePatch current;
  bool have_current = false;

  auto flush_current = [&]() {
    if (have_current) {
      out.push_back(std::move(current));
      current = FilePatch{};
      have_current = false;
    }
  };

  while (std::getline(ss, line)) {
    std::string trimmed = strip_eol(line);
    if (!in_patch) {
      if (trimmed.find("*** Begin Patch") != std::string::npos) {
        in_patch = true;
      }
      continue;
    }
    // 结束标记
    if (trimmed.find("*** End Patch") != std::string::npos) {
      flush_current();
      in_patch = false;
      break;
    }
    // 新文件块
    if (trimmed.find("*** Update File:") != std::string::npos) {
      flush_current();
      current.path = trimmed.substr(std::strlen("*** Update File:"));
      // 去掉可能的前导空格
      while (!current.path.empty() && current.path.front() == ' ') {
        current.path.erase(current.path.begin());
      }
      have_current = true;
      continue;
    }
    if (!have_current) continue;  // 文件块外的行忽略

    // 新 hunk：@@ ... @@
    if (trimmed.rfind("@@", 0) == 0 && trimmed.find("@@", 2) != std::string::npos) {
      current.hunks.push_back(Hunk{});
      current.hunks.back().context_hint = trimmed;
      continue;
    }
    // 内容行：无 hunk 时自动创建（兼容无 @@ 块的 V4A）
    if (current.hunks.empty()) {
      current.hunks.push_back(Hunk{});
    }

    Hunk& h = current.hunks.back();
    if (!trimmed.empty() && trimmed.front() == '-') {
      h.old_text += trimmed.substr(1) + "\n";
    } else if (!trimmed.empty() && trimmed.front() == '+') {
      h.new_text += trimmed.substr(1) + "\n";
    } else {
      // 上下文行：同时出现在 old 和 new
      h.old_text += trimmed + "\n";
      h.new_text += trimmed + "\n";
    }
  }

  if (in_patch) flush_current();  // 没有 End Patch 时也收尾
  return !out.empty();
}

bool V4APatcher::apply_file(std::string& content, const FilePatch& fp) const {
  // 从后往前应用 hunk，避免偏移漂移
  static const FuzzyPatcher patcher;
  for (auto it = fp.hunks.rbegin(); it != fp.hunks.rend(); ++it) {
    if (it->old_text.empty()) {
      // 纯新增 hunk：无删除行。定位用上一行上下文不可靠，跳过并报错
      return false;
    }
    auto res = patcher.patch(content, it->old_text, it->new_text, false);
    if (!res.success) return false;
  }
  return true;
}

V4APatcher::Result V4APatcher::apply_to_disk(const std::string& patch_text) const {
  Result result;
  std::vector<FilePatch> files;
  if (!parse(patch_text, files)) {
    result.errors.push_back("parse_failed: no valid *** Update File blocks");
    return result;
  }

  // 阶段 1: 读取 + 内存中应用（不写盘）
  struct Prepared {
    std::string path;
    std::string new_content;
  };
  std::vector<Prepared> prepared;
  for (const auto& fp : files) {
    // v0.49.1: 路径安全校验（patch = 读+写，与 safe_patch_file 同策略）
    auto& tls = ProjectContextTLS::current();
    PathValidator::Result vr;
    if (tls.ctx.valid) {
      vr = PathValidator::validate_project(fp.path, PathValidator::Op::Write, tls.ctx);
    } else {
      vr = PathValidator::validate_core(fp.path, PathValidator::Op::Write);
    }
    if (!vr.allowed) {
      result.errors.push_back("path_blocked(" + std::string(vr.reason) + "): " + fp.path);
      continue;
    }

    std::ifstream in(vr.normalized_path);
    if (!in.is_open()) {
      result.errors.push_back("cannot_open: " + fp.path);
      continue;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    in.close();

    if (!apply_file(content, fp)) {
      result.errors.push_back("apply_failed: " + fp.path);
      continue;
    }
    prepared.push_back({vr.normalized_path, std::move(content)});
  }

  // 阶段 2: 全部成功才写盘（原子语义）
  if (result.errors.empty() && !prepared.empty()) {
    bool write_ok = true;
    for (const auto& p : prepared) {
      std::ofstream out(p.path, std::ios::trunc);
      if (!out.is_open()) {
        result.errors.push_back("cannot_write: " + p.path);
        write_ok = false;
        break;
      }
      out << p.new_content;
    }
    if (write_ok) {
      result.ok = true;
      for (const auto& p : prepared) result.applied_files.push_back(p.path);
    } else {
      // 写盘失败（理论上已写的无法回滚，但这是极少数磁盘错误场景）
      result.ok = false;
    }
  }
  return result;
}

}  // namespace thin_agent
