// unit_json_coerce：v0.52.1 工具参数宽容取值回归
//
// 根因（e2e chat-26 实测）：GLM 传 {"limit":"5"}（字符串数字），
// nlohmann value<int>() 抛 type_error.302 → session_recent 报
// Execution error → FC 收敛追踪误判"错误率 62%"终止任务。
// 收口：12+ 处 .value("limit", N) 统一走 json_coerce_int。

#include <cassert>
#include <cstdio>
#include <string>

#include "nlohmann/json.hpp"
#include "thin_agent/core/JsonCoerce.h"

#include "test_macros.h"

using nlohmann::json;
using thin_agent::json_coerce_int;

int main() {
  // 正常 int
  ASSERT_EQ("int 直取", json_coerce_int(json{{"limit", 5}}, "limit", 20), 5);
  // 字符串数字（本次 bug 的直接形态）
  ASSERT_EQ("字符串数字", json_coerce_int(json{{"limit", "5"}}, "limit", 20), 5);
  // 缺 key → 默认
  ASSERT_EQ("缺 key 默认", json_coerce_int(json{{"x", 1}}, "limit", 20), 20);
  // 空对象 → 默认
  ASSERT_EQ("空对象默认", json_coerce_int(json::object(), "limit", 20), 20);
  // 非数字字符串 → 默认（不抛）
  ASSERT_EQ("垃圾字符串默认", json_coerce_int(json{{"limit", "abc"}}, "limit", 20), 20);
  // 负数字符串
  ASSERT_EQ("负数字符串", json_coerce_int(json{{"limit", "-3"}}, "limit", 20), -3);
  // 无符号
  ASSERT_EQ("unsigned", json_coerce_int(json{{"limit", 7u}}, "limit", 20), 7);
  // 浮点（截断不抛）
  ASSERT_EQ("浮点不抛", json_coerce_int(json{{"limit", 5.9}}, "limit", 20), 5);
  // null → 默认
  ASSERT_EQ("null 默认", json_coerce_int(json{{"limit", nullptr}}, "limit", 20), 20);
  // bool → 默认
  ASSERT_EQ("bool 默认", json_coerce_int(json{{"limit", true}}, "limit", 20), 20);
  // 非 object 容器 → 默认（不抛）
  ASSERT_EQ("数组容器默认", json_coerce_int(json::array({1, 2}), "limit", 20), 20);

  // ── v0.53.2: utf8_safe_truncate（生产崩溃修复：substr 切半中文）──
  // 短串原样
  ASSERT_EQ("短串原样", utf8_safe_truncate("hello", 10), "hello");
  // 纯 ASCII 精确截断
  ASSERT_EQ("ASCII 截断", utf8_safe_truncate("abcdef", 3), "abc");
  // 中文（3 字节/字）：9 字节 = 3 个字完整
  ASSERT_EQ("中文完整", utf8_safe_truncate("一二三", 9), "一二三");
  // 截 7 字节：2 个字（6 字节）+第 3 字放不下 → 回退到 6
  ASSERT_EQ("中文切半回退", utf8_safe_truncate("一二三", 7), "一二");
  // 截 4 字节：1 个字
  ASSERT_EQ("中文单字", utf8_safe_truncate("一二三", 4), "一");
  // 混合：ab+中文
  ASSERT_EQ("混合回退", utf8_safe_truncate("ab一二三", 4), "ab");
  ASSERT_EQ("混合恰好", utf8_safe_truncate("ab一二三", 5), "ab一");
  // 关键回归：截断结果可被 nlohmann strict dump 接受（不再 type_error.316）
  {
    std::string long_cn;
    for (int i = 0; i < 100; ++i) long_cn += "循环检测终止";
    std::string cut = utf8_safe_truncate(long_cn, 200);
    json j{{"preview", cut}};
    bool ok = true;
    try { j.dump(); } catch (...) { ok = false; }
    ASSERT_TRUE("截断结果 strict dump 不抛", ok);
  }

  return TEST_REPORT();
}
