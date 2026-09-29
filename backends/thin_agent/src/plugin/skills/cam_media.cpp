// v0.53.0: cam_media 插件 —— 媒体拍录工具（capture_photo/start/stop_recording）。
//
// 从 AgentService::register_media_agent_tools 迁出（核心瘦身第一批）。
// CAM 场景语义：实际拍摄由设备侧 media pipeline 执行，agent 侧只做
// 提交确认（原核心实现即 TaskEngine 占位——media_ok，行为零变化）。
// 非相机部署不装本插件即可。

#include <nlohmann/json.hpp>

#include "thin_agent/core/SkillRegistry.h"
#include "thin_agent/plugin/PluginContext.h"

namespace thin_agent {
namespace cam_media {

nlohmann::json handle_media_ok(const nlohmann::json&) {
  return {{"success", true}, {"output", "ok"}};
}

}  // namespace cam_media
}  // namespace thin_agent

extern "C" const char* thin_agent_plugin_init2(
    thin_agent::SkillRegistry& registry, thin_agent::PluginContext&) {
  registry.register_cpp_handler("capture_photo",
                                thin_agent::cam_media::handle_media_ok);
  registry.register_cpp_handler("start_recording",
                                thin_agent::cam_media::handle_media_ok);
  registry.register_cpp_handler("stop_recording",
                                thin_agent::cam_media::handle_media_ok);
  return "cam_media";
}
