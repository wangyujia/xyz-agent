#pragma once

#include <memory>
#include <string>
#include <vector>

#include "thin_agent/local/ILocalModel.h"

// 前向声明 llama.cpp C API
struct llama_model;
struct llama_context;

namespace thin_agent {
namespace local {

/// 基于 llama.cpp 的 GGUF 本地模型。
///
/// 可选编译：CMake 选项 THIN_AGENT_WITH_LLAMA_CPP=ON
/// 依赖：llama.cpp 静态库（libllama.a）+ ggml
///
/// 用法：
///   auto model = std::make_unique<GgufModel>("models/qwen2.5-1.5b-q4.gguf",
///       std::vector<ModelCapability>{{"chat", 1, 4096}});
///   model->load();
///   auto reply = model->infer("你好");
class GgufModel : public ILocalModel {
 public:
  /// @param model_path  GGUF 文件路径
  /// @param caps        能力列表
  /// @param n_ctx       上下文长度（默认 2048）
  /// @param n_threads   推理线程数
  GgufModel(const std::string& model_path,
            std::vector<ModelCapability> caps,
            int n_ctx = 2048,
            int n_threads = 4);

  ~GgufModel() override;

  std::string name() const override { return name_; }
  std::string backend() const override { return "gguf"; }
  std::vector<ModelCapability> capabilities() const override { return caps_; }

  std::string infer(const std::string& input,
                    int max_tokens = 256,
                    const std::string& capability = "chat") override;

  void infer_stream(const std::string& input,
                    TokenCallback on_token,
                    int max_tokens = 256,
                    const std::string& capability = "chat") override;

  bool load() override;
  void unload() override;
  bool is_loaded() const override { return loaded_; }
  size_t memory_bytes() const override;
  int64_t typical_latency_us() const override { return latency_us_; }

 private:
  std::string name_;
  std::string model_path_;
  std::vector<ModelCapability> caps_;
  int n_ctx_;
  int n_threads_;
  bool loaded_ = false;

  // llama.cpp 内部状态
  void* model_ = nullptr;    ///< llama_model*
  void* ctx_ = nullptr;      ///< llama_context*
  void* vocab_ = nullptr;    ///< const llama_vocab*
  void* sampler_ = nullptr;  ///< llama_sampler*
  size_t memory_bytes_ = 0;
  int64_t latency_us_ = 500000;  ///< 默认 500ms 典型延迟
};

}  // namespace local
}  // namespace thin_agent
