#!/usr/bin/env python3
"""v0.53.96 判官：测试间**固定资源冲突**扫描（端口 / 硬编码数据文件）。

背景（R83 实测）：`-j4` 并行下 e2e 互相抢端口 → 一方 `EADDRINUSE` 崩或 SKIP 掉
（丢覆盖）。复现：sweep 占住 1234 时 `test_shutdown_notice.js` 直接崩
（`syscall:'listen', port:1234`）。当时 **3 个 e2e 默认都用 1234**、2 个都用 8793。

判据（只报**默认值重复**，不报 env 可覆盖性）：
  · 每个测试文件的"绑定端口"集合，若同一端口出现在 ≥2 个文件 → 报红
  · 每个测试文件的"硬编码数据文件路径"（*.db / *.sqlite）同理
白名单：`/tmp/thin_e2e.log`（共享日志，故意）。

用法：`python3 scripts/check_test_resource_collisions.py [--root DIR]`
退出码：0 无冲突；1 有冲突。
"""
import argparse, collections, os, re, sys

# 共享是**故意**的资源：共享日志文件（多测试写同一日志便于排障）
ALLOW = {"/tmp/thin_e2e.log"}

PORT_PATTERNS = [
    re.compile(r'^\s*(?:MOCK_)?PORT\s*=\s*(\d{4,5})\s*$', re.M),          # PORT = 18793
    re.compile(r'^\s*(?:MOCK_)?PORT\s*=\s*int\(os\.environ\.get\([^,]+,\s*["\'](\d{4,5})["\']'),
    re.compile(r'^\s*(?:MOCK_)?PORT\s*=\s*Number\(process\.env\.[A-Z_]+\s*\|\|\s*(\d{4,5})'),
    re.compile(r'^\s*const (?:MOCK_)?PORT\s*=\s*(\d{4,5})\s*;', re.M),
    re.compile(r'htons\((\d{4,5})\)'),
]
PATH_PATTERNS = [
    re.compile(r'"((?:/tmp|/root|\.)[^"\s]*\.(?:db|sqlite))"'),
    re.compile(r"'((?:/tmp|/root|\.)[^'\s]*\.(?:db|sqlite))'"),
]


def scan(root):
    ports, paths = collections.defaultdict(set), collections.defaultdict(set)
    files = []
    for sub in ("tests/unit", "tests/e2e"):
        d = os.path.join(root, sub)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if f.endswith((".cpp", ".py", ".js")):
                files.append(os.path.join(d, f))
    for fp in files:
        name = os.path.relpath(fp, root)
        try:
            src = open(fp, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        for pat in PORT_PATTERNS:
            for m in pat.finditer(src):
                ports[m.group(1)].add(name)
        for pat in PATH_PATTERNS:
            for m in pat.finditer(src):
                p = m.group(1)
                if p not in ALLOW:
                    paths[p].add(name)
    return ports, paths


def scan_random_ranges(root, literal_ports):
    """v0.54.2 (R89): 随机端口区间**与其他测试的字面端口重叠**也属冲突。

    实测：tests/e2e/metrics_e2e.py 用 `random.randint(19100, 19399)` 起服务，而
    unit_agent_api_shutdown 用固定 19101 —— 并行下 ~1/300 概率撞上（撞上即 bind 失败 →
    HTTP 拒绝 → 该 e2e 偶发红）。只扫字面端口看不见这类，故单独一条规则。
    """
    out = []
    tests_dir = os.path.join(root, "tests")
    for dirpath, _dirs, files in os.walk(tests_dir):
        for fn in files:
            if not fn.endswith((".py", ".js", ".cpp", ".sh")):
                continue
            p = os.path.join(dirpath, fn)
            rel = os.path.relpath(p, root)
            try:
                txt = open(p, encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            for m in re.finditer(r"randint\(\s*(\d{4,5})\s*,\s*(\d{4,5})\s*\)", txt):
                lo, hi = int(m.group(1)), int(m.group(2))
                for port_s, users in literal_ports.items():
                    try:
                        port = int(port_s)
                    except (TypeError, ValueError):
                        continue
                    if lo <= port <= hi and any(rel not in u for u in users):
                        out.append((rel, lo, hi, port, sorted(users)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = ap.parse_args()
    ports, paths = scan(args.root)

    bad = 0
    for p, users in sorted(ports.items()):
        if len(users) > 1:
            bad += 1
            print("COLLISION port %s: %s" % (p, ", ".join(sorted(users))))
    for p, users in sorted(paths.items()):
        if len(users) > 1:
            bad += 1
            print("COLLISION path %s: %s" % (p, ", ".join(sorted(users))))

    n_scanned = len(ports) + len(paths)
    ranges = scan_random_ranges(args.root, ports)
    for rel, lo, hi, port, users in ranges:
        print("COLLISION random-range %s: [%d,%d] 覆盖 %d（%s 使用）——随机端口会与他测试撞车"
              % (rel, lo, hi, port, ", ".join(users)))
    bad = bad or bool(ranges)
    if bad:
        print("static_test_resource_collision FAIL: %d 处冲突（-j4 并行会互相踩）" % bad)
        return 1
    print("static_test_resource_collision PASS（扫 %d 个固定资源，无跨测试重复）" % n_scanned)
    return 0


if __name__ == "__main__":
    sys.exit(main())
