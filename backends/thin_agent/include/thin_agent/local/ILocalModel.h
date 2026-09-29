#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace thin_agent {
namespace local {

/// 流式输出回调：每次生成一个 token 时调用。
/// @param token  当前 token 文本（可能为空，表示无新 token）
/// @param done   是否已完成（最后调用，token 可能为最终文本）
using TokenCallback = std::function<void(const std::string& token, bool done)>;

/// 模型能力标签。
struct ModelCapability {
  std::string name;           ///< "intent", "chat", "embed", "translate", "code", ...
  int priority = 0;           ///< 越小越优先（同 capability 内排序）
  int max_input_tokens = 512; ///< 该能力下的最大输入 token 数
  int64_t typical_latency_us = 0; ///< 典型推理延迟（微秒），0=即时
};

/// 本地模型统一推理接口。
/// 所有后端（模板/ONNX/GGUF/HTTP）实现此接口。
class ILocalModel {
 public:
  virtual ~ILocalModel() = default;

  /// 模型名称（唯一标识）。
  virtual std::string name() const = 0;

  /// 后端类型："template", "onnx", "gguf", "http"。
  virtual std::string backend() const = 0;

  /// 该模型支持的能力列表。
  virtual std::vector<ModelCapability> capabilities() const = 0;

  /// 推理：输入文本 → 输出文本。
  /// @param input      输入文本
  /// @param max_tokens 最大输出 token 数
  /// @param capability 调用方声明所需能力（模型可据此调整行为）
  virtual std::string infer(const std::string& input,
                            int max_tokens = 256,
                            const std::string& capability = "chat") = 0;

  /// 流式推理：逐 token 回调。默认实现回退到 infer() 一次性返回。
  /// @param on_token   每次生成的 token 回调（done=true 时表示结束）
  virtual void infer_stream(const std::string& input,
                            TokenCallback on_token,
                            int max_tokens = 256,
                            const std::string& capability = "chat") {
    std::string result = infer(input, max_tokens, capability);
    on_token(result, true);
  }

  /// 加载模型到内存。
  virtual bool load() = 0;

  /// 卸载模型释放内存（默认无操作，GgufModel 等重量级模型需要实现）。
  virtual void unload() {}

  /// 是否已加载。
  virtual bool is_loaded() const = 0;

  /// 内存占用（字节）。
  virtual size_t memory_bytes() const = 0;

  /// 典型推理延迟（微秒），0 表示即时。ModelPool::best() 用此值过滤。
  virtual int64_t typical_latency_us() const { return 0; }

  /// 状态摘要。
  virtual nlohmann::json status() const {
    return {{"name", name()},
            {"backend", backend()},
            {"loaded", is_loaded()},
            {"memory_bytes", memory_bytes()},
            {"latency_us", typical_latency_us()}};
  }
};

}  // namespace local
}  // namespace thin_agent
