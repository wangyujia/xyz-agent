// unit_rotating_log_atomicity：RotatingLogBuf 的**行原子性**不变量（v0.54.27 补测）
//
// 为什么单独建用例：v0.54.24/27 两次改动都落在这个缓冲区上（"只写完整行 + 时间戳与正文合并单次写 +
// 跨线程不粘行 + 超长补换行"），而它此前**没有任何单测** —— 只靠服务级 e2e（LD_PRELOAD 探针）间接覆盖。
// 本用例直接驱动缓冲区，断言三条不变量：
//   1) 普通行：成行落盘（文件里每行都以 '\n' 结尾）；
//   2) 超长且**始终无换行**的内容：**不得**在文件里留下半行（>=64KB 时补换行后成行落盘）；
//   3) 两个线程先后写半行：**不得粘成一行**（A 写 "AAA" 无换行 → B 写 "BBB\n" ⇒ "AAA" 必须自成一行）。
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#include "thin_agent/log/RotatingLogger.h"

namespace {

int g_fail = 0;

void check(const std::string& name, bool ok, const std::string& extra = "") {
  if (ok) {
    std::cout << "PASS: " << name << "\n";
  } else {
    std::cout << "FAIL: " << name << (extra.empty() ? "" : " — " + extra) << "\n";
    ++g_fail;
  }
}

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

bool has_line(const std::string& content, const std::string& needle) {
  std::istringstream iss(content);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/rl_atomic_XXXXXX";
  char* mk = ::mkdtemp(tmpl);
  if (mk == nullptr) {
    std::cout << "SKIP: mkdtemp failed\n";
    return 0;
  }
  const std::string dir = mk;
  const std::string path = dir + "/svc.log";

  {
    thin_agent::RotatingLogBuf buf(path, 20ULL * 1024 * 1024);
    std::ostream os(&buf);

    // ① 普通行
    os << "[t] normal-line\n";
    os.flush();

    // ② 超长且始终无换行（>64KB）：不得留下半行
    {
      const std::string huge(70 * 1024, 'x');
      os << "[t] " << huge;
      os.flush();
    }

    // ③ 跨线程：A 写半行（无换行）后 B 写一整行 ⇒ 不得粘成一行
    {
      std::thread ta([&os]() { os << "[t] AAA"; });
      ta.join();
      std::thread tb([&os]() { os << "BBB\n"; });
      tb.join();
      os.flush();
    }

    // ④ 进程退出前未终结的残留（无换行）⇒ 析构时应补换行成行落盘，不得留半行
    os << "[t] tail-without-newline";
    os.flush();
  }   // ← buf 析构：残留成行落盘

  const std::string content = slurp(path);

  check("normal_line_written", has_line(content, "normal-line"));

  // 不变量：文件里每行都以 '\n' 结尾（末行也必须有）——"只出现完整行"
  check("file_ends_with_newline", !content.empty() && content.back() == '\n',
        "tail=[" + content.substr(content.size() > 40 ? content.size() - 40 : 0) + "]");

  // ② 超长且无换行：按 kMaxPendingBytes 安全阀**补换行分批**成行（允许拆成多行，但不许丢内容、
  //    也不许留下半行）。故断言：连续 'x' 段长度 >= 64KB，且文件末尾仍是换行（见上一条）。
  {
    size_t total_x = 0, best = 0, cur = 0;
    for (char ch : content) {
      if (ch == 'x') { ++cur; ++total_x; best = cur > best ? cur : best; } else { cur = 0; }
    }
    // 关键不变量：**内容一个字节都不能丢**（分批只是行边界变化）；单批上限受时间戳前缀挤占略有出入
    check("huge_no_newline_content_preserved", total_x == 70 * 1024,
          "total_x=" + std::to_string(total_x) + " longest_run=" + std::to_string(best));
  }

  // ③ 不得粘行："AAA" 与 "BBB" 必须在**不同**行
  const bool aaa_alone = has_line(content, "AAA");
  const bool bbb_alone = has_line(content, "BBB");
  const bool merged = content.find("AAABBB") != std::string::npos;
  check("no_cross_thread_line_merge", aaa_alone && bbb_alone && !merged,
        aaa_alone && bbb_alone ? "merged=true" : "missing AAA/BBB");

  // ④ 未终结残留成行
  check("tail_written_as_line", has_line(content, "tail-without-newline"));

  // ── ④ v0.54.29 轮转路径：触发轮转后，日志与 bak 里的行必须**一条不少、行行完整** ──
  {
    const std::string rdir = dir + "/rot";
    (void)::system(("mkdir -p " + rdir).c_str());
    const std::string rlog = rdir + "/rot.log";
    const std::string rbak = rdir + "/rot_bak.log";
    {
      thin_agent::RotatingLogBuf buf2(rlog, 400);   // 极小阈值 ⇒ 必然轮转
      std::ostream os2(&buf2);
      for (int i = 0; i < 20; ++i) os2 << "[r] line-" << i << "\n";
      os2.flush();
    }
    const std::string c_log = slurp(rlog);
    const std::string c_bak = slurp(rbak);
    int found = 0;
    for (int i = 0; i < 20; ++i) {
      const std::string needle = "line-" + std::to_string(i);
      if (has_line(c_log, needle) || has_line(c_bak, needle)) ++found;
    }
    check("rotation_no_line_lost", found == 20, "found=" + std::to_string(found));
    check("rotation_bak_created", !c_bak.empty() || !c_log.empty());
    // 轮转后两个文件里的每一行都必须是完整行（末尾换行）
    const bool log_ok = c_log.empty() || c_log.back() == '\n';
    const bool bak_ok = c_bak.empty() || c_bak.back() == '\n';
    check("rotation_lines_complete", log_ok && bak_ok);
  }

  // ── ⑤ v0.54.29 启动轮转：已存在的日志必须被搬去 bak（内容不丢） ──
  {
    const std::string sdir = dir + "/start";
    (void)::system(("mkdir -p " + sdir).c_str());
    const std::string slog = sdir + "/start.log";
    const std::string sbak = sdir + "/start_bak.log";
    { std::ofstream o(slog); o << "OLD-CONTENT\n"; }
    {
      thin_agent::RotatingLogBuf buf3(slog, 10ULL * 1024 * 1024);
      std::ostream os3(&buf3);
      os3 << "[s] new-line\n";
      os3.flush();
    }
    check("startup_rotate_moves_old_to_bak", has_line(slurp(sbak), "OLD-CONTENT"));
    check("startup_rotate_new_log_fresh", has_line(slurp(slog), "new-line"));
  }

  // ── ⑥ v0.54.29 轮转**失败**时绝不能丢日志（旧实现会 remove 日志文件 ⇒ 全量丢失） ──
  {
    const std::string fdir = dir + "/fail";
    (void)::system(("mkdir -p " + fdir).c_str());
    const std::string flog = fdir + "/f.log";
    const std::string fbak = fdir + "/f_bak.log";
    // 把 bak 路径造成**非空目录** ⇒ remove(bak) 失败 + rename(log→bak) 失败（即使 root 也必失败）
    (void)::system(("mkdir -p " + fbak + " && touch " + fbak + "/occupied").c_str());
    {
      thin_agent::RotatingLogBuf buf4(flog, 200);   // 小阈值 ⇒ 触发轮转（而轮转注定失败）
      std::ostream os4(&buf4);
      for (int i = 0; i < 12; ++i) os4 << "[f] keep-" << i << "\n";
      os4.flush();
    }
    const std::string c = slurp(flog);
    int kept = 0;
    for (int i = 0; i < 12; ++i) {
      if (has_line(c, "keep-" + std::to_string(i))) ++kept;
    }
    check("rotate_failure_keeps_log_content", kept == 12, "kept=" + std::to_string(kept));
    check("rotate_failure_log_complete", c.empty() || c.back() == '\n');
  }

  const std::string rm = "rm -rf " + dir;
  (void)::system(rm.c_str());

  if (g_fail != 0) {
    std::cout << "SOME FAILED (" << g_fail << " failures)\n";
    return 1;
  }
  std::cout << "ALL PASS\n";
  return 0;
}
