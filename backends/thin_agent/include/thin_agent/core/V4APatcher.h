#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "thin_agent/core/FuzzyPatcher.h"

namespace thin_agent {

/// v0.45.0: V4A 多文件 patch（对齐 Hermes V4A 格式）。
///
/// 格式：
/// ```text
/// *** Begin Patch
/// *** Update File: src/foo.cpp
/// @@ 上下文提示 @@
///  context line
/// -removed line
/// +added line
/// *** Update File: src/bar.h
/// -old
/// +new
/// *** End Patch
/// ```
///
/// - 每个 `*** Update File:` 块包含若干 hunk；
/// - hunk 内无前缀行为上下文，`-` 行为删除，`+` 行为新增；
/// - 每块的 old_text = 上下文+删除行，new_text = 上下文+新增行，
///   交给 FuzzyPatcher 做模糊匹配定位；
/// - 多文件整体原子应用：任一文件失败则全部不写盘。
class V4APatcher {
 public:
  struct Hunk {
    std::string context_hint;  // @@ 行内容
    std::string old_text;      // 上下文 + 删除行
    std::string new_text;      // 上下文 + 新增行
  };

  struct FilePatch {
    std::string path;
    std::vector<Hunk> hunks;
  };

  struct Result {
    bool ok{false};
    std::vector<std::string> applied_files;  // 成功应用的文件
    std::vector<std::string> errors;         // 失败原因（逐文件/逐 hunk）
  };

  /// 解析 V4A 文本 → 文件块列表。格式错误返回 false。
  bool parse(const std::string& patch_text, std::vector<FilePatch>& out) const;

  /// 应用整个 patch 到磁盘（原子：全部成功才写盘）。
  Result apply_to_disk(const std::string& patch_text) const;

  /// 应用单个文件块的 hunks 到 content（从后往前，偏移不漂移）。
  /// 返回 false 表示某个 hunk 匹配失败（content 不变）。
  bool apply_file(std::string& content, const FilePatch& fp) const;
};

}  // namespace thin_agent
