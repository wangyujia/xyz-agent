#pragma once
// test_macros.h — thin_agent 单元测试共享断言宏（v0.50.6）
//
// 背景：50 个测试文件三代断言风格并存——
//   ① 宏族（ASSERT_TRUE/ASSERT_EQ + static g_failures + PASS/FAIL 行输出）
//   ② expect() 函数族（cerr 输出 + g_failures 计数）
//   ③ 裸 assert()（cassert）
// v0.50.6 只统一 ①（语义完全相同的重复宏定义），② ③ 风格完整保持不动。
// 新测试文件应 #include 本头使用宏族。
//
// 用法：
//   #include "test_macros.h"
//   int main() {
//     ASSERT_TRUE("name", cond);
//     ASSERT_EQ("name", got, want);
//     return TEST_REPORT();   // 0=全过, 1=有失败
//   }

#include <iostream>

static int g_failures = 0;  // 每个测试 TU 独立副本

#define ASSERT_TRUE(name, cond)                                        \
  do {                                                                 \
    if (cond) {                                                        \
      std::cout << "PASS: " << name << "\n";                           \
    } else {                                                           \
      std::cout << "FAIL: " << name << " (line " << __LINE__ << ")\n"; \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

#define ASSERT_EQ(name, got, want)                                     \
  do {                                                                 \
    if ((got) == (want)) {                                             \
      std::cout << "PASS: " << name << "\n";                           \
    } else {                                                           \
      std::cout << "FAIL: " << name << " got=[" << (got) << "] want=[" \
                << (want) << "] (line " << __LINE__ << ")\n";          \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

/// 汇总返回：main 的 return。输出失败计数（>0 时非零退出码）。
inline int TEST_REPORT() {
  if (g_failures > 0) {
    std::cout << g_failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "ALL PASS\n";
  return 0;
}
