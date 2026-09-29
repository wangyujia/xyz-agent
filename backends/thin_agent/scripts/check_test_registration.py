#!/usr/bin/env python3
"""v0.54.5 (R92) 判官：**测试文件是否被 CI 引用**（棘轮式，冻结存量）。

背景（实测）：`tests/e2e/` 下有 pytest 用例**既没注册进 CMake/ctest，也不在 scripts/test.sh 里**
（test.sh 无 pytest 调用）⇒ 它们**从未被任何自动化跑到**：
  - `test_spawn_nesting_limit.py` 已**静默腐烂**（断言"护栏在第 4 层拦截"，实际先 `turns_exhausted`
    结束 → 长期红无人知）
  - `test_decompose_route.py` / `test_checkpoint_resume_hooks.py` **依赖真实 home 的既有状态**
    （hooks.json 需预先存在、`fc_runs` 表由历史运行创建）——隔离 THIN_AGENT_HOME 后立即变红，
    证明它们从未在干净环境验证过
判据（棘轮）：**新**测试文件必须在 CMakeLists（ctest）或 scripts/*.sh 里被引用，否则判红；
存量未登记项冻结在 BACKLOG 里（含证据），只提示不判红——防止"新增了没人跑的测试"。
"""
import argparse
import os
import re
import sys

BACKLOG = {
    "tests/demo/ws_chat_manual.py": "手工演示工具（非自动化用例）",
    "tests/ws_client/ws_client.py": "交互式客户端（scripts/test.sh 以目录方式调用）",
    "tests/ws_client/ws_repl.py": "交互式 REPL（同上）",
    "tests/ws_client/ws_smoke.py": "手工冒烟脚本（同上）",
}

# v0.54.11~v0.54.14 已解析项（从 BACKLOG 移除，留档以免后人重复调查）：
#   - tests/e2e/test_spawn_nesting_limit.py  → v0.54.11 修断言腐烂 + 登记（e2e_spawn_nesting_limit）
#   - tests/e2e/test_plugin_split.py         → v0.54.12 隔离 home + 登记（e2e_plugin_split）
#   - tests/e2e/metrics_unit_test.py         → v0.54.13 判定为「腐烂的重复覆盖」：自 v0.53.4
#     AgentService 构造签名变更后就编译不过；其断言已由 e2e_metrics 覆盖，纯单元角度已迁移进
#     tests/unit/test_agent_service.cpp 第 12 段 ⇒ 删除脚本（旧文件与 home 污染面一并消失）
#   - tests/e2e/test_smoke_and_safety.py     → v0.54.13 登记为 e2e_smoke_real（TA_SMOKE_REAL 门控，
#     真模型 e2e 默认 SKIP；实测 7 passed / 120.37s）
#   - tests/e2e/test_decompose_route.py      → v0.54.14 自足化（home 参数化）+ 登记
#     （e2e_decompose_route）；R92 记的"依赖真实 home 既有状态"实测为**路径硬编码的症状**
#   - tests/e2e/test_checkpoint_resume_hooks.py → v0.54.14 同上（e2e_checkpoint_resume_hooks）

HELPER_NAMES = {"conftest.py", "__init__.py", "helpers.py", "test_macros.h"}

# 判官自身的**内容标记**：排除"判官副本/改名版"充当引用来源（v0.54.13 实测的第三处假绿——
# 见 referenced_text 注释）。用判官独有的输出文案而非文件名，改名复制后依然可辨识。
JUDGE_MARKER = "测试登记普查"


def collect_tests(root):
    out = []
    for dirpath, dirnames, files in os.walk(os.path.join(root, "tests")):
        dirnames[:] = [d for d in dirnames if d != "__pycache__"]
        for fn in files:
            if fn.endswith((".py", ".js", ".cpp", ".sh")) and fn not in HELPER_NAMES:
                out.append(os.path.relpath(os.path.join(dirpath, fn), root))
    return sorted(out)


def strip_comments(text):
    """去掉注释内容（整行 `#` 开头的直接丢；行内引号外的 `#` 起到行尾）。

    为什么必须去（R93 实测的判官假绿）：本判官的判据是"文件名出现在 CMakeLists/scripts 文本里
    ⇒ 该测试被 CI 引用"，于是**注释里的提及**会被当成登记——实测在 CMake 注释里写了一句
    "`test_plugin_split.py` 的宽匹配 SIGKILL"，该文件立刻从"未登记"清单里消失（10→8），
    而它其实依旧没被任何 ctest 引用。判官的判据必须只认**可执行文本**。
    """
    out = []
    for line in text.splitlines():
        if line.strip().startswith("#"):
            continue
        res, quote = [], None
        for ch in line:
            if quote:
                res.append(ch)
                if ch == quote:
                    quote = None
                continue
            if ch in ('"', "'"):
                quote = ch
                res.append(ch)
                continue
            if ch == "#":
                break
            res.append(ch)
        out.append("".join(res))
    return "\n".join(out)


