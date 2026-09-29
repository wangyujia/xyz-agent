#pragma once

#include <string>
#include <vector>

namespace thin_agent {

class SkillRegistry;
class PluginContext;  // v0.53.0

namespace plugin {

/// 单个插件加载结果
struct PluginLoadResult {
  std::string name;
  bool ok = false;
  std::string error;
};

/// 从指定目录加载插件
///
/// 扫描 root_dir/{mode}/*.so，对每个 .so 调用:
///   dlopen → dlsym("thin_agent_plugin_init") → init(registry)
///
/// @param registry   Skill 注册表，传给每个插件
/// @param root_dir   插件根目录 (如 "~/.thin_agent/plugins")
/// @param modes      要加载的模式列表 (如 {"common", "dev"})
/// @return           每个插件的加载结果
std::vector<PluginLoadResult> load_plugins(
    SkillRegistry& registry,
    const std::string& root_dir,
    const std::vector<std::string>& modes,
    PluginContext* ctx = nullptr)  // v0.53.0: v2 注入
;

}  // namespace plugin
}  // namespace thin_agent
