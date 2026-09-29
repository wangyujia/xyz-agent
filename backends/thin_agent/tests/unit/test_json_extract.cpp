// unit_json_extract：v0.52.3 LLM 输出 JSON 提取回归
//
// e2e 实锤形态全覆盖：markdown fence 包裹、前导说明、尾随说明文字
// （旧 find('{')/rfind('}') 截出非法片段的元凶）、字符串内花括号、
// 多候选段（首个可解析含 tasks 的胜出）、无目标键。
#include <string>

#include "thin_agent/core/JsonExtract.h"

#include "test_macros.h"

using namespace thin_agent;

int main() {
  const std::string ok_tasks = R"({"tasks":[{"task_id":"t1","name":"A"}]})";

  // 纯 JSON
  auto j = extract_json_object_with_array(ok_tasks, "tasks");
  ASSERT_TRUE("纯 JSON 提取", !j.is_null() && j["tasks"].size() == 1);

  // markdown fence
  j = extract_json_object_with_array("```json\n" + ok_tasks + "\n```", "tasks");
  ASSERT_TRUE("fence 包裹", !j.is_null() && j["tasks"].size() == 1);

  // 前导说明
  j = extract_json_object_with_array("好的，分解如下：\n" + ok_tasks, "tasks");
  ASSERT_TRUE("前导说明", !j.is_null());

  // 尾随说明（旧实现 rfind('}') 元凶场景）
  j = extract_json_object_with_array(
      ok_tasks + "\n\n以上就是任务分解，共 1 个子任务。", "tasks");
  ASSERT_TRUE("尾随说明（旧法必挂）", !j.is_null() && j["tasks"].size() == 1);

  // 字符串值内花括号（不干扰平衡扫描）
  j = extract_json_object_with_array(
      R"(前言 {"tasks":[{"task_id":"a}b","name":"含}花括号"}]} 尾文)", "tasks");
  ASSERT_TRUE("字符串内花括号", !j.is_null());
  ASSERT_EQ("转义花括号保真",
            j["tasks"][0]["task_id"].get<std::string>(), std::string("a}b"));

  // 多段候选：第一段不含 tasks、第二段含——第二段胜出
  j = extract_json_object_with_array(
      R"(备注 {"note":"x"} 分解 {"tasks":[{"task_id":"t1"}]})", "tasks");
  ASSERT_TRUE("跳过无关段取目标段", !j.is_null() && j["tasks"].size() == 1);

  // 无目标键 → null
  j = extract_json_object_with_array(R"({"foo":[1,2]})", "tasks");
  ASSERT_TRUE("无目标键返回 null", j.is_null());

  // 空串/无花括号
  ASSERT_TRUE("空串", extract_json_object_with_array("", "tasks").is_null());
  ASSERT_TRUE("无花括号",
              extract_json_object_with_array("plain text", "tasks").is_null());

  return TEST_REPORT();
}
