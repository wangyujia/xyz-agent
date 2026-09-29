// v0.53.4: PluginContext 存活注册表——插件后台线程（cron ticker / monitor
// 轮询等）持有 ctx 裸指针，宿主 AgentService 析构后指针悬垂。虚调用
// alive() 在对象亡后本身即 UB，故用进程级注册表：
//   ctx 构造 → 登记；ctx 析构 → 除名；ticker 每轮回调前 probe(g_ctx)。
// map 生命周期覆盖全进程，悬垂指针只做【键查询】不解引用。

#include "thin_agent/plugin/PluginContext.h"

#include <mutex>
#include <unordered_set>

namespace thin_agent {

namespace {
std::mutex g_ctx_mu;
std::unordered_set<const void*> g_live_ctxs;
}  // namespace

PluginContext::PluginContext() { register_self(); }
PluginContext::~PluginContext() { unregister_self(); }

void PluginContext::register_self() const {
  std::lock_guard<std::mutex> lk(g_ctx_mu);
  g_live_ctxs.insert(static_cast<const void*>(this));
}

void PluginContext::unregister_self() const {
  std::lock_guard<std::mutex> lk(g_ctx_mu);
  g_live_ctxs.erase(static_cast<const void*>(this));
}

bool PluginContext::probe(const void* ctx) {
  std::lock_guard<std::mutex> lk(g_ctx_mu);
  return g_live_ctxs.count(ctx) > 0;
}

bool PluginContext::alive() const { return probe(static_cast<const void*>(this)); }

// v0.53.47: 静态版——悬垂指针安全(仅键查询)
bool PluginContext::is_alive(const PluginContext* ctx) {
  return ctx != nullptr && probe(static_cast<const void*>(ctx));
}

}  // namespace thin_agent
