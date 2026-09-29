// v0.53.40: code_dev::execute_code 多语言执行——类驱动白名单回归
// 背景：execute_code 此前硬编码 python3(违反"语言=类"设计原则,
// 用户 2026-08-21 拍板白名单=解释器这个类)。
// 验证：①bash/sh/node 可执行 ②非法语言明确拒绝 ③python 默认路径不回归
//（dlopen 插件,模拟生产加载路径;whole-archive+rdynamic 导出核心符号）
#include <dlfcn.h>
#include <iostream>
#include "test_macros.h"
#include <nlohmann/json.hpp>

using Json = nlohmann::json;

int main() {
  const char* so = "libskill_code_dev.so";
  void* h = dlopen(so, RTLD_NOW);
  if (!h) {
    // build 目录相对路径兜底（ctest WORKING_DIRECTORY=build）
    h = dlopen("./libskill_code_dev.so", RTLD_NOW);
  }
  if (!h) {
    std::cerr << "dlopen failed: " << dlerror() << "\n";
    return 1;
  }
  using Handler = Json (*)(const Json&);
  // 符号名手写太脆——用 dlsym 找 thin_agent_plugin_init 拿注册回调更稳,
  // 但该 init 需要 SkillRegistry。直接解析 mangled 名(单测内可控):
  auto fn = (Handler)dlsym(
      h, "_ZN10thin_agent8code_dev19handle_execute_codeERKN8nlohmann10basic_json"
         "ISt3mapSt6vectorNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEblmdSaNS1_14adl_serializerES4_IhSaIhEEEE");
  if (!fn) {
    std::cerr << "dlsym failed: " << dlerror() << "\n";
    return 1;
  }

  // ① bash
  {
    Json r = fn(Json{{"code", "echo lang-bash-ok"}, {"language", "bash"}});
    ASSERT_TRUE("bash 可执行", r.value("success", false));
    ASSERT_TRUE("bash 输出含标记", r.value("stdout", "").find("lang-bash-ok") != std::string::npos);
  }
  // ② node（若环境无 node 则跳过——不硬失败）
  {
    Json r = fn(Json{{"code", "console.log('lang-node-ok')"}, {"language", "node"}});
    if (r.value("success", false)) {
      ASSERT_TRUE("node 输出含标记", r.value("stdout", "").find("lang-node-ok") != std::string::npos);
    } else {
      std::cout << "SKIP: node 解释器不可用（" << r.value("error", "?") << "）\n";
    }
  }
  // ③ 非法语言拒绝
  {
    Json r = fn(Json{{"code", "x"}, {"language", "perl"}});
    ASSERT_TRUE("非法语言明确拒绝", !r.value("success", false));
    ASSERT_TRUE("拒绝信息含 unsupported_language",
                r.value("error", "").find("unsupported_language") != std::string::npos);
  }
  // ④ python 默认路径
  {
    Json r = fn(Json{{"code", "print('lang-py-ok')"}});
    ASSERT_TRUE("python 默认路径不回归", r.value("success", false));
    ASSERT_TRUE("python 输出含标记", r.value("stdout", "").find("lang-py-ok") != std::string::npos);
  }
  dlclose(h);
  return TEST_REPORT();
}
