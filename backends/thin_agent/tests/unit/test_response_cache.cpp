// unit_response_cache：LLM 响应缓存 LRU+TTL 测试
// 验证 v0.47.5 ResponseCache 核心不变量：
//   1. put/get 基本：写入命中，不同 key 未命中
//   2. LRU 淘汰：超容量时淘汰最久未访问
//   3. TTL 过期：过期后 get 返回空
//   4. 空值不缓存
//   5. stats 统计：hits/misses/hit_rate
//   6. make_cache_key：相同输入相同 key，不同输入不同 key

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "thin_agent/core/ResponseCache.h"
#include "test_macros.h"

int main() {
  using namespace thin_agent;

  // ── 测试 1: put/get 基本 ──
  {
    ResponseCache cache(100, 3600);
    cache.put("key1", "response1");
    ASSERT_EQ("basic_hit", cache.get("key1"), "response1");
    ASSERT_EQ("basic_miss", cache.get("key2"), "");
  }

  // ── 测试 2: LRU 淘汰 ──
  {
    ResponseCache cache(3, 3600);  // 只存 3 条
    cache.put("a", "va");
    cache.put("b", "vb");
    cache.put("c", "vc");
    // 访问 a，使其变为最近使用
    cache.get("a");
    // 插入 d → 淘汰最久未使用的 b
    cache.put("d", "vd");
    ASSERT_EQ("lru_a_survives", cache.get("a"), "va");
    ASSERT_EQ("lru_b_evicted", cache.get("b"), "");
    ASSERT_EQ("lru_c_survives", cache.get("c"), "vc");
    ASSERT_EQ("lru_d_survives", cache.get("d"), "vd");
  }

  // ── 测试 3: TTL 过期 ──
  {
    ResponseCache cache(100, 1);  // TTL=1 秒
    cache.put("ephemeral", "data");
    ASSERT_EQ("ttl_before_expiry", cache.get("ephemeral"), "data");
    // 等待 1.5 秒过期
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ASSERT_EQ("ttl_after_expiry", cache.get("ephemeral"), "");
  }

  // ── 测试 4: 空值不缓存 ──
  {
    ResponseCache cache(100, 3600);
    cache.put("empty_key", "");
    ASSERT_EQ("empty_not_cached", cache.get("empty_key"), "");
  }

  // ── 测试 5: stats 统计 ──
  {
    ResponseCache cache(100, 3600);
    cache.put("s1", "v1");
    cache.get("s1");   // hit
    cache.get("s1");   // hit
    cache.get("miss"); // miss
    auto s = cache.stats();
    ASSERT_EQ("stats_hits", s["hits"].get<int64_t>(), 2);
    ASSERT_EQ("stats_entries", s["entries"].get<int64_t>(), 1);
    // hit_rate = 2/3 ≈ 0.667
    double hr = s["hit_rate"].get<double>();
    ASSERT_TRUE("stats_hit_rate", hr > 0.6 && hr < 0.7);
  }

  // ── 测试 6: make_cache_key 确定性 + 区分性 ──
  {
    std::string k1 = make_cache_key("model-a", "hello world");
    std::string k2 = make_cache_key("model-a", "hello world");
    std::string k3 = make_cache_key("model-a", "hello earth");
    std::string k4 = make_cache_key("model-b", "hello world");
    ASSERT_TRUE("key_deterministic", k1 == k2);
    ASSERT_TRUE("key_diff_text", k1 != k3);
    ASSERT_TRUE("key_diff_model", k1 != k4);
    // key 以 model 名开头
    ASSERT_TRUE("key_starts_with_model", k1.rfind("model-a:", 0) == 0);
  }

  // ── 测试 7: 更新已存在的 key（不新增条目）──
  {
    ResponseCache cache(100, 3600);
    cache.put("upd", "old");
    cache.put("upd", "new");
    ASSERT_EQ("update_value", cache.get("upd"), "new");
    auto s = cache.stats();
    ASSERT_EQ("update_no_dup", s["entries"].get<int64_t>(), 1);
  }

  // ── 测试 8: clear 清空 ──
  {
    ResponseCache cache(100, 3600);
    cache.put("c1", "v1");
    cache.put("c2", "v2");
    cache.clear();
    ASSERT_EQ("clear_1", cache.get("c1"), "");
    ASSERT_EQ("clear_2", cache.get("c2"), "");
    auto s = cache.stats();
    ASSERT_EQ("clear_stats", s["entries"].get<int64_t>(), 0);
  }

  TEST_REPORT();
}
