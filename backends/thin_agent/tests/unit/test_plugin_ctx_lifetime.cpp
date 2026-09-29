// v0.53.47: PluginContext 悬垂保护——静态 is_alive 回归
// 背景:cron 插件 ticker(静态单例)比宿主 AgentService 活得久,
// g_ctx 悬垂后虚调用 alive() 读亡对象虚表=SegFault(实测 cron.cpp:134)。
// 验证:①实例析构后 is_alive=false ②存活实例 true ③nullptr false
#include "test_macros.h"
#include "thin_agent/plugin/PluginContext.h"
int main() {
  using thin_agent::PluginContext;
  ASSERT_TRUE("nullptr 非活", !PluginContext::is_alive(nullptr));
  {
    PluginContext live;
    ASSERT_TRUE("存活实例", PluginContext::is_alive(&live));
    ASSERT_TRUE("成员 alive 一致", live.alive());
  }
  PluginContext* dead = nullptr;
  {
    PluginContext tmp;
    dead = &tmp;
  }
  ASSERT_TRUE("析构后 is_alive=false(悬垂安全)", !PluginContext::is_alive(dead));
  return TEST_REPORT();
}
