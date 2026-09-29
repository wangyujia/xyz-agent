#include "thin_agent/local/OnnxChatModel.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

// ONNX Runtime C API (use project include path)
#include <onnxruntime_c_api.h>

namespace thin_agent {
namespace local {

namespace {

const OrtApi* g_ort = nullptr;

void ensure_ort() {
  if (!g_ort) g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
}

}  // namespace

OnnxChatModel::OnnxChatModel(const std::string& model_path,
                             std::vector<ModelCapability> caps,
                             int n_threads)
    : model_path_(model_path),
      caps_(std::move(caps)),
      n_threads_(n_threads) {
  auto pos = model_path_.find_last_of("/\\");
  name_ = pos != std::string::npos ? model_path_.substr(pos + 1) : model_path_;
  if (auto dot = name_.rfind('.'); dot != std::string::npos)
    name_ = name_.substr(0, dot);
}

OnnxChatModel::~OnnxChatModel() {
  if (session_) {
    ensure_ort();
    g_ort->ReleaseSession(static_cast<OrtSession*>(session_));
  }
}

bool OnnxChatModel::load() {
  if (loaded_) return true;
  if (!std::filesystem::exists(model_path_)) return false;

  ensure_ort();

  OrtEnv* env = nullptr;
  OrtSessionOptions* opts = nullptr;
  OrtSession* sess = nullptr;

  if (g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "onnx_chat", &env) != nullptr)
    return false;
  if (g_ort->CreateSessionOptions(&opts) != nullptr) {
    g_ort->ReleaseEnv(env);
    return false;
  }
  if (g_ort->SetIntraOpNumThreads(opts, n_threads_) != nullptr) {}  // v0.53.16: 消费返回值

  if (g_ort->CreateSession(env, model_path_.c_str(), opts, &sess) != nullptr) {
    g_ort->ReleaseSessionOptions(opts);
    g_ort->ReleaseEnv(env);
    return false;
  }

  session_ = sess;
  // 估算内存（粗略）
  memory_bytes_ = 50 * 1024 * 1024;  // ~50MB placeholder
  loaded_ = true;
  return true;
}

// 简单字符级 tokenizer（仅概念验证，生产应使用 sentencepiece）
std::vector<int64_t> OnnxChatModel::tokenize(const std::string& text) {
  std::vector<int64_t> tokens;
  // 简单映射：每个 UTF-8 字节 → "token id"
  // 真实场景需要 sentencepiece 模型
  for (unsigned char c : text) {
    tokens.push_back(static_cast<int64_t>(c) + 1);  // +1 避免 0 (padding)
  }
  return tokens;
}

std::string OnnxChatModel::detokenize(const std::vector<int64_t>& tokens) {
  std::string out;
  for (int64_t t : tokens) {
    if (t > 0 && t < 256) out.push_back(static_cast<char>(t - 1));
  }
  return out;
}

std::string OnnxChatModel::infer(const std::string& input,
                                  int max_tokens,
                                  const std::string& /*capability*/) {
  if (!loaded_ && !load()) return "";
  if (input.empty()) return "";

  // 概念验证：ONNX 聊天推理需要特定模型格式（decoder + past KV cache）
  // 当前返回占位符，提示用户配置 GGUF 或 sentencepiece tokenizer
  return "[ONNX chat model loaded but inference requires a decoder-style ONNX model "
         "with past KV cache support. Consider using GgufModel with llama.cpp for "
         "production chat inference. Input: " + input.substr(0, 50) + "...]";
}

}  // namespace local
}  // namespace thin_agent
