// v0.53.50: 插件静态指针悬垂防护——系统性回归
// 背景:插件静态指针(g_ctx/g_board/g_bus/g_facts×4)在多 AgentService
// 场景(前实例析构后)悬垂——R35 cron SegFault 实锤,R38/R50 全面收口。
// 验证:PluginContext 构造/析构周期内 is_alive 的翻转驱动各插件的
// 回落逻辑(不直接调插件内部静态——验证公共 API 语义):
//   ①活 ctx 注入锚→is_alive true ②析构→false ③悬垂校验路径可用
#include "test_macros.h"
#include "thin_agent/plugin/PluginContext.h"
#include <memory>
int main() {
  using thin_agent::PluginContext;
  struct Anchor {
    PluginContext* p = nullptr;
  } a;
  {
    auto ctx = std::make_unique<PluginContext>();
    a.p = ctx.get();
    ASSERT_TRUE("注入锚存活", PluginContext::is_alive(a.p));
    ctx.reset();  // 宿主析构(多实例场景的前实例)
  }
  ASSERT_TRUE("宿主析构后锚失效(悬垂可检)", !PluginContext::is_alive(a.p));
  ASSERT_TRUE("失效锚+null 防御", !PluginContext::is_alive(nullptr));
  return TEST_REPORT();
}
