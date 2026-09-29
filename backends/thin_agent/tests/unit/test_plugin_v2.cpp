// test_plugin_v2：v0.53.0 插件 v2 机制回归（PluginContext 注入 + init2 加载）
//
// 覆盖：PluginContext 默认实现（chat 不可用降级）；init2 符号解析路径
//（编译一个最小 v2 插件 .so → dlopen → init2 注册 handler → dispatch
// 生效）；旧插件（仅 init）兼容不受影响。

#include "../../include/thin_agent/core/SkillRegistry.h"
#include "../../include/thin_agent/plugin/PluginContext.h"
#include "../../include/thin_agent/plugin/PluginLoader.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>

using thin_agent::PluginContext;
using thin_agent::SkillRegistry;

static int g_failures = 0;
#define CHECK(cond, msg) \
  do { if (!(cond)) { ++g_failures; std::printf("FAIL: %s\n", msg); } \
       else { std::printf("PASS: %s\n", msg); } } while (0)

// 测试用 v2 插件源码：注册 test_v2_tool，调用 ctx.data_dir
static const char* kPluginSrc = R"CPP(
#include <nlohmann/json.hpp>
#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/plugin/PluginContext.h"
static std::string g_dir;
nlohmann::json handle(const nlohmann::json& p) {
  return {{"success", true}, {"echo", p.value("x", "")}, {"dir", g_dir}};
}
extern "C" const char* thin_agent_plugin_init2(
    thin_agent::SkillRegistry& registry, thin_agent::PluginContext& ctx) {
  g_dir = ctx.data_dir("test_v2");
  registry.register_cpp_handler("test_v2_tool", handle);
  return "test_v2";
}
)CPP";

int main() {
  // 1) PluginContext 默认实现：chat 降级不崩
  {
    PluginContext def;
    auto r = def.chat("s", "hello");
    CHECK(r.value("type", "") == "error", "默认 ctx.chat 降级 error");
    CHECK(def.session_snapshot("s").empty(), "默认 ctx.snapshot 空");
    CHECK(def.data_dir("p") == ".", "默认 ctx.data_dir 退化");
    CHECK(def.version() == 2, "ctx.version=2");
  }

  // 2) 写出最小 v2 插件源码 + 编译 + dlopen + init2 → dispatch
  {
    const std::string dir = "/tmp/test_plugin_v2";
    std::system(("mkdir -p " + dir).c_str());
    {
      FILE* f = fopen((dir + "/plug.cpp").c_str(), "w");
      if (f) { fputs(kPluginSrc, f); fclose(f); }
    }
    std::string inc = "-I/root/code/thin_agent/include";
    std::string cc = "g++ -std=c++17 -fPIC -shared " + inc + " " + dir +
                     "/plug.cpp -o " + dir + "/libplug.so 2>/tmp/tpv2_err.txt";
    if (std::system(cc.c_str()) != 0) {
      std::printf("插件编译失败:\n");
      std::system("cat /tmp/tpv2_err.txt");
    }
    CHECK(access((dir + "/libplug.so").c_str(), F_OK) == 0, "测试插件编译");

    SkillRegistry reg;
    void* h = dlopen((dir + "/libplug.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) { const char* e = dlerror(); std::printf("dlerror: %s\n", e ? e : "?"); }
    CHECK(h != nullptr, "dlopen 测试插件");
    if (h) {
      using Init2Fn = const char* (*)(SkillRegistry&, PluginContext&);
      auto* init2 = reinterpret_cast<Init2Fn>(
          dlsym(h, "thin_agent_plugin_init2"));
      CHECK(init2 != nullptr, "init2 符号存在");
      struct TestCtx : PluginContext {
        std::string data_dir(const std::string& n) const override {
          return "/data/test/" + n;
        }
      } ctx;
      const char* name = init2(reg, ctx);
      CHECK(name && std::string(name) == "test_v2", "init2 返回插件名");
      nlohmann::json args;
      args["x"] = "hello";
      auto r = reg.dispatch_cpp("test_v2_tool", args);
      CHECK(r.value("echo", "") == "hello", "v2 插件工具 dispatch");
      CHECK(r.value("dir", "") == "/data/test/test_v2",
            "ctx.data_dir 注入传递到插件");
      auto r2 = reg.dispatch_cpp("nope", {});
      CHECK(r2.dump().find("no_cpp_handler") != std::string::npos,
            "未知工具 no_cpp_handler");
      // 注意：不 dlclose——SkillRegistry 的 std::function manager 指向
      // 插件代码段，dlclose 后析构=悬垂调用（生产 PluginLoader 同样
      // 只加不卸，进程生命周期=插件生命周期）。
    }
    // 3) load_plugins 旧签名兼容（ctx=nullptr 默认）
    auto results = thin_agent::plugin::load_plugins(
        reg, "/tmp/test_plugin_v2_none", {"x"});
    CHECK(results.empty() || !results[0].ok, "空目录加载不崩");
  }

  std::system("rm -rf /tmp/test_plugin_v2 /tmp/test_plugin_v2_load_root");
  if (g_failures == 0) std::printf("ALL PASS\n");
  return g_failures == 0 ? 0 : 1;
}
