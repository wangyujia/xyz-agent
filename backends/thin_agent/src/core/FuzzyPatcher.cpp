#include "thin_agent/core/FuzzyPatcher.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

namespace thin_agent {

// ══════════════════════════════════════════════════════════════════
// 归一化函数实现
// ══════════════════════════════════════════════════════════════════

/// 逐字符构建归一化文本 + 位置映射表。
/// keep(ch): 保留字符 → 写入输出，映射到原始位置
/// skip():   丢弃字符 → 原始位置前进但不写入输出
struct Builder {
  std::string text;
  std::vector<size_t> map;  // map[normalized_pos] = original_pos
  size_t orig = 0;

  void keep(char ch) {
    text += ch;
    map.push_back(orig++);
  }
  void skip() { orig++; }
};

NormalizedText normalize_identity(const std::string& s) {
  NormalizedText r;
  r.text = s;
  r.pos_map.resize(s.size());
  for (size_t i = 0; i < s.size(); ++i) r.pos_map[i] = i;
  return r;
}

NormalizedText normalize_strip_leading(const std::string& s) {
  Builder b;
  bool at_line_start = true;
  for (char ch : s) {
    if (at_line_start && (ch == ' ' || ch == '\t')) {
      b.skip();
    } else {
      b.keep(ch);
      at_line_start = (ch == '\n');
    }
  }
  return {std::move(b.text), std::move(b.map)};
}

NormalizedText normalize_strip_trailing(const std::string& s) {
  // 策略：先扫描找到每行的尾部空白区间，再逐字符构建
  // 标记每个位置是否属于"行尾空白"
  std::vector<bool> is_trailing_ws(s.size(), false);
  for (size_t i = 0; i < s.size();) {
    // 找到本行末尾（下一个 \n 或字符串结尾）
    size_t eol = i;
    while (eol < s.size() && s[eol] != '\n') ++eol;
    // 从行尾往回找非空白字符
    size_t end = eol;
    while (end > i && (s[end - 1] == ' ' || s[end - 1] == '\t'))
      --end;
    // [end, eol) 是尾部空白
    for (size_t j = end; j < eol; ++j) is_trailing_ws[j] = true;
    i = eol + 1;  // 跳过 \n
  }

  Builder b;
  for (size_t i = 0; i < s.size(); ++i) {
    if (is_trailing_ws[i]) {
      b.skip();
    } else {
      b.keep(s[i]);
    }
  }
  return {std::move(b.text), std::move(b.map)};
}

NormalizedText normalize_strip_blank_lines(const std::string& s) {
  // 找第一个和最后一个非空行
  // 空行 = 只含空格/Tab/\r 的行（包括完全空行）
  auto is_blank_line = [](const std::string& line) {
    return line.find_first_not_of(" \t\r") == std::string::npos;
  };

  // 拆成行（保留换行符信息）
  std::vector<std::pair<std::string, bool>> lines;  // (content, has_newline)
  std::string cur;
  for (char ch : s) {
    if (ch == '\n') {
      lines.push_back({cur, true});
      cur.clear();
    } else {
      cur += ch;
    }
  }
  if (!cur.empty() || (!s.empty() && s.back() == '\n')) {
    // 最后一行：如果 s 以 \n 结尾，cur 为空但有一行
    lines.push_back({cur, s.back() == '\n'});
  }

  if (lines.empty()) return {{}, {}};

  // 找 first_non_blank 和 last_non_blank
  size_t first = 0;
  while (first < lines.size() && is_blank_line(lines[first].first))
    ++first;

  size_t last = lines.size();
  while (last > first && is_blank_line(lines[last - 1].first))
    --last;

  if (first >= last) {
    // 全是空行
    Builder b;
    for (size_t i = 0; i < s.size(); ++i) b.skip();
    return {std::move(b.text), std::move(b.map)};
  }

  // 构建输出：跳过 first 之前的行，保留 [first, last)
  Builder b;
  size_t byte_pos = 0;

  for (size_t i = 0; i < first; ++i) {
    // 跳过这一行的所有字符 + 换行符
    for (size_t j = 0; j < lines[i].first.size(); ++j) b.skip();
    if (lines[i].second) b.skip();  // newline
  }

  for (size_t i = first; i < last; ++i) {
    for (char ch : lines[i].first) b.keep(ch);
    if (lines[i].second) b.keep('\n');
  }

  // last 之后的行自动跳过（不写入 b）

  return {std::move(b.text), std::move(b.map)};
}

NormalizedText normalize_strip_leading_and_blanks(const std::string& s) {
  auto n1 = normalize_strip_blank_lines(s);
  auto n2 = normalize_strip_leading(n1.text);

  // 合成 pos_map: pos_map_result[i] = n1.pos_map[n2.pos_map[i]]
  NormalizedText r;
  r.text = std::move(n2.text);
  r.pos_map.reserve(n2.pos_map.size());
  for (size_t mapped : n2.pos_map) {
    r.pos_map.push_back(n1.pos_map[mapped]);
  }
  return r;
}

// v0.40.4: collapse whitespace — 多空格/制表符→单空格
NormalizedText normalize_collapse_whitespace(const std::string& s) {
  Builder b;
  bool in_space = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!in_space) { b.keep(' '); in_space = true; }
    } else {
      b.keep(c); in_space = false;
    }
  }
  return {std::move(b.text), std::move(b.map)};
}

