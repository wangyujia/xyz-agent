#pragma once

#include <string>
#include <vector>

#include "thin_agent/local/ILocalModel.h"

namespace thin_agent {
namespace local {

/// 基于 ONNX Runtime 的本地对话模型。
///
/// 需要 ONNX 格式的 decoder-only 模型（如 SmolLM-135M 的 ONNX 导出）。
/// 注意：ONNX Runtime 本身不包含 tokenizer，需外部提供分词。
///
/// 当前实现：tokenizer 使用简单的字符级切分（仅用于概念验证）。
/// 生产使用建议用 llama.cpp GGUF 替代，或集成 sentencepiece tokenizer。
class OnnxChatModel : public ILocalModel {
 public:
  /// @param model_path  ONNX 模型文件路径
  /// @param caps        能力列表
  /// @param n_threads   推理线程数
  OnnxChatModel(const std::string& model_path,
                std::vector<ModelCapability> caps,
                int n_threads = 2);

  ~OnnxChatModel() override;

  std::string name() const override { return name_; }
  std::string backend() const override { return "onnx"; }
  std::vector<ModelCapability> capabilities() const override { return caps_; }

  std::string infer(const std::string& input,
                    int max_tokens = 256,
                    const std::string& capability = "chat") override;

  bool load() override;
  bool is_loaded() const override { return loaded_; }
  size_t memory_bytes() const override { return memory_bytes_; }

 private:
  std::vector<int64_t> tokenize(const std::string& text);
  std::string detokenize(const std::vector<int64_t>& tokens);

  std::string name_;
  std::string model_path_;
  std::vector<ModelCapability> caps_;
  int n_threads_;
  bool loaded_ = false;
  size_t memory_bytes_ = 0;
  void* session_ = nullptr;  ///< Ort::Session*
};

}  // namespace local
}  // namespace thin_agent
