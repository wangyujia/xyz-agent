#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace thin_agent {

/// 模糊匹配结果。
struct FuzzyMatch {
  bool found = false;
  size_t pos = 0;          // 原始内容中的匹配起始位置
  size_t match_length = 0; // 原始内容中实际匹配的长度（可能 ≠ old_string.size()）
  size_t match_count = 0;  // 匹配次数（>1 表示不唯一）
  int strategy_index = 0;  // 命中策略的索引
  std::string strategy_name;
};

/// 文本预处理器：将文本归一化为更易匹配的形式。
/// 同时构建位置映射表，用于反查原始位置。
struct NormalizedText {
  std::string text;                   // 归一化后的文本
  std::vector<size_t> pos_map;        // pos_map[normalized_pos] = original_pos
};

/// 归一化策略函数签名。
using Normalizer = NormalizedText (*)(const std::string& original);

/// FuzzyPatcher：分层模糊匹配引擎。
///
/// 用法：
///   FuzzyPatcher fp;
///   auto match = fp.find(content, old_string);
///   if (match.found) {
///       content.replace(match.pos, old_string.size(), new_string);
///   }
///
/// 策略管道（按顺序尝试，命中即停）：
///   1. 精确匹配
///   2. 去所有行首空白
///   3. 去所有行尾空白
///   4. 去头尾空行
///   5. 去行首空白 + 去头尾空行
class FuzzyPatcher {
public:
  FuzzyPatcher();

  /// 在 content 中查找 old_string，支持模糊匹配。
  /// @param replace_all  若 false，要求 old_string 在原文本中唯一匹配
  FuzzyMatch find(const std::string& content,
                  const std::string& old_string,
                  bool replace_all = false) const;

  /// 执行替换：先 find，再替换。一站式接口。
  /// @return {success, output, replacements, strategy_used}
  struct PatchResult {
    bool success = false;
    std::string output;       // 人类可读的描述
    int replacements = 0;
    std::string strategy;
  };

  PatchResult patch(std::string& content,
                    const std::string& old_string,
                    const std::string& new_string,
                    bool replace_all = false) const;

private:
  /// 注册一条策略。
  void add_strategy(const char* name, Normalizer normalizer);

  struct Strategy {
    const char* name;
    Normalizer normalize;
  };
  std::vector<Strategy> strategies_;
};

// ── 内置归一化函数 ──

/// 恒等变换（精确匹配）。
NormalizedText normalize_identity(const std::string& s);

/// 去掉每行的前导空白（空格/Tab）。
NormalizedText normalize_strip_leading(const std::string& s);

/// 去掉每行的尾部空白。
NormalizedText normalize_strip_trailing(const std::string& s);

/// 去掉文本首尾的空行。
NormalizedText normalize_strip_blank_lines(const std::string& s);

/// 去行首空白 + 去头尾空行（组合）。
NormalizedText normalize_strip_leading_and_blanks(const std::string& s);

}  // namespace thin_agent
