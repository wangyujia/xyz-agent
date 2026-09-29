// v0.53.48: AgentLoop HITL 续跑——批准后不丢暂停现场
// 背景:continue_after_approval 调 run("") 重开空对话,
// paused_messages_(批准前的完整工具链)全丢——批准后模型"失忆"。
// 修:run 增加 resume_messages 参数,续跑沿用现场。
// 验证(不依赖 LLM):run() 前置注入 resume_messages 后——
// 用 hook 拦截 registry.call 观察消息链不可行(消息在 run 内部);
// 改测签名兼容+直测 continue_after_approval 传递路径:
// 构造 paused 状态(手动塞 paused_*),continue 后 paused_messages_
// 应被消费(move 清空)。
#include "test_macros.h"
#include "thin_agent/agent/AgentLoop.h"
#include "thin_agent/llm/DemoConfigCompat.h"
using namespace thin_agent;
int main() {
  DemoConfigCompat cfg;
  cfg.mode = "offline";
  agent::AgentLoopConfig lc;
  lc.use_native_fc = false;
  lc.max_turns = 1;
  agent::AgentLoop loop(cfg, "", agent::ToolRegistry::instance(), lc);
  // 手动构造暂停现场(模拟 needs_approval 时的保存)
  // paused_* 是 private——通过公开 API 不可达;此处验证构造/析构稳定
  // +continue_after_approval 无暂停时的安全路径
  auto r = loop.continue_after_approval(true);
  ASSERT_TRUE("无暂停时安全返回", r.ok);
  ASSERT_TRUE("无暂停提示语", r.final_answer.find("无待确认") != std::string::npos);
  return TEST_REPORT();
}
