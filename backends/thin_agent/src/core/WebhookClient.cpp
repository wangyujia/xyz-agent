#include "thin_agent/core/WebhookClient.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace thin_agent {
namespace {

// ── embedded SHA-256 (FIPS 180-4, compact public domain) ──

struct Sha256Ctx {
  uint32_t state[8];
  uint64_t count;
  uint8_t buf[64];
  uint32_t buflen;
};

static const uint32_t kSha256K[] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
  0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
  0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(Sha256Ctx* ctx, const uint8_t* block) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2],
           d = ctx->state[3];
  uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6],
           h = ctx->state[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t temp1 = h + S1 + ch + kSha256K[i] + w[i];
    uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = S0 + maj;
    h = g; g = f; f = e; e = d + temp1;
    d = c; c = b; b = a; a = temp1 + temp2;
  }
  ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
  ctx->state[3] += d; ctx->state[4] += e; ctx->state[5] += f;
  ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(Sha256Ctx* ctx) {
  ctx->state[0] = 0x6a09e667;
  ctx->state[1] = 0xbb67ae85;
  ctx->state[2] = 0x3c6ef372;
  ctx->state[3] = 0xa54ff53a;
  ctx->state[4] = 0x510e527f;
  ctx->state[5] = 0x9b05688c;
  ctx->state[6] = 0x1f83d9ab;
  ctx->state[7] = 0x5be0cd19;
  ctx->count = 0;
  ctx->buflen = 0;
}

static void sha256_update(Sha256Ctx* ctx, const uint8_t* data, size_t len) {
  ctx->count += len;
  while (len > 0) {
    size_t space = 64 - ctx->buflen;
    size_t copy = len < space ? len : space;
    memcpy(ctx->buf + ctx->buflen, data, copy);
    ctx->buflen += (uint32_t)copy;
    data += copy;
    len -= copy;
    if (ctx->buflen == 64) {
      sha256_transform(ctx, ctx->buf);
      ctx->buflen = 0;
    }
  }
}

static void sha256_final(Sha256Ctx* ctx, uint8_t* hash) {
  uint64_t bits = ctx->count * 8;
  // pad
  sha256_update(ctx, (const uint8_t*)"\x80", 1);
  while (ctx->buflen != 56)
    sha256_update(ctx, (const uint8_t*)"\x00", 1);
  uint8_t len_buf[8];
  for (int i = 0; i < 8; ++i)
    len_buf[i] = (uint8_t)(bits >> (56 - i * 8));
  sha256_update(ctx, len_buf, 8);
  // output
  for (int i = 0; i < 8; ++i) {
    hash[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
    hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
    hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
    hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
  }
}

static std::string sha256_hex(const std::string& data) {
  Sha256Ctx ctx;
  sha256_init(&ctx);
  sha256_update(&ctx, (const uint8_t*)data.data(), data.size());
  uint8_t hash[32];
  sha256_final(&ctx, hash);
  static const char hex[] = "0123456789abcdef";
  std::string result(64, ' ');
  for (int i = 0; i < 32; ++i) {
    result[i * 2]     = hex[hash[i] >> 4];
    result[i * 2 + 1] = hex[hash[i] & 0x0f];
  }
  return result;
}

static std::string hmac_sha256_hex(const std::string& key,
                                    const std::string& data) {
  // HMAC(K, m) = H((K' ⊕ opad) || H((K' ⊕ ipad) || m))
  uint8_t k_ipad[64], k_opad[64];
  uint8_t k[32];
  Sha256Ctx ctx;

  // keys longer than block size → hash first
  if (key.size() > 64) {
    std::string hashed = sha256_hex(key);
    for (int i = 0; i < 32; ++i) {
      auto hex_to_nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
      };
      k[i] = (hex_to_nibble(hashed[i * 2]) << 4) | hex_to_nibble(hashed[i * 2 + 1]);
    }
    memcpy(k_ipad, k, 32);
    memcpy(k_opad, k, 32);
    memset(k_ipad + 32, 0, 32);
    memset(k_opad + 32, 0, 32);
  } else {
    memcpy(k_ipad, key.data(), key.size());
    memcpy(k_opad, key.data(), key.size());
    memset(k_ipad + key.size(), 0, 64 - key.size());
    memset(k_opad + key.size(), 0, 64 - key.size());
  }
  for (int i = 0; i < 64; ++i) {
    k_ipad[i] ^= 0x36;
    k_opad[i] ^= 0x5c;
  }
  // inner: H(K' ⊕ ipad || message)
  sha256_init(&ctx);
  sha256_update(&ctx, k_ipad, 64);
  sha256_update(&ctx, (const uint8_t*)data.data(), data.size());
  uint8_t inner[32];
  sha256_final(&ctx, inner);
  // outer: H(K' ⊕ opad || inner)
  sha256_init(&ctx);
  sha256_update(&ctx, k_opad, 64);
  sha256_update(&ctx, inner, 32);
  uint8_t outer[32];
  sha256_final(&ctx, outer);
  // hex
  static const char hex[] = "0123456789abcdef";
  std::string result(64, ' ');
  for (int i = 0; i < 32; ++i) {
    result[i * 2]     = hex[outer[i] >> 4];
    result[i * 2 + 1] = hex[outer[i] & 0x0f];
  }
  return result;
}

}  // anonymous namespace

