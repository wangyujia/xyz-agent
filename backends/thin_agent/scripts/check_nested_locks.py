#!/usr/bin/env python3
"""同互斥量嵌套获取扫描器（"补锁前置三问"第一问的机械化判官）。

背景（v0.53.82 实锤）：v0.53.77 提交信息写着"Ws 五处补锁(无嵌套核查)"——
被补锁的 5 处（ping/action/task_submit/task_cancel/task_replay）**已在
mu_ 临界区内**，于是同一 std::mutex（非递归）被同一线程二次获取 =
首次调用即自死锁、整个 AgentService 永久冻结（mutex owner==自身 TID,
gdb 实锤）。同一提交的过程实锤① 早就记录了"误补锁=同线程二次 lock 自死锁"
（L262），却没对被改的 5 处做同样核查 —— 修一漏一的教科书案例。

本扫描器把该核查变成机械判据，接入 ctest（期望 0 命中）：
同一作用域链内，同一互斥量表达式被二次获取（且非 recursive_mutex）。

判据实现：分子作用域栈——每个 `{` 压一层锁列表，`}` 弹层（连带丢弃该层
获取的锁）；"当前持有集" = 栈内所有层的并集。unique_lock 中途 unlock()
显式摘除。仅作语法级判据，注释/字符串已剥离。
"""
import os
import re
import sys


GUARD_RE = re.compile(
    r"std::(lock_guard|unique_lock)\s*<\s*std::(recursive_)?mutex\s*>\s*"
    r"(\w+)\s*[({]\s*([A-Za-z_][\w\.\->]*)")
RAW_LOCK_RE = re.compile(r"([A-Za-z_][\w\.\->]*)\.lock\(\)")
UNLOCK_RE = re.compile(r"([A-Za-z_][\w\.\->]*)\.unlock\(\)")
# lock_guard<mutex> lk(...) 也支持无 std:: 前缀的简写（本项目统一带 std::，兜底）
LOOSE_GUARD_RE = re.compile(
    r"(?<!std::)(lock_guard|unique_lock)\s*<\s*mutex\s*>\s*(\w+)\s*[({]\s*([A-Za-z_][\w\.\->]*)")


def strip_code(line: str) -> str:
    line = re.sub(r"//.*$", "", line)
    line = re.sub(r"'(\\.|[^'])*'", "''", line)
    line = re.sub(r'"(\\.|[^"])*"', '""', line)
    return line


def scan_text(text: str):
    hits = []
    stack = [[]]                       # 作用域栈：每层 = [(锁变量名, 互斥量表达式)]
    for i, raw in enumerate(text.split("\n"), 1):
        code = strip_code(raw)
        # 1) 显式 unlock 摘除（可能跨层——按变量名全局摘）
        for var in UNLOCK_RE.findall(code):
            for layer in stack:
                layer[:] = [e for e in layer if e[0] != var]
        # 2) 本行事件流：锁获取 / 括号开合——**按字符序**结算，单行 `{...}`
        #    （如一行析构体）内的锁必须归属内层，否则会误挂到外层永不到期
        events = []
        for mo in GUARD_RE.finditer(code):
            events.append((mo.start(), "lock", (mo.group(3), mo.group(4), bool(mo.group(2)))))
        for mo in LOOSE_GUARD_RE.finditer(code):
            events.append((mo.start(), "lock", ("<raw>", mo.group(3), False)))
        for mo in RAW_LOCK_RE.finditer(code):
            events.append((mo.start(), "lock", ("<raw>", mo.group(1), False)))
        for k, ch in enumerate(code):
            if ch == "{":
                events.append((k, "open", None))
            elif ch == "}":
                events.append((k, "close", None))
        for _, kind, payload in sorted(events, key=lambda e: e[0]):
            if kind == "open":
                stack.append([])
            elif kind == "close":
                if len(stack) > 1:
                    stack.pop()
            else:
                var, mtx, is_rec = payload
                held = {m for layer in stack for (_, m) in layer}
                if not is_rec and mtx in held:
                    hits.append((i, mtx, raw.strip()[:110]))
                stack[-1].append((var, mtx))
    return hits


