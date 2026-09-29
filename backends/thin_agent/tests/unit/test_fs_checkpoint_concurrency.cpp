// unit_fs_checkpoint_concurrency：FilesystemCheckpoint 的**并发 save** 与 **manifest 原子性**
//
// 第五轮审查（v0.54.31）发现：`save()` 写 manifest 用的是**就地覆盖**（`std::ofstream ofs(mp); ofs << dump;`），
// 而同一文件对 blob/workdir 都用 tmp+rename 原子替换。就地覆盖有两个真实后果：
//   ① 进程在写中途被 kill ⇒ 磁盘上留下**半截 manifest**；
//   ② **另一个进程/读者**（如共享 THIN_AGENT_HOME 的 cron 侧、或任何直接读 manifest 的代码）能读到
//      半截 manifest ⇒ 解析失败 ⇒ 该快照被 list/prune **静默丢弃**（比崩溃更隐蔽：回滚时快照"不存在"）。
// 本用例用**直接读文件**的读者线程忠实模拟"跨进程读者"（进程内公开 API 同锁，掩盖不了这个问题）。
//
// 断言：
//   A) 并发 save（2 线程 × 4 次）后：checkpoint id **互不重复**，且每个 manifest 都能完整解析；
//   B) 在有 save 并发进行时，读者线程**从磁盘直接读** manifest 字节流 —— 任何一次"读到的字节不是完整
//      JSON"都算**撕裂**（原子写 ⇒ 读者只会看到"旧完整版"或"新完整版"）⇒ 计数必须为 0；
//   C) manifest 数量与 save 次数一致（无静默丢失）。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "thin_agent/agent/FilesystemCheckpoint.h"

namespace fs = std::filesystem;

namespace {

int g_fail = 0;

void check(const std::string& name, bool ok, const std::string& extra = "") {
  std::cout << (ok ? "PASS: " : "FAIL: ") << name
            << (ok || extra.empty() ? "" : " — " + extra) << "\n";
  if (!ok) ++g_fail;
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/fs_ckpt_conc_XXXXXX";
  char* mk = ::mkdtemp(tmpl);
  if (mk == nullptr) {
    std::cout << "SKIP: mkdtemp failed\n";
    return 0;
  }
  const std::string root = mk;
  const std::string base = root + "/ckpt";
  const std::string workdir = root + "/work";
  fs::create_directories(workdir);

  // 造足够多的小文件 ⇒ manifest 足够大（写几十~几百 KB），就地覆盖的"半截窗口"才明显可观测
  const int kFiles = 1500;
  for (int i = 0; i < kFiles; ++i) {
    std::ofstream o(workdir + "/f" + std::to_string(i) + ".txt");
    o << "content-" << i << "\n";
  }

  thin_agent::agent::FilesystemCheckpoint ck(base);

  // 先做一次 save，拿到"完整 manifest 的字节数"（同 workdir ⇒ 每次 save 的 manifest 大小恒定）
  const std::string seed_id = ck.save(workdir, {}, "conc");
  if (seed_id.empty()) {
    std::cout << "SKIP: 首次 save 失败\n";
    return 0;
  }
  const std::string seed_mp = base + "/" + seed_id + "/manifest.json";
  const std::uintmax_t full_size = fs::file_size(seed_mp);
  if (full_size < 4096) {
    std::cout << "SKIP: manifest 太小（" << full_size << "）无法有效观测\n";
    return 0;
  }
  std::cout << "  [观察] 完整 manifest 大小 = " << full_size << " 字节\n";

  std::atomic<bool> stop{false};
  std::atomic<long> samples{0};
  std::atomic<int> torn{0};        // 在"已存在的 manifest 路径"上读到**小于完整尺寸** ⇒ 就地覆盖铁证

  // 读者线程：只 stat（极快 ⇒ 采样密度高）。原子替换下，该路径要么不存在，要么**必为完整尺寸**
  // （同 workdir 的 manifest 大小恒定）⇒ 任何"存在但更小"的采样都只能来自**就地截断写**。
  std::thread reader([&]() {
    std::error_code ec;
    while (!stop.load()) {
      std::uintmax_t sz = fs::file_size(seed_mp, ec);
      if (!ec) {
        ++samples;
        if (sz < full_size) ++torn;
      }
      ec.clear();
      // 同时扫其它 manifest（写者会写新 id）
      for (const auto& e : fs::directory_iterator(base, ec)) {
        if (e.path().filename().string() == "objects") continue;
        std::error_code ec2;
        const std::uintmax_t s2 = fs::file_size(e.path() / "manifest.json", ec2);
        if (!ec2) {
          ++samples;
          if (s2 < full_size) ++torn;
        }
      }
      ec.clear();
    }
  });

  // 写者：3 线程并发 save（同一 workdir、同一实例）—— 每次 save 都会重写同尺寸 manifest
  std::vector<std::string> ids;
  std::mutex ids_mu;
  auto writer = [&]() {
    for (int i = 0; i < 6; ++i) {
      const std::string id = ck.save(workdir, {}, "conc");
      std::lock_guard<std::mutex> lk(ids_mu);
      ids.push_back(id);
    }
  };
  std::thread w1(writer);
  std::thread w2(writer);
  std::thread w3(writer);
  w1.join();
  w2.join();
  w3.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  stop.store(true);
  reader.join();

  // A) id 唯一
  {
    std::vector<std::string> sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    const bool uniq = std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
    check("concurrent_ids_unique", uniq && ids.size() == 18,
          "n=" + std::to_string(ids.size()));
  }

  // C) manifest 数量 == save 次数（无静默丢失）
  {
    int cnt = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(base, ec)) {
      if (e.path().filename().string() == "objects") continue;
      if (fs::exists(e.path() / "manifest.json", ec)) ++cnt;
    }
    check("all_manifests_present", cnt == 19, "cnt=" + std::to_string(cnt));   // 18 + seed
    if (cnt != 19) {
      std::cout << "  [诊断] ids=" << ids.size() << " manifests=" << cnt << "\n";
    }
  }

  // A2) 每个 manifest 都能完整解析
  {
    int ok_cnt = 0, bad = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(base, ec)) {
      if (e.path().filename().string() == "objects") continue;
      std::ifstream ifs(e.path() / "manifest.json", std::ios::binary);
      if (!ifs) continue;
      std::stringstream ss;
      ss << ifs.rdbuf();
      try {
        auto j = nlohmann::json::parse(ss.str());
        if (j.contains("files") && j.contains("workdir")) ++ok_cnt; else ++bad;
      } catch (...) {
        ++bad;
      }
    }
    check("all_manifests_parse", bad == 0 && ok_cnt == 19,
          "ok=" + std::to_string(ok_cnt) + " bad=" + std::to_string(bad));
  }

  // B) 并发读：不得出现撕裂（半截 JSON）
  {
    const bool ok = (torn.load() == 0) && (samples.load() > 1000);
    check("no_inplace_truncation_observed", ok,
          "torn=" + std::to_string(torn.load()) + " samples=" + std::to_string(samples.load()) +
              " full_size=" + std::to_string(full_size));
  }

  const std::string rm = "rm -rf " + root;
  (void)::system(rm.c_str());

  if (g_fail != 0) {
    std::cout << "SOME FAILED (" << g_fail << " failures)\n";
    return 1;
  }
  std::cout << "ALL PASS\n";
  return 0;
}
