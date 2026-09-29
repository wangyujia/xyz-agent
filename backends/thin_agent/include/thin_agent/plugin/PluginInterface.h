#pragma once

/// thin_agent 插件接口
///
/// 每个插件 .so 必须导出:
///   const char* thin_agent_plugin_init(SkillRegistry& registry);
/// 返回值: 插件名 (成功) 或 nullptr (失败)
///
/// 示例:
///   extern "C" const char* thin_agent_plugin_init(SkillRegistry& reg) {
///     reg.register_cpp_handler("code_read_file", ...);
///     return "code_dev";
///   }
///
/// 插件查找规则:
///   ~/.thin_agent/plugins/{mode}/*.so
///   - common/    始终加载
///   - dev/       --dev 模式加载
///   - embedded/  --embedded 模式加载
///   - server/    --server 模式加载

#include "thin_agent/core/SkillRegistry.h"

namespace thin_agent {
namespace plugin {

/// 插件初始化函数签名
using PluginInitFn = const char* (*)(SkillRegistry& registry);

/// 插件入口函数名 (dlsym 查找)
inline constexpr const char* kPluginInitSymbol = "thin_agent_plugin_init";

/// 默认插件根目录
inline constexpr const char* kDefaultPluginDir = "~/.thin_agent/plugins";

}  // namespace plugin
}  // namespace thin_agent