def strip_docstrings(text):
    """去掉三引号字符串块（Python docstring/长字符串）。

    第二层假绿（同轮实测）：`check_no_broad_kill.py` 的 docstring 里引用了
    `tests/e2e/test_plugin_split.py::_kill_stale` 作说明——那又是"提及不是登记"。
    """
    out, in_triple = [], None
    for line in text.splitlines():
        s = line.strip()
        if in_triple:
            if in_triple in s:
                in_triple = None
            continue
        for q in ('"""', "'''"):
            if s.startswith(q):
                if s.count(q) < 2:
                    in_triple = q
                break
        else:
            out.append(line)
    return "\n".join(out)


def cmake_command_args(path):
    """只取 CMake **命令的参数文本**（add_test/add_executable 的括号内）。

    判据收紧到"真的被 CMake 调用"——注释、message() 文案、自定义判官的 docstring 一律不算。
    """
    import re as _re
    text = open(path, encoding="utf-8", errors="ignore").read()
    chunks = []
    for m in _re.finditer(r"\b(add_test|add_executable)\s*\(", text):
        i, depth = m.end(), 1
        while i < len(text) and depth:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        chunks.append(text[m.end():i - 1])
    return "\n".join(chunks)


def referenced_text(root):
    """CMake 命令参数 + scripts/*.sh|py 的**可执行文本**（去注释、去 docstring）"""
    text = ""
    p_cmake = os.path.join(root, "CMakeLists.txt")
    if os.path.exists(p_cmake):
        text += cmake_command_args(p_cmake)
    sdir = os.path.join(root, "scripts")
    if os.path.isdir(sdir):
        for fn in os.listdir(sdir):
            p = os.path.join(sdir, fn)
            # 排除判官自身：它内部 BACKLOG 登记了未跑文件的名字，会造成"自指→看似被引用"的假绿
            # v0.54.13: 由"按文件名排除"改为"**按内容标记排除**"——判官自测实测的第三个假绿：
            # 把判官复制成 scripts/_probe_judge.py（任何改名/副本/带后缀的变体）时文件名不再匹配，
            # 副本的 BACKLOG 文本立刻被当成"引用来源"⇒ 全部未登记项"看起来都登记了"（存量 6→0，
            # 且什么都查不出来）。判据必须只认"非判官脚本的可执行文本"。
            if not os.path.isfile(p) or not fn.endswith((".sh", ".py")):
                continue
            raw = open(p, encoding="utf-8", errors="ignore").read()
            if JUDGE_MARKER in raw:
                continue
            text += strip_docstrings(strip_comments(raw)) if fn.endswith(".py") \
                else strip_comments(raw)
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    args = ap.parse_args()
    ref = referenced_text(args.root)
    tests = collect_tests(args.root)
    unref_backlog, unref_new = [], []
    for t in tests:
        base = os.path.basename(t)
        if base in ref or t in ref:
            continue
        (unref_backlog if t in BACKLOG else unref_new).append(t)
    # v0.54.13: BACKLOG 自身也会腐烂——已登记的项仍留在表里会误导后人（"它还没跑"），
    # 文件已删除的项则永远查不到。二者均**只提示不判红**（清理动作留给人，避免判官误删证据）。
    stale = []
    for t, why in BACKLOG.items():
        if not os.path.exists(os.path.join(args.root, t)):
            stale.append((t, "文件已不存在（条目应删除）"))
        elif os.path.basename(t) in ref or t in ref:
            stale.append((t, "**已登记进 CI**（条目应删除）"))
    print("== 测试登记普查：共 %d 个测试文件，未被 CI 引用 %d 个（存量 %d / 新增 %d）"
          % (len(tests), len(unref_backlog) + len(unref_new), len(unref_backlog), len(unref_new)))
    for t, why in stale:
        print("  [BACKLOG 过期] %s —— %s" % (t, why))
    for t in unref_backlog:
        print("  [存量·已冻结] %s\n        %s" % (t, BACKLOG[t]))
    for t in unref_new:
        print("  [**新增未登记**] %s —— 必须注册进 ctest 或 scripts/*.sh，否则永远不会被执行"
              % t)
    if unref_new:
        print("FAIL: 有新增测试文件未被任何 CI 引用（测试写完没人跑 = 会腐烂）")
        return 1
    print("PASS: 无新增未登记测试（存量 %d 项已冻结在 BACKLOG，仅提示）" % len(unref_backlog))
    return 0


if __name__ == "__main__":
    sys.exit(main())
