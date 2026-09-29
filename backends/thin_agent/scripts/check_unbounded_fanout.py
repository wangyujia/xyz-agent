#!/usr/bin/env python3
"""v0.54.2 (R89) 判官：**单请求内重活/扇出无上限**家族普查（"界"清单）。

背景（R86/R88 两轮实锤）：
  - R86 `goal_auto_reason()` 对每个 goal 串行跑完整 chat/FC 管线，无上限 ⇒ 实测单请求 42~44s
  - R88 `agent_debate` 的 roles×rounds 全由请求体决定 ⇒ roles=[...1000]×rounds=1000 可驱动
    百万级 `std::async` 全流程
  - R89 `agent_decompose*` 的 `max_concurrent = req.value(...)` 客户端直接可控且无上限

判据：扫描 src/core/*.cpp 里**循环体内含重活调用**（handle_request / std::async / spawn_agent
等）的循环，并判断其**上界是否来自请求体**（req.value / req[ / roles_json / tasks / *_json）。
凡"请求体驱动"的循环，必须在 ALLOWLIST 里登记（附理由：上限在哪、如何钳制），否则判红——
这样新增"客户端可控的无上限循环"无法静默上线。

用法：python3 scripts/check_unbounded_fanout.py [--root .] [--selftest]
"""
import argparse
import ast
import os
import re
import sys

HEAVY = re.compile(r"handle_request\(|std::async\(|spawn_agent|run_chat\(|execute_chat\(")
LOOP = re.compile(r"^\s*(for|while)\s*\(")
REQ_DRIVEN = re.compile(r"req\.value\(|req\[|roles_json|tasks\b|_json\b|max_concurrent")

# 已登记（含理由）——**新增项必须人工确认上限在哪里**
ALLOWLIST = [
    ("src/core/AgentServiceWs.cpp", "for (auto& rj : roles_json) {",
     "v0.54.1 R88: roles 已由 clamp_debate_fanout 钳到 8（THIN_AGENT_DEBATE_MAX_ROLES）"),
    ("src/core/AgentServiceWs.cpp", "for (int round = 0; round < rounds; ++round) {",
     "v0.54.1 R88: rounds 已钳到 5（THIN_AGENT_DEBATE_MAX_ROUNDS）"),
    ("src/core/AgentService.cpp", "for (const auto& goal : to_process) {",
     "v0.53.99 R86: to_process 已钳到 3（THIN_AGENT_GOAL_AUTO_REASON_MAX）"),
    ("src/core/AgentServiceWs.cpp", "while (!pending.empty()) {",
     "v0.52.4: pending 来自 LLM 分解/存储 meta（非直接请求体）；并发由 max_concurrent 控制，"
     "R89 已把 max_concurrent 钳到 8（上限内波浪推进）"),
    ("src/core/AgentServiceTools.cpp", "for (const auto& t : tasks) {",
     "v0.54.2 R89: 工具 delegate_task 的 tasks 来自 LLM 参数——已钳到 ≤8 项、每批并发 ≤4、"
     "每项 ≤12 轮 / ≤300s（THIN_AGENT_DELEGATE_MAX_TASKS/CONCURRENT 可调）"),
]


def scan(root: str):
    hits = []
    core = os.path.join(root, "src", "core")
    for fn in sorted(os.listdir(core)):
        if not fn.endswith(".cpp"):
            continue
        path = os.path.join(core, fn)
        rel = os.path.relpath(path, root)
        lines = open(path, encoding="utf-8").read().split("\n")
        i = 0
        while i < len(lines):
            if not LOOP.match(lines[i]):
                i += 1
                continue
            # v0.54.2: 语句型循环（header 无花括号，如 `for (..) req[k]=v;`）只取该行，
            # 否则会把后续函数体吞进来造成假阳性（实测 AgentService.cpp chat() 的 meta 拷贝）。
            if "{" not in lines[i]:
                i += 1
                continue
            depth, started, body, j = 0, False, [], i
            while j < len(lines) and j < i + 120:
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
            if HEAVY.search(b):
                ctx = "\n".join(lines[max(0, i - 6):i + 1])
                hits.append({
                    "file": rel,
                    "line": i + 1,
                    "header": lines[i].strip(),
                    "heavy": sorted(set(HEAVY.findall(b))),
                    "req_driven": bool(REQ_DRIVEN.search(ctx)),
                    "body_lines": len(body),
                })
            i = max(i + 1, j + 1)
    return hits


def in_allowlist(hit):
    for f, header, _reason in ALLOWLIST:
        if hit["file"] == f and hit["header"] == header:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    args = ap.parse_args()
    hits = scan(args.root)
    print("== 单请求内重活/扇出普查（%d 处含重活的循环）==" % len(hits))
    bad = []
    for h in hits:
        tag = "REQ-DRIVEN" if h["req_driven"] else "internal  "
        state = "已登记" if in_allowlist(h) else ("**未登记**" if h["req_driven"] else "n/a")
        print("  %s %s:%d  %-58s %s" % (tag, h["file"], h["line"], h["header"][:58], state))
        if h["req_driven"] and not in_allowlist(h):
            bad.append(h)
    if bad:
        print("FAIL: 以下 REQ-DRIVEN 重活循环未登记上限理由：")
        for h in bad:
            print("   - %s:%d %s" % (h["file"], h["line"], h["header"]))
        return 1
    print("PASS: 所有请求体驱动的重活循环都已登记上限（上限变更须同步 ALLOWLIST 与说明）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
