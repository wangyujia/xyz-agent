// unit_bg_stdin：后台进程 stdin 通路的**真链路**验证（v0.54.22）
//
// 背景（这是一个被审查抓出来的真缺陷）：`write_stdin` 曾经写的是 `proc->pipe_fd` —— 那是
// **子进程 stdout 的读端**，实证 `write(read_end) = -1 errno=9 EBADF`，即该功能**从未成功过**；
// 更糟的是失败被 `if (::write(...) < 0) {}` 吞掉，而工具层无条件返回
// `{"success": true, "output": "wrote N bytes"}` —— **对 LLM 假报成功**。
//
// 本用例不模拟、不重实现算法，直接驱动产品类 + 真实子进程：
//   1. 起 `cat`（读 stdin → 回显 stdout），写入的字节必须**真的出现在子进程输出里**；
//   2. 对不存在的会话 id 写 → 必须返回 false（不许假报成功）；
//   3. kill 之后再写 → 必须返回 false（stdin 写端已随收割关闭）。
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "test_macros.h"
#include "thin_agent/core/BackgroundProcessManager.h"

using thin_agent::BackgroundProcessManager;
using thin_agent::BgProcessState;

int main() {
  // 单例（构造/析构为 private）
  BackgroundProcessManager& mgr = BackgroundProcessManager::instance();

  // ── 1) cat 真回显：写入的字节必须出现在输出里 ──
  const std::string id = mgr.start("cat", 10000);
  ASSERT_TRUE("start_ok", !id.empty());

  const std::string payload = "hello-bg-stdin-line\n";
  ASSERT_TRUE("write_ok", mgr.write_stdin(id, payload));

  bool seen = false;
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0).count() < 4000) {
    BgProcessState st = mgr.poll(id);
    if (st.output.find("hello-bg-stdin-line") != std::string::npos) {
      seen = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ASSERT_TRUE("written_bytes_reach_child_stdin", seen);

  // ── 2) 不存在的会话 id：必须 false（旧实现吞错+上层假报成功）──
  ASSERT_TRUE("unknown_id_returns_false", !mgr.write_stdin("proc_does_not_exist", "x"));

  // ── 2b) v0.54.27 探针：子进程**不读 stdin** 时写大块数据 —— 必须**快速返回**（不得阻塞）──
  // 隐患（本轮审查发现）：父进程持写端，管道写满（64KB）后 write() 会**阻塞**；而 write_stdin 是
  // **持管理器互斥锁**写的 ⇒ 一旦阻塞，poll/wait/kill 全部卡死 = 后台进程整体冻结（旧实现恒 EBADF 反而不会）。
  {
    const std::string id2 = mgr.start("sleep 30", 60000);
    if (!id2.empty()) {
      std::string big(1024 * 1024, 'x');   // 1MB > 管道容量
      const auto t2 = std::chrono::steady_clock::now();
      (void)mgr.write_stdin(id2, big);
      const auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t2).count();
      std::cout << "  [timing] big-write-no-reader returned in " << ms2 << "ms\n";
      ASSERT_TRUE("big_write_does_not_block", ms2 < 2000);
      (void)mgr.kill(id2);
    }
  }

  // ── 2c) v0.54.28: 大载荷（>64KB 管道容量）对**正常读**的子进程必须**完整送达** ──
  // 旧版（v0.54.27）对 >60KB 直接硬拒 ⇒ 本断言必红；新版走有界排空 ⇒ 应绿。
  {
    const std::string id3 = mgr.start("cat", 20000);
    if (!id3.empty()) {
      const std::string big(80 * 1024, 'y');
      ASSERT_TRUE("big_write_to_reader_succeeds", mgr.write_stdin(id3, big));
      // 注意：管理器对子进程输出只**保留最后 32KB**（BackgroundProcessManager 里的 substr 截断），
      // 故断言"尾部被 'y' 填满"即证明 80KB 载荷**确实流过子进程**（而不是断言能留住 80KB）。
      bool got = false;
      for (int i = 0; i < 120 && !got; ++i) {
        BgProcessState st3 = mgr.poll(id3);
        if (st3.output.size() >= 32768) {
          const std::string tail = st3.output.substr(st3.output.size() - 1000);
          got = (tail.find_first_not_of('y') == std::string::npos);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      ASSERT_TRUE("big_payload_reaches_child", got);
      (void)mgr.kill(id3);
    }
  }

  // ── 3) 收割后写端关闭：必须 false ──
  (void)mgr.kill(id);
  ASSERT_TRUE("after_kill_returns_false", !mgr.write_stdin(id, "x"));

  TEST_REPORT();
}
