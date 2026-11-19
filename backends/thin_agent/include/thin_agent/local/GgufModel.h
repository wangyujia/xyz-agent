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
  /// @param n_ctx       上下文长度；<=0 表示按模型训练窗口自动（llama n_ctx_train）
  /// @param n_threads   推理线程数
  GgufModel(const std::string& model_path,
            std::vector<ModelCapability> caps,
            int n_ctx = 0,
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

  /// 多轮对话推理（role/content；无 system 时注入默认人设）。
  /// 用于离线聊天携带 session 历史，避免每轮只看当前一句。
  using ChatTurn = std::pair<std::string, std::string>;
  std::string infer_turns(const std::vector<ChatTurn>& turns,
                          int max_tokens = 256);
  void infer_stream_turns(const std::vector<ChatTurn>& turns,
                          TokenCallback on_token,
                          int max_tokens = 256);

  bool load() override;
  void unload() override;
  bool is_loaded() const override { return loaded_; }
  size_t memory_bytes() const override;
  int64_t typical_latency_us() const override { return latency_us_; }
  nlohmann::json status() const override;

  /// 构造时请求的上下文；<=0 表示自动。
  int requested_n_ctx() const { return n_ctx_requested_; }
  /// 实际生效的上下文（加载后才可靠；未加载时可能仍为请求值）。
  int n_ctx() const { return n_ctx_; }
  /// 模型训练窗口（加载后从 GGUF 读取；未加载为 0）。
  int n_ctx_train() const { return n_ctx_train_; }

 private:
  std::string name_;
  std::string model_path_;
  std::vector<ModelCapability> caps_;
  int n_ctx_requested_ = 0;  ///< 用户/调用方请求；<=0 = 跟模型
  int n_ctx_ = 0;            ///< 实际 KV 窗口
  int n_ctx_train_ = 0;      ///< 模型原生能力
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