# ── 规则二：持锁作用域内调用"已知会自行加同一把锁"的函数 ──
# 背景（v0.53.84 实锤）：AgentServiceWs 的 handler 分支若在**外层作用域**持锁，
# 又 `return finish(...)`——而 finish lambda 自身也要锁 mu_ → 同线程二次 lock
# = 自死锁、全服务永久冻结（get_project / set_project_mode 实测）。
# 文本级作用域栈看不见"跨函数"的第二次加锁，故用显式"已知加锁被调方"名单补盲。
KNOWN_LOCKING_CALLEES = {
    "finish": "mu_",              # AgentServiceWs handle_request 内的 finish lambda
}
CALL_RE = re.compile(r"\b(finish)\s*\(")


def scan_locked_calls(text: str):
    """持锁作用域内调用已知加锁函数 → 命中。"""
    hits = []
    stack = [[]]
    for i, raw in enumerate(text.split("\n"), 1):
        code = strip_code(raw)
        for var in UNLOCK_RE.findall(code):
            for layer in stack:
                layer[:] = [e for e in layer if e[0] != var]
        events = []
        for mo in GUARD_RE.finditer(code):
            events.append((mo.start(), "lock", (mo.group(3), mo.group(4), bool(mo.group(2)))))
        for mo in LOOSE_GUARD_RE.finditer(code):
            events.append((mo.start(), "lock", ("<raw>", mo.group(3), False)))
        for mo in CALL_RE.finditer(code):
            events.append((mo.start(), "call", (mo.group(1),)))
        for k, ch in enumerate(code):
            if ch == "{":
                events.append((k, "open", None))
            elif ch == "}":
                events.append((k, "close", None))
        for _, kind, payload in sorted(events, key=lambda e: e[0]):
            if kind == "open":
                stack.append([])
            elif kind == "close":
                if len(stack) > 1:
                    stack.pop()
            elif kind == "lock":
                var, mtx, is_rec = payload
                stack[-1].append((var, mtx))
            else:
                (callee,) = payload
                need = KNOWN_LOCKING_CALLEES.get(callee)
                if need is None:
                    continue
                held = {m for layer in stack for (_, m) in layer}
                if need in held and callee not in code_hash_guard(code):
                    # 契约：第 2 字段必须是**互斥量名**（main 用它做锁种类分流）
                    hits.append((i, need,
                                 "%s 持锁调用会再锁 %s | %s" % (callee, need, raw.strip()[:90])))
    return hits


def code_hash_guard(code: str):
    return ()  # 预留：如需豁免名单在此扩展


# ── 规则三：同文件内"会加锁的函数"被持同锁处调用（跨函数盲区全仓推广）──
# 规则二只登记了 finish（AgentServiceWs）。规则三自动建立"函数名 → 它锁的互斥量"
# 表，再扫"持有同一互斥量时调用该函数"= 同一死法（v0.53.84 家族推广）。
FUNC_DEF_RE = re.compile(r"^[A-Za-z_][\w:<>,&*\s]*?\b(\w+)\s*\([^;{]*\)\s*(?:const)?\s*\{", re.M)


def build_locking_functions(text: str):
    """扫出"函数体内直接 lock 了某互斥量变量"的函数名 → {name: set(mutex_expr)}。"""
    table = {}
    for mo in FUNC_DEF_RE.finditer(text):
        name = mo.group(1)
        if name in ("if", "for", "while", "switch", "catch"):
            continue
        # 取该函数体（花括号配平）
        i = text.find("{", mo.end() - 1)
        depth, j = 0, i
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = text[i:j]
        mtx = set()
        for g in GUARD_RE.finditer(body):
            mtx.add(g.group(4))
        for g in LOOSE_GUARD_RE.finditer(body):
            mtx.add(g.group(3))
        if mtx:
            table.setdefault(name, set()).update(mtx)
    return table


def _enclosing_func_name(text: str, line_no: int) -> str:
    """粗略定位某行所处的函数名（向前找最近的函数定义头）。"""
    lines = text.split("\n")[:line_no]
    for k in range(len(lines) - 1, max(-1, len(lines) - 400), -1):
        mo = FUNC_DEF_RE.match(lines[k].strip())
        if mo:
            return mo.group(1)
    return ""


