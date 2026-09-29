// v0.53.76: 流式 usage 尾帧无 choices 防御回归
// 背景:parse_sse_stream 对 chunk["choices"] 裸 const operator[]——
// 缺失键=断言 Abort(实测 core dumped);usage 尾帧
// (stream_options.include_usage)是 OpenAI 兼容流标准帧,一来就崩
#include <cstdio>
#include <string>
#include "thin_agent/llm/CloudLlmClient.h"
using thin_agent::CloudLlmClient;
int main() {
  // 模拟:正常 delta 帧 + usage 尾帧(无 choices)
  std::string sse =
      "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"!\"}}]}\n"
      "data: {\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2,\"total_tokens\":12}}\n"
      "data: [DONE]\n";
  auto r = CloudLlmClient::parse_sse_stream(sse);
  printf("content=%s usage_total=%d\n", r.text.c_str(), r.usage.total_tokens);
  if (r.text == "hi!" && r.usage.total_tokens == 12) {
    printf("PASS: usage 尾帧不崩+内容/用量双正确\n");
    return 0;
  }
  printf("FAIL\n");
  return 1;
}
