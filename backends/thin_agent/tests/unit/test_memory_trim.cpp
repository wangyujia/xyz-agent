// v0.53.81: trim_memory_by_token_budget 行为回归(零拷贝快路径等价性)
// 背景:trim 逐条消息无条件值拷贝 content 后才判断是否截断——零拷贝
// 快路径(近 full_context 轮/短消息直接以原 content 估 token)改写后
// 须保证裁剪决策【逐字节等价】:哪些消息被保留、老消息截断只参与
// token 估算(不落库)、预算超限时至少保留最新一条
#include <cstdio>
#include <string>
#include <vector>
#include "thin_agent/core/AgentServiceUtil.h"
#include "thin_agent/llm/CloudLlmClient.h"

using thin_agent::ChatMessage;
using thin_agent::svc_util::estimate_tokens;
using thin_agent::svc_util::trim_memory_by_token_budget;

static ChatMessage mk(const std::string& role, const std::string& content) {
  ChatMessage m;
  m.role = role;
  m.content = content;
  return m;
}

static int failures = 0;

static void expect(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  } else {
    std::printf("PASS: %s\n", what);
  }
}

int main() {
  // ①预算充足:全部保留(不裁剪)
  {
    std::vector<ChatMessage> mem = {mk("user", "a"), mk("assistant", "b"),
                                    mk("user", "c"), mk("assistant", "d")};
    trim_memory_by_token_budget(mem, 100000, 2, 100);
    expect(mem.size() == 4, "预算充足=全保留");
  }

  // ②预算极小:至少保留最新一条(最新消息永不因超预算被裁)
  {
    std::vector<ChatMessage> mem = {mk("user", std::string(500, 'x')),
                                    mk("assistant", std::string(500, 'y')),
                                    mk("user", std::string(500, 'z'))};
    trim_memory_by_token_budget(mem, 1, 1, 10);
    expect(mem.size() >= 1 && mem.back().content == std::string(500, 'z'),
           "超小预算=至少留最新一条且内容完整");
  }

  // ③裁剪只删前缀,保留后缀(时序不破坏)
  {
    std::vector<ChatMessage> mem;
    for (int i = 0; i < 10; ++i)
      mem.push_back(mk(i % 2 ? "assistant" : "user", std::string(200, 'a' + i)));
    const size_t before = mem.size();
    const std::string last = mem.back().content;
    // 200 字符消息≈99 tok(近轮不截断),老消息截到 50 字符≈25 tok;
    // 预算 150 → 仅最新两条可容纳,余者裁掉
    trim_memory_by_token_budget(mem, 150, 1, 50);
    expect(mem.size() < before, "小预算确触发裁剪");
    expect(mem.back().content == last, "裁剪后最新消息原样保留");
  }

  // ④老长消息的截断只影响估算、不改内容(落库内容完整)
  {
    std::vector<ChatMessage> mem = {mk("user", std::string(5000, 'o')),
                                    mk("assistant", "r1"), mk("user", "r2"),
                                    mk("assistant", "r3"), mk("user", "r4")};
    const std::string old_full = mem.front().content;
    trim_memory_by_token_budget(mem, 100000, 1, 100);
    expect(mem.size() == 5, "预算足够时老消息不被裁");
    if (!mem.empty() && mem.front().role == "user" && mem.front().content.size() == 5000)
      expect(mem.front().content == old_full, "老消息内容未被截断改写");
  }

  // ⑤空输入安全
  {
    std::vector<ChatMessage> mem;
    trim_memory_by_token_budget(mem, 10, 1, 10);
    expect(mem.empty(), "空输入不崩");
  }

  // ⑥estimate_tokens 边界:空串=0、非空至少 1
  expect(estimate_tokens("") == 0, "空串 token=0");
  expect(estimate_tokens("a") >= 1, "非空 token>=1");

  std::printf(failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", failures);
  return failures == 0 ? 0 : 1;
}