def scan_locked_calls_auto(text: str, table: dict, known: dict = None):
    """持锁作用域内调用同文件"会加同一把锁"的函数 → 命中。"""
    known = dict(known or {})
    for k, v in table.items():
        known.setdefault(k, set()).update(v)
    hits = []
    stack = [[]]
    for i, raw in enumerate(text.split("\n"), 1):
        code = strip_code(raw)
        for var in UNLOCK_RE.findall(code):
            for layer in stack:
                layer[:] = [e for e in layer if e[0] != var]
        events = []
        for mo in GUARD_RE.finditer(code):
            events.append((mo.start(), "lock", (mo.group(3), mo.group(4))))
        for mo in LOOSE_GUARD_RE.finditer(code):
            events.append((mo.start(), "lock", ("<raw>", mo.group(3))))
        for mo in re.finditer(r"(?<![.>:])\b(\w+)\s*\(", code):
            # 排除 obj.size() / a->clear() / std::filesystem::remove() 这类限定调用
            # （否则同文件里恰好同名函数会把容器方法误判成"会加锁的被调方"）
            events.append((mo.start(), "call", (mo.group(1),)))
        for k, ch in enumerate(code):
            if ch == "{":
                events.append((k, "open", None))
            elif ch == "}":
                events.append((k, "close", None))
        for _, kind, payload in sorted(events, key=lambda e: e[0]):
            if kind == "open":
                stack.append([])
            elif kind == "close":
                if len(stack) > 1:
                    stack.pop()
            elif kind == "lock":
                var, mtx = payload
                stack[-1].append((var, mtx))
            else:
                (callee,) = payload
                need = known.get(callee)
                if not need:
                    continue
                held = {m for layer in stack for (_, m) in layer}
                inter = need & held
                if inter and callee != _enclosing_func_name(text, i):
                    # 自递归豁免：函数体内调用同名函数（容器 clear/erase/size 等
                    # 自身方法）不是跨函数嵌套
                    hits.append((i, ",".join(sorted(inter)),
                                 "%s 持锁调用会再锁 %s | %s" % (
                                     callee, ",".join(sorted(inter)), raw.strip()[:90])))
    return hits


# ── 锁种类感知（v0.53.85）──
# recursive_mutex 的"同线程二次 lock"是**合法**的（cron v0.53.4 有意设计：
# ticker 持锁扫描时要能在锁内回调 execute_task）。但这类嵌套仍有"锁内重活"
# 风险（recursive 锁跨整个任务执行 → 其他线程的 list/stats/ticker 全被挡住），
# 故单列为 INFO（提示复核锁内重活），不计入 FAIL。
RECURSIVE_DECL_RE = re.compile(r"std::recursive_mutex\s+(\w+)")


def recursive_mutex_names(files):
    names = set()
    for f in files:
        try:
            names.update(RECURSIVE_DECL_RE.findall(open(f, encoding="utf-8", errors="replace").read()))
        except OSError:
            pass
    return names

def main():
    roots = sys.argv[1:] or ["src", "include"]
    # 收集全部源文件（一遍），用于判定互斥量种类——判定必须整仓看：同名变量
    # 在不同类里可能是 mutex 或 recursive_mutex（cron mu_ 即 recursive）
    all_files = []
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames
                           if d not in ("third_party", "build", "build-release", ".git")]
            for f in sorted(filenames):
                if f.endswith((".cpp", ".h")):
                    all_files.append(os.path.join(dirpath, f))
    rec_names = recursive_mutex_names(all_files)

    total = 0
    infos = []
    for p in all_files:
        text = open(p, encoding="utf-8", errors="replace").read()
        tbl = build_locking_functions(text)
        for (ln, mtx, txt) in (list(scan_text(text))
                               + list(scan_locked_calls(text))
                               + list(scan_locked_calls_auto(text, tbl))):
            # 锁种类分流：recursive_mutex 同线程可重入 → 合法（非死锁），
            # 但"持 recursive 锁跨重活"仍要人复核 → INFO 不计 FAIL
            if any(mtx == n or mtx.endswith("." + n) or mtx.endswith("->" + n)
                   for n in rec_names):
                infos.append("%s:%d  [INFO 可重入锁 %s]  %s" % (p, ln, mtx, txt))
                continue
            total += 1
            print("%s:%d  [二次获取 %s]  %s" % (p, ln, mtx, txt))
    for line in infos:
        print(line)
    if total:
        print("FAIL: 同互斥量嵌套获取 %d 处（非递归 mutex 二次 lock = 自死锁）" % total)
        return 1
    print("PASS: 无同互斥量嵌套获取（INFO 可重入锁 %d 处，需人复核锁内重活）" % len(infos))
    return 0


if __name__ == "__main__":
    sys.exit(main())
