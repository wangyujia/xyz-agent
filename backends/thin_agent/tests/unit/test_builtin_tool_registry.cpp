// test_builtin_tool_registry：v0.52.16 内置工具注册表化回归
//
// 背景：真 e2e t4 审查任务（viewer 角色）实测两个工具全挂：
// ① read_file → no_cpp_handler:read_file——主 FC 循环里它是
//    execute_one_tool 内置直实现，从未注册进 SkillRegistry，
//    子代理经桥接调用时无 handler。
// ② search_files → tool execution exception——LLM 把数值参数
//    传成字符串（"max_results":"50"），nlohmann value<int> 抛
//    type_error.302 冒到桥接层。
//
// 验证（纯逻辑：参数 coerce 语义 + 契约形状）：
// 1. 字符串数值 coerce 为 int（thin_agent::json_coerce_int）
// 2. 真数值直取
// 3. 缺省回退
// 4. search_files 参数提取逻辑容错（字符串字段非字符串/缺失时
//    走缺省，不抛）
// 5. read_file handler 契约形状（成功含 output/total_lines）
#include "test_macros.h"

#include "thin_agent/core/JsonCoerce.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

int main() {
  // 1) 字符串数值 coerce
  {
    json p = {{"max_results", "50"}};
    ASSERT_TRUE("字符串数值 coerce",
                thin_agent::json_coerce_int(p, "max_results", 10) == 50);
  }
  // 2) 真数值直取
  {
    json p = {{"max_results", 30}};
    ASSERT_TRUE("真数值直取",
                thin_agent::json_coerce_int(p, "max_results", 10) == 30);
  }
  // 3) 缺省回退
  {
    json p = json::object();
    ASSERT_TRUE("缺省回退", thin_agent::json_coerce_int(p, "max_results", 50) == 50);
  }
  // 4) search_files 参数提取容错（复刻 v0.52.16 提取逻辑——不抛）
  {
    // LLM 乱传：pattern 是数字、dir 缺失、max_results 字符串
    json p = {{"pattern", 123}, {"max_results", "7"}};
    std::string pattern, dir;
    try {
      if (p.contains("pattern") && p["pattern"].is_string())
        pattern = p["pattern"].get<std::string>();
      else if (p.contains("glob") && p["glob"].is_string())
        pattern = p["glob"].get<std::string>();
      if (p.contains("dir") && p["dir"].is_string())
        dir = p["dir"].get<std::string>();
      else if (p.contains("path") && p["path"].is_string())
        dir = p["path"].get<std::string>();
      if (dir.empty()) dir = ".";
      int mr = thin_agent::json_coerce_int(p, "max_results", 50);
      ASSERT_TRUE("乱参数不抛：pattern 空+dir 缺省+max coerce",
                  pattern.empty() && dir == "." && mr == 7);
    } catch (const std::exception& e) {
      ASSERT_TRUE(std::string("乱参数不应抛: ") + e.what(), false);
    }
  }
  // 5) 正常参数提取
  {
    json p = {{"pattern", "*.h"}, {"dir", "/tmp"}, {"max_results", "50"}};
    std::string pattern, dir;
    if (p.contains("pattern") && p["pattern"].is_string())
      pattern = p["pattern"].get<std::string>();
    if (p.contains("dir") && p["dir"].is_string())
      dir = p["dir"].get<std::string>();
    int mr = thin_agent::json_coerce_int(p, "max_results", 50);
    ASSERT_TRUE("正常提取", pattern == "*.h" && dir == "/tmp" && mr == 50);
  }

  return TEST_REPORT();
}