// v0.40.4: common indent — 移除所有行公共缩进
NormalizedText normalize_common_indent(const std::string& s) {
  size_t min_indent = std::string::npos, pos = 0;
  while (pos < s.size()) {
    size_t ls = pos;
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    size_t indent = pos - ls;
    if (pos < s.size() && s[pos] != '\n' && s[pos] != '\r')
      if (indent < min_indent) min_indent = indent;
    while (pos < s.size() && s[pos] != '\n' && s[pos] != '\r') ++pos;
    if (pos < s.size()) ++pos;
  }
  if (min_indent == std::string::npos || min_indent == 0) return {s, {}};

  Builder b;
  pos = 0;
  while (pos < s.size()) {
    size_t ic = 0;
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t') && ic < min_indent) {
      ++pos; ++ic;
    }
    while (pos < s.size() && s[pos] != '\n' && s[pos] != '\r') { b.keep(s[pos]); ++pos; }
    if (pos < s.size()) { b.keep(s[pos]); ++pos; }
  }
  return {std::move(b.text), std::move(b.map)};
}

// v0.41.1: ignore case — 全小写后匹配（大小写容错）
NormalizedText normalize_ignore_case(const std::string& s) {
  Builder b;
  for (size_t i = 0; i < s.size(); ++i) {
    b.keep(static_cast<char>(std::tolower(static_cast<unsigned char>(s[i]))));
  }
  return {std::move(b.text), std::move(b.map)};
}

// v0.41.1: inline whitespace — 行内连续空格→单空格，保留换行
NormalizedText normalize_inline_whitespace(const std::string& s) {
  Builder b;
  bool in_space = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\n' || c == '\r') {
      b.keep(c); in_space = false;
    } else if (c == ' ' || c == '\t') {
      if (!in_space) { b.keep(' '); in_space = true; }
    } else {
      b.keep(c); in_space = false;
    }
  }
  return {std::move(b.text), std::move(b.map)};
}

// ══════════════════════════════════════════════════════════════════
// FuzzyPatcher
// ══════════════════════════════════════════════════════════════════

FuzzyPatcher::FuzzyPatcher() {
  add_strategy("exact", normalize_identity);
  add_strategy("strip_leading", normalize_strip_leading);
  add_strategy("strip_trailing", normalize_strip_trailing);
  add_strategy("strip_blank_lines", normalize_strip_blank_lines);
  add_strategy("strip_leading+blanks", normalize_strip_leading_and_blanks);
  add_strategy("collapse_whitespace", normalize_collapse_whitespace);    // v0.40.4
  add_strategy("common_indent", normalize_common_indent);                // v0.40.4
  add_strategy("ignore_case", normalize_ignore_case);                    // v0.41.1
  add_strategy("inline_whitespace", normalize_inline_whitespace);        // v0.41.1
}

void FuzzyPatcher::add_strategy(const char* name, Normalizer normalizer) {
  strategies_.push_back({name, normalizer});
}

