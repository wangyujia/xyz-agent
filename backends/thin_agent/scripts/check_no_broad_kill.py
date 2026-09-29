#!/usr/bin/env python3
"""机械判官：测试/脚本**不得宽匹配杀进程**（R93 基建地雷清理，v0.54.10）。

为什么（两处真实地雷，均为"会制造假故障"的隐患）：
  1) `scripts/demo_smoke_all.sh::cleanup_procs` 旧实现
        pids=$(pgrep -f "build/thin_agent"); kill $pids
     —— 同机上**任何** thin_agent 进程都会被装进去：并行跑的 e2e/ctest 服务实例、开发者手起的
     服务，全部照杀。表现为"服务莫名消失 / 端口冲突"的假故障，而且**只在并行时出现**（单跑看不出）。
  2) `tests/e2e/test_plugin_split.py::_kill_stale` 旧实现
        pgrep -f "port 18793" → 逐个 SIGKILL
     —— 宽匹配：命令行里含 "port 18793" 的任何进程（别的工具、隧道、他人实例）都会中招，且杀之前
     不看是谁。

判据（两条，均为硬红）：
  A. 禁止 `pkill` / `killall` / `fuser -k`（全局杀器：模式再窄也可能命中无关进程）。
  B. 凡"把 pgrep 结果拿去 kill"的文件，其 **pgrep 模式必须带辨识参数**（含 `--`，如 `--port`）——
     只按二进制名/端口号文本匹配不算窄（旧实现正是这种形态）。
     例外：pgrep 仅用于**计数/探测**（文件内无杀进程动作）时可自由写。

用法：python3 scripts/check_no_broad_kill.py --root .
退出码：0=通过；1=有宽匹配杀进程。
"""
import argparse
import os
import re
import sys

# 全局杀器：任何出现即判红（含注释/字符串——宁可让人显式写进 ALLOWLIST 并说明理由）
GLOBAL_KILLERS = ["pkill", "killall", "fuser -k"]

# "把动态找到的进程杀掉"的形态（shell / python / cpp）
KILL_OF_LIST_RE = re.compile(
    r"(kill\s+\$\(?pgrep"
    r"|kill\s+\$pids"
    r"|pgrep[^\n]*\|\s*xargs[^\n]*kill"
    r"|os\.kill\(\s*int\("
    r"|for\s+pid\s+in[^\n]*\n[^\n]*os\.kill)",
    re.S,
)
PGREP_RE = re.compile(r"pgrep\s+(?:-[A-Za-z]+\s+)*['\"]([^'\"]+)['\"]")
# Python 列表形式：subprocess.run(["pgrep", "-af", f"build/thin_agent.*--port {PORT}"])
# （旧地雷 test_plugin_split.py 正是这种形态——第一版判官只认 shell 串，漏判了它）
PGREP_LIST_RE = re.compile(
    r"[\"']pgrep[\"']\s*,\s*(?:[\"']-[A-Za-z]+[\"']\s*,\s*)*[frbu]?[\"']([^\"']+)[\"']")
# 变量式模式：subprocess.run(["pgrep", "-af", pat]) —— 模式在别处定义，静态看不到
PGREP_DYN_RE = re.compile(
    r"[\"']pgrep[\"']\s*,\s*(?:[\"']-[A-Za-z]+[\"']\s*,\s*)*([A-Za-z_][A-Za-z_0-9]*)"
    r"\s*[,\)]")


def pgrep_patterns(line):
    """抽取 pgrep 的**字面量**模式（剔除 `-af` 这类短选项被误当成模式的情形）。"""
    pats = [m.group(1) for m in PGREP_RE.finditer(line)]
    pats += [m.group(1) for m in PGREP_LIST_RE.finditer(line)]
    return [p for p in pats if not p.startswith("-")]

# 存量豁免（需写明理由；当前应为空）
ALLOWLIST = {}

SCAN_DIRS = ["tests", "scripts"]
EXTS = (".py", ".sh", ".bash", ".cpp", ".h", ".js", ".mjs")


