// unit_credential_pool：CredentialPool 全分支覆盖 — 加载/轮转/降级/统计。
//
// 覆盖点：
//   1. load_json 正常数组 + 非数组拒绝 + 空 service/key 跳过
//   2. get：单 key、多 key 轮转、type 过滤、未知 service
//   3. mark_failed：3 次失败自动降级（active=false）→ 切到下一个 key
//   4. stats：total/active/failed 汇总与 per-entry 明细
//   5. load：文件不存在 false / 合法文件 true

#include <cstdio>
#include <fstream>

#include <nlohmann/json.hpp>

#include "thin_agent/core/CredentialPool.h"

namespace {

int g_failures = 0;

int expect(bool ok, const std::string& msg) {
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", msg.c_str());
  }
  return g_failures == 1 ? 1 : 0;
}

}  // namespace

int main() {
  using thin_agent::CredentialPool;

  // ── 1. load_json ──
  {
    CredentialPool pool;
    nlohmann::json cfg = nlohmann::json::array({
        {{"service", "openai"}, {"key", "sk-a"}, {"type", "api_key"}},
        {{"service", "openai"}, {"key", "sk-b"}, {"type", "api_key"}},
        {{"service", "anthropic"}, {"key", "ak-x"}, {"type", "api_key"}},
        {{"service", "openai"}, {"key", "sk-c"}, {"type", "bearer"}},
        // 无效条目：空 service / 空 key → 跳过
        {{"service", ""}, {"key", "k"}},
        {{"service", "bad"}, {"key", ""}},
    });
    expect(pool.load_json(cfg), "load_json valid array returns true");

    // ── 2. get + 轮转 ──
    std::string k1 = pool.get("openai");
    expect(!k1.empty(), "get openai non-empty");
    // 多 key 轮转：sk-a/sk-b 均为 api_key，交替返回
    std::string seen_a, seen_b;
    for (int i = 0; i < 4; ++i) {
      std::string k = pool.get("openai");
      if (k == "sk-a") seen_a = k;
      if (k == "sk-b") seen_b = k;
    }
    expect(seen_a == "sk-a" && seen_b == "sk-b",
           "round-robin alternates sk-a/sk-b (got " + seen_a + "/" + seen_b + ")");
    // type 过滤：bearer 类型独立获取
    expect(pool.get("openai", "bearer") == "sk-c", "get with type=bearer");
    // 未知 service
    expect(pool.get("nope").empty(), "unknown service returns empty");

    // ── 3. mark_failed 降级 ──
    pool.mark_failed("openai", "sk-a");
    pool.mark_failed("openai", "sk-a");
    // 2 次未降级，sk-a 仍可返回
    bool a_still = false;
    for (int i = 0; i < 2; ++i) {
      if (pool.get("openai") == "sk-a") a_still = true;
    }
    expect(a_still, "2 failures: sk-a still active");
    pool.mark_failed("openai", "sk-a");  // 第 3 次 → 降级
    bool a_gone = true;
    for (int i = 0; i < 4; ++i) {
      if (pool.get("openai") == "sk-a") a_gone = false;
    }
    expect(a_gone, "3 failures: sk-a deactivated, only sk-b served");

    // ── 4. stats ──
    nlohmann::json st = pool.stats();
    expect(st["_summary"]["total"].get<int>() == 4, "stats total=4 valid entries");
    expect(st["_summary"]["total_entries"].get<int>() == 4, "stats invalid entries skipped");
    // v0.50.1 去重后：首个保持原名（sk-a fail 3 次），sk-b 为 #2
    expect(st["openai:api_key"]["fail_count"].get<int>() == 3, "stats sk-a fail_count=3");
    expect(st["openai:api_key"]["active"].get<bool>() == false, "stats sk-a inactive");
    expect(st.contains("openai:api_key#2"), "stats multi-key entry #2 present");
    expect(st["openai:api_key#2"]["active"].get<bool>() == true, "stats sk-b active");
    expect(st["anthropic:api_key"]["active"].get<bool>() == true, "stats anthropic active");
  }

  // ── 5. load 文件 ──
  {
    CredentialPool pool;
    expect(!pool.load("/nonexistent/credentials.json"), "load missing file false");
    const char* tmp = "/tmp/test_credential_pool.json";
    {
      std::ofstream f(tmp);
      f << R"([{"service":"svc1","key":"k1"}])";
    }
    expect(pool.load(tmp), "load valid file true");
    expect(pool.get("svc1") == "k1", "loaded key retrievable");
    std::remove(tmp);
  }

  // ── 非数组拒绝 ──
  {
    CredentialPool pool;
    expect(!pool.load_json(nlohmann::json::object({{"a", 1}})), "object rejected");
    expect(!pool.load_json(nlohmann::json::array()), "empty array rejected");
  }

  if (g_failures == 0) {
    std::printf("unit:test_credential_pool PASS\n");
    return 0;
  }
  std::printf("unit:test_credential_pool FAIL (%d)\n", g_failures);
  return 1;
}