FuzzyMatch FuzzyPatcher::find(const std::string& content,
                              const std::string& old_string,
                              bool replace_all) const {
  // 策略 0（精确匹配）快速路径
  {
    size_t pos = content.find(old_string);
    if (pos != std::string::npos) {
      size_t count = 1;
      if (!replace_all) {
        size_t next = content.find(old_string, pos + old_string.size());
        if (next == std::string::npos) {
          return {true, pos, old_string.size(), 1, 0, "exact"};
        }
        // 不唯一，统计总数并继续尝试模糊策略
        count = 2;
        while ((next = content.find(old_string, next + old_string.size())) !=
               std::string::npos)
          ++count;
      }
      if (replace_all) {
        return {true, pos, old_string.size(), count, 0, "exact"};
      }
      // 不唯一，不返回（继续尝试模糊策略缩小范围）
    }
  }

  // 其他策略按顺序尝试
  for (size_t si = 1; si < strategies_.size(); ++si) {
    const auto& st = strategies_[si];
    auto nc = st.normalize(content);
    auto no = st.normalize(old_string);

    if (no.text.empty()) continue;  // 归一化后为空串，跳过

    size_t npos = nc.text.find(no.text);
    if (npos == std::string::npos) continue;

    if (!replace_all) {
      size_t next = nc.text.find(no.text, npos + no.text.size());
      if (next != std::string::npos) continue;  // 不唯一
    }

    // 映射回原始位置，计算真实匹配跨度
    if (npos >= nc.pos_map.size()) continue;  // 安全检查
    size_t orig_pos = nc.pos_map[npos];
    size_t nend = npos + no.text.size() - 1;
    if (nend >= nc.pos_map.size()) continue;
    size_t orig_end = nc.pos_map[nend];
    size_t match_len = orig_end - orig_pos + 1;
    return {true, orig_pos, match_len, 1, static_cast<int>(si), st.name};
  }

  return {false, 0, 0, 0, -1, ""};
}

FuzzyPatcher::PatchResult FuzzyPatcher::patch(
    std::string& content,
    const std::string& old_string,
    const std::string& new_string,
    bool replace_all) const {
  PatchResult r;

  if (!replace_all) {
    auto m = find(content, old_string, false);
    if (!m.found) {
      r.output = "old_string_not_found (tried " +
                 std::to_string(strategies_.size()) + " strategies)";
      return r;
    }

    // 模糊匹配时，恢复 new_string 的缩进
    std::string indented_new = new_string;
    if (m.strategy_index > 0) {
      // 检测原始内容中匹配区域所在行的前导空白
      size_t line_start = m.pos;
      while (line_start > 0 && content[line_start - 1] != '\n')
        --line_start;
      size_t indent_end = line_start;
      while (indent_end < content.size() &&
             (content[indent_end] == ' ' || content[indent_end] == '\t'))
        ++indent_end;
      std::string indent = content.substr(line_start, indent_end - line_start);

      if (!indent.empty()) {
        // 为 new_string 的每一行添加缩进
        std::string result;
        result.reserve(new_string.size() + indent.size() * 4);
        bool first = true;
        for (size_t i = 0; i < new_string.size();) {
          if (!first) result += indent;
          first = false;
          while (i < new_string.size() && new_string[i] != '\n')
            result += new_string[i++];
          if (i < new_string.size()) result += new_string[i++];  // \n
        }
        indented_new = result;
      }
    }

    content.replace(m.pos, m.match_length, indented_new);
    r.success = true;
    r.replacements = 1;
    r.strategy = m.strategy_name;
    r.output = "Replaced 1 occurrence" +
               (m.strategy_index > 0 ? " via " + m.strategy_name : "");
    return r;
  }

  // replace_all 模式：循环替换
  // 为了效率，先用精确匹配；如果精确匹配失败，再降级
  int count = 0;
  std::string strategy_used = "exact";

  // 先尝试精确匹配 replace_all
  size_t search_from = 0;
  while (true) {
    size_t pos = content.find(old_string, search_from);
    if (pos == std::string::npos) break;
    content.replace(pos, old_string.size(), new_string);
    search_from = pos + new_string.size();
    ++count;
  }

  if (count > 0) {
    r.success = true;
    r.replacements = count;
    r.strategy = "exact";
    r.output = "Replaced " + std::to_string(count) + " occurrence(s)";
    return r;
  }

  // 精确匹配失败，逐个模糊匹配
  search_from = 0;
  while (true) {
    auto m = find(content.substr(search_from), old_string, true);
    if (!m.found) break;
    size_t abs_pos = search_from + m.pos;
    content.replace(abs_pos, m.match_length, new_string);
    search_from = abs_pos + new_string.size();
    strategy_used = m.strategy_name;
    ++count;
  }

  if (count > 0) {
    r.success = true;
    r.replacements = count;
    r.strategy = strategy_used;
    r.output = "Replaced " + std::to_string(count) + " occurrence(s) via " +
               strategy_used;
  } else {
    r.output = "old_string_not_found (tried " +
               std::to_string(strategies_.size()) + " strategies)";
  }

  return r;
}

}  // namespace thin_agent
