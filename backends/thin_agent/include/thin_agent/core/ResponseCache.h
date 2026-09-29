#pragma once

#include <chrono>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace thin_agent {

/// v0.47.5: LLM 响应缓存（LRU + TTL）
///
/// 缓存无工具调用的简单回复，避免相同 prompt 重复调用云 API 省 cost。
/// FC 循环（含工具调用副作用）不走缓存。
///
/// 线程安全：所有公共方法加锁。
/// 容量上限：默认 100 条（可配）。TTL：默认 3600s（1 小时）。
class ResponseCache {
 public:
  explicit ResponseCache(size_t max_entries = 100,
                         int64_t ttl_seconds = 3600)
      : max_entries_(max_entries), ttl_seconds_(ttl_seconds) {}

  /// 查询缓存。命中返回文本，未命中返回空串。
  std::string get(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(key);
    if (it == cache_.end()) return "";

    // TTL 检查
    auto now = now_ms();
    if (now - it->second.second->ts_ms > ttl_seconds_ * 1000) {
      // 过期，删除
      lru_list_.erase(it->second.second);
      cache_.erase(it);
      ++evictions_;
      return "";
    }

    // 命中：移到 LRU 头部
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.second);
    ++hits_;
    return it->second.first;
  }

  /// 写入缓存。
  void put(const std::string& key, const std::string& value) {
    if (value.empty()) return;  // 不缓存空结果
    std::lock_guard<std::mutex> lk(mu_);

    // 已存在则更新
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      it->second.first = value;
      it->second.second->value = value;
      it->second.second->ts_ms = now_ms();
      lru_list_.splice(lru_list_.begin(), lru_list_, it->second.second);
      return;
    }

    // 新增：如果超容量，淘汰 LRU 尾部
    while (cache_.size() >= max_entries_ && !lru_list_.empty()) {
      const auto& back = lru_list_.back();
      cache_.erase(back.key);
      lru_list_.pop_back();
      ++evictions_;
    }

    lru_list_.push_front({key, value, now_ms()});
    cache_[key] = {value, lru_list_.begin()};
    ++misses_;
  }

  /// 清空缓存。
  void clear() {
    std::lock_guard<std::mutex> lk(mu_);
    cache_.clear();
    lru_list_.clear();
    hits_ = 0;
    misses_ = 0;
    evictions_ = 0;
  }

  /// 统计信息。
  nlohmann::json stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    int64_t total = hits_ + misses_;
    return {
        {"entries", (int64_t)cache_.size()},
        {"max_entries", (int64_t)max_entries_},
        {"hits", hits_},
        {"misses", misses_},
        {"evictions", evictions_},
        {"hit_rate", total > 0 ? (double)hits_ / (double)total : 0.0},
    };
  }

 private:
  struct LruEntry {
    std::string key;
    std::string value;
    int64_t ts_ms;
  };

  static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  size_t max_entries_;
  int64_t ttl_seconds_;
  mutable std::mutex mu_;
  std::list<LruEntry> lru_list_;  // front = most recent
  std::unordered_map<std::string,
                     std::pair<std::string,
                               std::list<LruEntry>::iterator>> cache_;
  int64_t hits_{0};
  int64_t misses_{0};
  int64_t evictions_{0};
};

/// 生成缓存 key：model + user_text 的 SHA-256 摘要前 16 字符 hex。
/// 使用嵌入的 SHA-256（与 WebhookClient 同款，零外部依赖）。
std::string make_cache_key(const std::string& model_name,
                           const std::string& user_text);

}  // namespace thin_agent
