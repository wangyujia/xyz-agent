#include "thin_agent/agent/EmbeddingProvider.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "thin_agent/llm/IHttpClient.h"

namespace thin_agent {
namespace agent {

// ────────────────────────────────────────────────────────────────
// 通用向量运算
// ────────────────────────────────────────────────────────────────

float cosine_similarity(const std::vector<float>& a,
                        const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) return 0.0f;

  double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    norm_a += static_cast<double>(a[i]) * static_cast<double>(a[i]);
    norm_b += static_cast<double>(b[i]) * static_cast<double>(b[i]);
  }

  if (norm_a == 0.0 || norm_b == 0.0) return 0.0f;
  return static_cast<float>(dot / (std::sqrt(norm_a) * std::sqrt(norm_b)));
}

void normalize_l2(std::vector<float>& vec) {
  double norm = 0.0;
  for (float v : vec) norm += static_cast<double>(v) * static_cast<double>(v);
  if (norm == 0.0) return;
  float inv = 1.0f / static_cast<float>(std::sqrt(norm));
  for (float& v : vec) v *= inv;
}

// ────────────────────────────────────────────────────────────────
// LocalHashEmbeddingProvider
// ────────────────────────────────────────────────────────────────

namespace {

/// FNV-1a 64-bit 哈希。
uint64_t fnv1a_64(const std::string& s) {
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char c : s) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

/// 提取字符 n-gram（n=2 或 n=3）。
std::vector<std::string> extract_ngrams(const std::string& text, int n) {
  std::vector<std::string> out;
  if (static_cast<int>(text.size()) < n) return out;

  // 转为小写
  std::string lower;
  lower.reserve(text.size());
  for (unsigned char c : text) {
    lower.push_back(static_cast<char>(std::tolower(c)));
  }

  std::unordered_set<std::string> seen;
  for (size_t i = 0; i + n <= lower.size(); ++i) {
    std::string gram = lower.substr(i, n);
    if (seen.insert(gram).second) {
      out.push_back(std::move(gram));
    }
  }
  return out;
}

}  // namespace

LocalHashEmbeddingProvider::LocalHashEmbeddingProvider(int dim)
    : dim_(dim) {
  if (dim_ < 1) dim_ = 128;
}

std::vector<float> LocalHashEmbeddingProvider::encode(const std::string& text) {
  std::vector<float> vec(dim_, 0.0f);

  if (text.empty()) return vec;

  // 提取 2-gram 和 3-gram
  auto bigrams = extract_ngrams(text, 2);
  auto trigrams = extract_ngrams(text, 3);

  // 将所有 n-gram 哈希后映射到向量
  auto apply_gram = [&](const std::string& gram, float weight) {
    uint64_t h = fnv1a_64(gram);
    int idx = static_cast<int>(h % static_cast<uint64_t>(dim_));
    // 用哈希的高位决定符号，低位决定幅值微调
    float sign = ((h >> 32) & 1) ? -1.0f : 1.0f;
    float mag = 1.0f + static_cast<float>(h & 0xFF) / 1024.0f;
    vec[idx] += sign * mag * weight;
  };

  for (const auto& bg : bigrams) apply_gram(bg, 1.0f);
  for (const auto& tg : trigrams) apply_gram(tg, 0.7f);

  // L2 归一化
  normalize_l2(vec);
  return vec;
}

// ────────────────────────────────────────────────────────────────
// CloudEmbeddingProvider
// ────────────────────────────────────────────────────────────────

namespace {

std::string trim_trailing_slash(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

}  // namespace

CloudEmbeddingProvider::CloudEmbeddingProvider(IHttpClient& http,
                                               const std::string& api_base,
                                               const std::string& api_key,
                                               const std::string& model,
                                               int timeout_ms)
    : http_(http),
      api_base_(trim_trailing_slash(api_base)),
      api_key_(api_key),
      model_(model),
      timeout_ms_(timeout_ms) {}

std::vector<float> CloudEmbeddingProvider::encode(const std::string& text) {
  if (api_base_.empty() || api_key_.empty()) {
    LocalHashEmbeddingProvider fallback(256);
    dim_ = fallback.dimension();
    return fallback.encode(text);
  }

  nlohmann::json body;
  body["model"] = model_;
  body["input"] = text;

  std::string endpoint = api_base_ + "/embeddings";
  auto resp = http_.post(endpoint, body.dump(), api_key_);

  if (!resp.ok || resp.status_code < 200 || resp.status_code >= 300) {
    LocalHashEmbeddingProvider fallback(256);
    dim_ = fallback.dimension();
    return fallback.encode(text);
  }

  try {
    auto jr = nlohmann::json::parse(resp.body);
    const auto& data = jr["data"];
    if (!data.is_array() || data.empty()) {
      LocalHashEmbeddingProvider fallback(256);
      dim_ = fallback.dimension();
      return fallback.encode(text);
    }
    const auto& emb = data[0]["embedding"];
    if (!emb.is_array()) {
      return {};
    }
    std::vector<float> vec;
    vec.reserve(emb.size());
    for (const auto& v : emb) {
      vec.push_back(v.get<float>());
    }
    dim_ = static_cast<int>(vec.size());
    normalize_l2(vec);
    return vec;
  } catch (...) {
    LocalHashEmbeddingProvider fallback(256);
    dim_ = fallback.dimension();
    return fallback.encode(text);
  }
}

}  // namespace agent
}  // namespace thin_agent
