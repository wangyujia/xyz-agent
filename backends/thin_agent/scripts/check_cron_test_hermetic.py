#!/usr/bin/env python3
"""机械判官：`CronScheduler` 测试必须**密闭**——固定路径 DB 必须在 `start()` 之前清除。

为什么要这条铁律（v0.54.7 / R93 实测：一次真抖动 + 确定性复现）：
  `CronScheduler::start(path)` 首次 tick 会把库里**已到期且 enabled=1** 的行**补跑**
  （cron catch-up 语义，生产上是特性）。于是测试若用固定路径 DB 且启动前不清理：
    · 上一轮运行 / 旧版测试留下的到期行会被**本进程的 ticker** 触发；
    · callback 的调用次数、并发峰值、负载内容全部漂移 ⇒ 断言随库状态飘；
    · 表现为"全量 -j4 红、单跑绿"的**假抖动**（真因是残留，不是并发）。
  实测：向 `/tmp/test_cron_trigger_lock.db` 注入 1 行
  `name='trig', schedule='2020-01-01T00:00:00', enabled=1, next_run_ts=now-100`
  ⇒ 立刻 5 条断言失败；`test_cron_trigger_lock` 单跑也有 1/3 概率红（库越脏越容易）。

判据：对 `tests/**/*.cpp` 里每处 `start("<path>"`，要求**在它之前**：
  ①出现过该 `<path>` 字面量，且 ②该字面量的 ±5 行内出现 `remove(`/`unlink(` 调用。
  （兼容"逐条 remove"与"循环体里 remove"两种写法；`-wal`/`-shm` 只提示不判红。）

用法：python3 scripts/check_cron_test_hermetic.py [--root <repo>]
退出码：0=通过；1=存在未密闭的 cron 测试。
"""
import argparse
import os
import re
import sys

START_RE = re.compile(r'\.start\(\s*"([^"]+)"')
DYN_START_RE = re.compile(r'\.start\(\s*[^"\s)]')
CLEAN_RE = re.compile(r'(std::remove|std::filesystem::remove|remove|unlink)\s*\(')
NEAR = 5


def is_persistent_db(path):
    """只对**持久化 DB 文件**判红：危害来自"跨运行残留 + ticker 补跑"。
    `:memory:` 天然密闭（无残留）；`pty.start(\"sleep 10\")` 之类不是 DB（判官曾误伤，已收敛）。"""
    return path.endswith(".db")


def scan_file(text):
    """返回 [(行号, db路径)] —— 未密闭的 start() 点。"""
    lines = text.splitlines()
    offenders = []
    for i, line in enumerate(lines):
        for m in START_RE.finditer(line):
            db = m.group(1)
            if not is_persistent_db(db):
                continue
            ok = False
            for j in range(i):
                if db not in lines[j]:
                    continue
                lo = max(0, j - NEAR)
                hi = min(i, j + NEAR + 1)
                if any(CLEAN_RE.search(lines[k]) for k in range(lo, hi)):
                    ok = True
                    break
            if not ok:
                offenders.append((i + 1, db))
    return offenders


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    tests_dir = os.path.join(root, "tests")
    if not os.path.isdir(tests_dir):
        print("PASS: 无 tests/ 目录（跳过）")
        return 0

    checked = 0
    bad = []
    info = []
    for dirpath, _dirs, files in os.walk(tests_dir):
        for fn in sorted(files):
            if not fn.endswith(".cpp"):
                continue
            p = os.path.join(dirpath, fn)
            try:
                with open(p, encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            if not any(is_persistent_db(m.group(1)) for m in START_RE.finditer(text)):
                # 动态路径（start(db_var, ...)）：静态看不到，给 INFO 提示人工复核
                if DYN_START_RE.search(text) and ".db" in text:
                    info.append(os.path.relpath(p, root))
                continue
            checked += 1
            for ln, db in scan_file(text):
                bad.append((os.path.relpath(p, root), ln, db))

    if bad:
        print("FAIL: 以下 CronScheduler 测试用固定路径 DB 但启动前未清除 ⇒ 非密闭"
              "（到期残留会被 ticker 补跑，断言随库状态漂）")
        for rel, ln, db in bad:
            print('  - %s:%d  start("%s") 前未见 remove/unlink("%s")' % (rel, ln, db, db))
        print("\n修法：main() 开头清除该路径（连同 -wal/-shm 更佳）。参考 "
              "tests/unit/test_cron_trigger_lock.cpp 的密闭化注释与复现口令。")
        return 1

    print("PASS: %d 个含 start(<持久化 .db>) 的测试均已密闭（启动前清除）" % checked)
    for rel in info:
        print("  INFO 动态路径需人复核：%s（start(<变量>) 且文件内含 .db 字面量）" % rel)
    return 0


if __name__ == "__main__":
    sys.exit(main())
