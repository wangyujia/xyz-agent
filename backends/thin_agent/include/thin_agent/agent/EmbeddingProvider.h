#pragma once

#include <memory>
#include <string>
#include <vector>

namespace thin_agent {

// Forward declaration — avoid pulling IHttpClient into every include.
class IHttpClient;

namespace agent {

/// 嵌入向量提供者抽象接口。
/// 将文本转换为固定维度浮点向量，用于语义相似度搜索。
class EmbeddingProvider {
 public:
  virtual ~EmbeddingProvider() = default;

  /// 将文本编码为向量。
  virtual std::vector<float> encode(const std::string& text) = 0;

  /// 返回向量维度。
  virtual int dimension() const = 0;

  /// 提供者名称（用于日志）。
  virtual std::string name() const = 0;
};

/// 基于字符 n-gram 哈希的本地嵌入提供者。
///
/// 零外部依赖，纯 C++ 实现：
///   - 提取 2-gram 和 3-gram 字符序列
///   - 对每个 n-gram 做 FNV-1a 哈希
///   - 将哈希值映射到固定大小向量（取模 + 符号决定）
///
/// 精度不如神经网络嵌入模型，但无需 ONNX 模型或 tokenizer，
/// 适合轻量语义搜索场景。
class LocalHashEmbeddingProvider : public EmbeddingProvider {
 public:
  /// @param dim 向量维度（建议 128 或 256）
  explicit LocalHashEmbeddingProvider(int dim = 128);

  std::vector<float> encode(const std::string& text) override;
  int dimension() const override { return dim_; }
  std::string name() const override { return "local_hash"; }

 private:
  int dim_;
};

/// 云端嵌入提供者：通过 OpenAI 兼容 /embeddings 端点获取向量。
///
/// 需要配置有效的 api_base 和 api_key。
/// @param http           HTTP 客户端（注入，便于测试 mock）
class CloudEmbeddingProvider : public EmbeddingProvider {
 public:
  /// @param api_base      API 基础 URL（如 https://api.openai.com/v1）
  /// @param api_key       Bearer Token
  /// @param model         嵌入模型名（默认 text-embedding-3-small）
  /// @param timeout_ms    HTTP 请求超时
  CloudEmbeddingProvider(IHttpClient& http,
                         const std::string& api_base,
                         const std::string& api_key,
                         const std::string& model = "text-embedding-3-small",
                         int timeout_ms = 15000);

  std::vector<float> encode(const std::string& text) override;
  int dimension() const override { return dim_; }
  std::string name() const override { return "cloud_" + model_; }

 private:
  IHttpClient& http_;
  std::string api_base_;
  std::string api_key_;
  std::string model_;
  int timeout_ms_;
  int dim_ = 0;
};

/// 计算两个向量的余弦相似度（-1 到 1）。
float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b);

/// L2 归一化向量（原地修改）。
void normalize_l2(std::vector<float>& vec);

}  // namespace agent
}  // namespace thin_agent
