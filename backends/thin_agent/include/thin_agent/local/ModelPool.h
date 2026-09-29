#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "thin_agent/local/ILocalModel.h"

namespace thin_agent {
namespace local {

/// 级联推理结果。
struct CascadeResult {
  bool ok = false;
  std::string output;              ///< 最终输出文本
  std::string model_used;          ///< 实际使用的模型名
  int tier = -1;                   ///< 命中层级（0=最快）
  int64_t latency_us = 0;          ///< 总延迟（微秒）
  std::vector<std::string> tried;  ///< 尝试过的模型列表
};

/// 本地模型池：注册、发现、路由、级联、并行推理。
///
/// 用法：
///   ModelPool::instance().add(std::make_unique<GgufModel>("models/qwen.gguf", ...));
///   auto model = ModelPool::instance().best("chat", 500);
///   auto result = ModelPool::instance().cascade("chat", "你好");
class ModelPool {
 public:
  static ModelPool& instance();

  /// 注册/移除模型。
  void add(std::unique_ptr<ILocalModel> model);
  void remove(const std::string& name);

  /// 查找所有支持指定能力的模型（按 priority 升序）。
  std::vector<ILocalModel*> match(const std::string& capability);

  /// 返回最佳匹配模型（已加载 + priority 最低 + 延迟预算内）。
  /// @param capability       所需能力
  /// @param max_latency_us   最大延迟预算（微秒），0 不限；负值表示不限制延迟
  ILocalModel* best(const std::string& capability,
                    int64_t max_latency_us = 0);

  /// 级联推理：按 priority 逐个尝试，第一个成功即返回。
  /// 所有模型都失败 → 返回空。
  CascadeResult cascade(const std::string& capability,
                        const std::string& input,
                        int max_tokens = 256);

  /// 流式级联推理：按 priority 逐个尝试，第一个成功模型逐 token 回调。
  /// @param on_token  逐 token 回调（done=true 时结束，模型名通过 CascadeResult 返回）
  /// @return 级联结果（ok/模型名/tried/latency），output 字段为空（内容通过回调发送）
  CascadeResult cascade_stream(const std::string& capability,
                               const std::string& input,
                               TokenCallback on_token,
                               int max_tokens = 256);

  /// 并行推理：多个模型同时跑（适合 embed + translate 并发）。
  /// 返回 [(模型名, 输出), ...]。
  std::vector<std::pair<std::string, std::string>>
  parallel(const std::vector<std::string>& capabilities,
           const std::string& input,
           int max_tokens = 256);

  /// 热切换模型：卸载当前指定模型，加载新的 GGUF 文件。
  /// @param name          目标模型名称 (如 "qwen2.5-0.5b-instruct-q2_k")
  /// @param new_gguf_path 新 GGUF 文件路径
  /// @return 是否成功
  bool reload(const std::string& name, const std::string& new_gguf_path);

  /// 获取/设置默认 capability（cascade 不指定时使用）。
  std::string default_capability() const { return default_cap_; }
  void set_default_capability(const std::string& cap) { default_cap_ = cap; }

  /// 统计信息。
  size_t size() const;
  size_t total_memory_bytes() const;
  std::vector<nlohmann::json> status_all() const;

 private:
  ModelPool() = default;
  ModelPool(const ModelPool&) = delete;
  ModelPool& operator=(const ModelPool&) = delete;

  std::vector<std::unique_ptr<ILocalModel>> models_;
  std::string default_cap_ = "chat";
  mutable std::mutex mu_;
};

}  // namespace local
}  // namespace thin_agent