def _code_lines(text):
    """返回"非注释、非 docstring"的行 (行号, 内容)。

    为什么要剔除：判官本身要把 `pkill` 这类词写进规则表，文档里也常引用"旧实现长什么样"
    （如 `pgrep -f "port 18793"`）——那都不是**可执行动作**。不剔除就会把规则表和文档判红，
    逼着人删掉说明文字（R93 实测：本判官第一版就自判 13 处）。
    """
    out = []
    in_triple = None
    for i, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if in_triple:
            if in_triple in s:
                in_triple = None
            continue
        for q in ('"""', "'''"):
            if s.startswith(q):
                # 同行闭合（"""doc"""）则不算进入块态
                if s.count(q) < 2:
                    in_triple = q
                break
        else:
            if s.startswith(("#", "//", "*", "/*", "--")):
                continue
            out.append((i, line))
    return out


def scan_file(path):
    out = []
    try:
        text = open(path, encoding="utf-8", errors="ignore").read()
    except OSError:
        return out
    lines = _code_lines(text)
    for tok in GLOBAL_KILLERS:
        for i, line in lines:
            if tok in line:
                out.append((i, "全局杀器 %s：模式再窄也可能命中无关进程——"
                               "改为只杀自己启动/登记的 PID" % tok, "fail"))
    if KILL_OF_LIST_RE.search("\n".join(l for _i, l in lines)):
        for i, line in lines:
            for pat in pgrep_patterns(line):
                if "--" not in pat:
                    out.append((i, "pgrep 模式 %r 不够窄（缺 `--` 级辨识参数，如 `--port`）："
                                   "只按二进制名/端口号文本匹配会误杀同机其它实例" % pat, "fail"))
            # 变量式模式：查同文件里该变量的赋值是否带 -- 级辨识参数；查不到则给 INFO（不判红）
            for m in PGREP_DYN_RE.finditer(line):
                var = m.group(1)
                if re.search(r"\b%s\s*=\s*[^\n]*--" % re.escape(var), text):
                    continue
                out.append((i, "INFO 变量式 pgrep 模式（%s）静态不可见——请人工确认其值含 `--` 级"
                               "辨识参数（如 `--port`）" % var, "info"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    args = ap.parse_args()
    root = os.path.abspath(args.root)

    offenders, infos, checked = [], [], 0
    for d in SCAN_DIRS:
        base = os.path.join(root, d)
        for dirpath, _dirs, files in os.walk(base):
            if "/.git" in dirpath:
                continue
            for fn in sorted(files):
                if not fn.endswith(EXTS):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, fn), root)
                if rel in ALLOWLIST:
                    continue
                if fn == os.path.basename(__file__):
                    # 判官自身：规则表里必然写着 pkill/killall 这些字样（非可执行动作）
                    continue
                checked += 1
                for line_no, reason, kind in scan_file(os.path.join(dirpath, fn)):
                    (infos if kind == "info" else offenders).append((rel, line_no, reason))

    if offenders:
        print("FAIL: 发现宽匹配杀进程 %d 处（宽匹配会误杀同机其它 thin_agent/无关进程，"
              "只在并行时暴露成假故障）：" % len(offenders))
        for rel, line_no, reason in offenders:
            print("  %s:%d  %s" % (rel, line_no, reason))
        print("\n修法：①shell —— 登记自己启动的 PID（STARTED_PIDS+=(\"$!\")）只杀这些；"
              "端口被占就报错，别替用户杀进程 ②python —— pgrep -af \"<二进制路径>.*--port <PORT>\" "
              "并在杀之前打印命令行")
        return 1
    print("PASS: 扫 %d 个文件，无宽匹配杀进程（无 pkill/killall/fuser -k；"
          "pgrep 用法均带 -- 级辨识参数或仅用于计数）" % checked)
    for rel, line_no, reason in infos:
        print("  INFO %s:%d  %s" % (rel, line_no, reason))
    return 0


if __name__ == "__main__":
    sys.exit(main())
