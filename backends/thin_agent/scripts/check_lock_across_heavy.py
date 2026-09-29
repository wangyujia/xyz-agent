#!/usr/bin/env python3
"""v0.54.3 (R90) 判官：**锁跨重活**（持锁执行长耗时操作）普查。

背景（R73 发现，长期挂"待拍板"）：`CronScheduler::trigger_now` 持 `std::recursive_mutex`
跨**整个** `execute_task`（LLM/工具，秒到分钟级）。这类"锁跨重活"不会死锁（recursive 或
不重入），但会把**所有其他持锁路径**阻塞到分钟级——实测关联：R86 的 goal_auto_reason 曾
单请求 42s（串行重活独占 worker）。

判据：扫描持锁作用域（lock_guard / unique_lock / scoped_lock / lock()..unlock()）内是否出现
**长耗时调用**（LLM/HTTP/sleep/execute_task/handle_request/run_chat/AgentLoop）。
凡命中必须登记（ALLOWLIST：说明为何必须持锁，或给出改动方案），否则判红。

用法：python3 scripts/check_lock_across_heavy.py [--root .]
"""
import argparse
import os
import re
import sys

LOCK = re.compile(
    r"(std::)?(lock_guard|unique_lock|scoped_lock)\s*<[^>]*>\s+(\w+)\s*\(|"
    r"\b(mu_|mutex_|_mutex)\w*\.lock\s*\(\s*\)")
HEAVY = re.compile(
    r"execute_task\s*\(|handle_request\s*\(|run_chat\s*\(|AgentLoop\b|"
    r"curl_easy_perform|http_post|http_get|"
    r"sleep_for\s*\(|sleep\s*\(|usleep\s*\(|"
    r"trigger_now\s*\(|\.run\s*\(\s*\)")


def scan_file(path, rel):
    lines = open(path, encoding="utf-8", errors="ignore").read().split("\n")
    hits = []
    for i, l in enumerate(lines):
        if not LOCK.search(l):
            continue
        # 找持锁作用域的结束（brace 配平）
        # v0.54.3: 锁行若**不带花括号**（如 `std::lock_guard<...> lock(mu_);` 在函数体开头），
        # 其作用域是**整个外层块**——此前从 0 起数括号只截到第一个内层 block，导致漏检
        # （实测漏掉 CronScheduler::trigger_now 持锁跑 execute_task）。故 depth 初值取 1。
        depth = 1 if "{" not in lines[i] else 0
        started, body, j = True, [], i   # 无括号锁行也算"已进入作用域"，到外层 } 结束
        while j < len(lines) and j < i + 200:
            for ch in lines[j]:
                if ch == "{":
                    depth += 1
                    started = True
                elif ch == "}":
                    depth -= 1
            body.append(lines[j])
            if started and depth <= 0:
                break
            j += 1
        b = "\n".join(body)
        heavy = sorted(set(HEAVY.findall(b)))
        # v0.54.3: **显式放锁启发式**——作用域内出现 `.unlock()` 说明重活在锁外
        # （实测 v1/v2 gw 的 send_worker 都是"pop 后 unlock 再 HTTP" = 正确写法）。
        if heavy and ".unlock()" in b:
            continue
        if heavy:
            hits.append({
                "file": rel, "line": i + 1, "lock": l.strip()[:70],
                "heavy": heavy, "span": j - i + 1,
            })
    return hits


# 已登记：必须持锁的理由 / 或引用的修复 commit
ALLOWLIST = {
    # v0.54.3 (R90): trigger_now 与 code_exec 的"锁跨重活"**已修复**（收窄临界区），
    # 不再命中；此处保留历史说明以免后人以为从未存在过。
    #   - CronScheduler::trigger_now  → 两段式（锁内读行+登记在飞，锁外 execute_task）
    #   - plugin/skills/code_exec     → 锁只护 g_interpreters 查表（原先 lock_guard 全程持有）
    #
    # v0.54.6 (R91 拍板)：trigger_now 的**每任务执行锁**持锁跨 execute_task 是**设计语义**，
    # 不是本判官要防的缺陷：
    #   · 那是任务级锁（`task_exec_mu_`，每任务一把），只让**同一任务**的并发二次触发排队
    #     （用户拍板"排完队再跑一遍"），不阻塞任何全局 API；
    #   · 全局 `mu_` 仍然**不跨执行**（add/remove/set_enabled/list/stats/stop 只受 mu_ 短暂
    #     保护，不受任务执行时长影响）——R73/R90 的成果未被回退；
    #   · 复核要点：若有人把 `mu_` 挪回来包住执行段（全局阻塞），命中会是**另一行锁**
    #     （`lock(mu_)`），本登记不覆盖，仍会判红。
    ("src/core/CronScheduler.cpp",
     "std::lock_guard<std::recursive_mutex> exec_lock"):
        "每任务执行锁跨 execute_task = 排队重跑语义（v0.54.6 用户拍板）；全局 mu_ 不跨执行",
}


def registered(hit):
    """按 (file, 锁行前缀) 匹配 ALLOWLIST。返回 (已登记?, 说明)"""
    for (f, lock_prefix), why in ALLOWLIST.items():
        if hit["file"] == f and hit["lock"].startswith(lock_prefix):
            return True, why
    return False, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    args = ap.parse_args()
    hits = []
    for base in ("src",):
        for dirpath, _d, files in os.walk(os.path.join(args.root, base)):
            for fn in sorted(files):
                if fn.endswith(".cpp"):
                    p = os.path.join(dirpath, fn)
                    hits.extend(scan_file(p, os.path.relpath(p, args.root)))
    print("== 持锁作用域内含长耗时调用：%d 处 ==" % len(hits))
    unregistered = []
    for h in hits:
        reg, why = registered(h)
        tag = "已登记" if reg else "**未登记**"
        if reg and "待用户拍板" in why:
            tag = "待拍板"
        print("  %s:%d  锁=%-40s 重活=%s（%d 行）  %s"
              % (h["file"], h["line"], h["lock"][:40], ",".join(h["heavy"])[:46], h["span"], tag))
        if reg:
            print("       理由：" + why[:110])
        else:
            unregistered.append(h)
    if unregistered:
        print("FAIL: 以下锁跨重活未登记（须说明为何必须持锁，或收窄临界区）：")
        for h in unregistered:
            print("   - %s:%d %s" % (h["file"], h["line"], h["lock"]))
        return 1
    print("PASS: 所有锁跨重活都已登记（新命中须先决策：收窄临界区 或 登记理由）")
    print("(注：近似扫描——已排除作用域内显式 .unlock() 的写法；命中仍需人工确认)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