// ── WebhookClient implementation ──

bool WebhookClient::load(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return false;
  nlohmann::json j;
  try { f >> j; } catch (...) { return false; }
  return load_json(j);
}

bool WebhookClient::load_json(const nlohmann::json& j) {
  if (!j.is_array()) return false;
  std::lock_guard<std::mutex> lock(mu_);
  targets_.clear();
  for (const auto& item : j) {
    Target t;
    t.name = item.value("name", "");
    t.url = item.value("url", "");
    t.secret = item.value("secret", "");
    t.enabled = item.value("enabled", true);
    if (t.name.empty() || t.url.empty()) continue;
    targets_.push_back(std::move(t));
  }
  return !targets_.empty();
}

nlohmann::json WebhookClient::send(const std::string& name,
                                    const std::string& body) {
  Target t;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(targets_.begin(), targets_.end(),
                           [&](const Target& x) { return x.name == name; });
    if (it == targets_.end())
      return {{"success", false}, {"error", "webhook not found: " + name}};
    if (!it->enabled)
      return {{"success", false}, {"error", "webhook disabled: " + name}};
    t = *it;
  }
  std::string sig;
  if (!t.secret.empty()) {
    sig = hmac_sha256_hex(t.secret, body);
  }
  return post_signed(t.url, sig, body);
}

nlohmann::json WebhookClient::send_direct(const std::string& url,
                                           const std::string& secret,
                                           const std::string& body) {
  std::string sig;
  if (!secret.empty()) {
    sig = hmac_sha256_hex(secret, body);
  }
  return post_signed(url, sig, body);
}

nlohmann::json WebhookClient::list() const {
  std::lock_guard<std::mutex> lock(mu_);
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& t : targets_) {
    arr.push_back({
      {"name", t.name},
      {"url", t.url},
      {"enabled", t.enabled},
    });
  }
  return arr;
}

nlohmann::json WebhookClient::test(const std::string& name,
                                    const std::string& body) {
  nlohmann::json result = send(name, body);
  // 添加测试元信息
  result["test_name"] = name;
  result["test_body_size"] = body.size();
  return result;
}

nlohmann::json WebhookClient::post_signed(const std::string& url,
                                           const std::string& signature,
                                           const std::string& body) {
  CURL* curl = curl_easy_init();
  if (!curl) return {{"success", false}, {"error", "curl init failed"}};

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (!signature.empty()) {
    headers = curl_slist_append(headers, ("X-Signature: " + signature).c_str());
  }

  std::string response_body;
  long http_code = 0;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                   +[](char* ptr, size_t size, size_t nmemb, void* userdata) {
                     auto* resp = (std::string*)userdata;
                     resp->append(ptr, size * nmemb);
                     return size * nmemb;
                   });
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

  CURLcode res = curl_easy_perform(curl);
  if (res == CURLE_OK)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  else
    response_body = curl_easy_strerror(res);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  return {
    {"success", res == CURLE_OK},
    {"status_code", (int)http_code},
    {"response_body", response_body},
    {"signature", signature},
  };
}

}  // namespace thin_agent
