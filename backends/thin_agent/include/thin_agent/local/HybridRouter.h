#pragma once

#include <string>
#include <string_view>

#include "thin_agent/local/ModelPool.h"

namespace thin_agent {
namespace local {

/// 任务复杂度分级。
enum class TaskComplexity {
  simple,    ///< 问候 / 简单问答 / 短句
  moderate,  ///< 普通对话 / 自然语言查询
  complex    ///< 长文本 / 代码 / 多步指令
};

/// 混合路由：编排多层本地推理，按复杂度自适应选择模型层级。
///
/// v0.10.1: 增加基于任务复杂度的自适应路由——
///   simple    → 只用 TemplateModel（0ms, 0MB）
///   moderate  → 用最佳匹配模型（一般 Qwen-0.5B）
///   complex   → 全级联（Template → Qwen → Gemma → 云）
///
/// 用法：
///   HybridRouter router;
///   auto result = router.route("chat", "今天适合穿什么", 256);
///   if (result.ok) { ... result.output ... }
class HybridRouter {
 public:
  struct Config {
    bool enable_cascade = true;
    bool enable_adaptive = true;       ///< v0.10.1: 启用复杂度自适应
    int cascade_max_tokens = 256;
    int simple_max_tokens = 128;       ///< 简单任务 tokens 上限
    int moderate_max_tokens = 256;     ///< 中等任务 tokens 上限
    int complex_max_tokens = 512;      ///< 复杂任务 tokens 上限
    std::string default_capability = "chat";
  };

  HybridRouter();
  explicit HybridRouter(Config cfg);

  /// 分类任务复杂度（启发式）。
  static TaskComplexity classify_complexity(const std::string& input);

  /// 路由推理：模板 → 模型池级联 → 兜底。
  CascadeResult route(const std::string& capability,
                     const std::string& input,
                     int max_tokens = 256);

  /// 流式路由推理。
  CascadeResult route_stream(const std::string& capability,
                             const std::string& input,
                             TokenCallback on_token,
                             int max_tokens = 256);

  /// 便捷方法：仅 chat（自适应）。
  std::string chat(const std::string& input, int max_tokens = 256);

  /// 获取配置（用于测试）。
  const Config& config() const { return cfg_; }

 private:
  /// 按复杂度自适应路由。
  CascadeResult adaptive_route(const std::string& capability,
                               const std::string& input,
                               int max_tokens,
                               TokenCallback on_token = nullptr);

  Config cfg_;
};

}  // namespace local
}  // namespace thin_agent
