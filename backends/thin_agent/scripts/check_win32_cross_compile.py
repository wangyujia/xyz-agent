#!/usr/bin/env python3
"""v0.54.16 判官：**Windows 段真编译**门禁（MinGW-w64 交叉编译，棘轮式）。

背景（为什么需要它）：`_WIN32` 分支在 Linux CI 里**从不被编译器解析**（无 windows.h）⇒ 属
"从未编译过的代码"，历史上只能靠肉眼预扫（R53-56 预扫抓到 CreatePipe 不 ReadFile 之类 P1）。
v0.54.16 实测发现：WSL 侧的 **MinGW-w64（x86_64-w64-mingw32-g++，带真 windows.h）可真编译
这些分支** —— 一轮就抓出 **9 处真缺陷**（详见 CHANGELOG v0.54.16）。本判官把这条能力做成机械门禁。

判据（棘轮）：
  ① **必须绿** = 【含 `_WIN32` 的 `.cpp`（动态发现）】∪【`PORTED` 已移植清单】——两者交叉编译都必须
     通过；失败 = 新写的 Windows 分支编不过、或已移植文件回归 → 红。
     （`PORTED` 是给"**完全可移植、不含 `_WIN32`**"的文件用的——它们改走标准库后不再含平台宏，
     动态发现抓不到，必须显式登记，否则会成为新的门禁盲区。）
  ② **棘轮**：被判集合（必须绿 ∪ BACKLOG 冻结项）出现 BACKLOG 之外的失败 → 红。
  ③ **清理提示**：BACKLOG 里已能编过的项 → 提示**迁到 `PORTED`**（不是简单删除——迁入后它就永久受
     门禁保护）。判官自身数据也会腐烂，故有此提示。
无交叉编译器 → 打印 `SKIP:` 并返回 0（CTest `SKIP_REGULAR_EXPRESSION "SKIP:"` 标为 Skipped）。
`--full`：只盘点全仓（不判红），用于出"Windows 可编译性盘点表"。

BACKLOG 分类（v0.54.16 盘点）：
  · **环境类**：只缺第三方头（sqlite3 / curl / onnxruntime / fdbus）⇒ 真机或 vcpkg 提供后即消失，
    **不是代码缺陷**。
  · **真缺陷类**：公共/平台路径上用了 POSIX 专属 API ⇒ 需逐文件补 Windows 分支（note 见下）。
"""
import argparse
import concurrent.futures as cf
import os
import re
import shutil
import subprocess
import sys

COMPILER = "x86_64-w64-mingw32-g++"
FLAGS = [
    "-std=c++17", "-fsyntax-only", "-Wshadow=local", "-Werror=shadow",
    "-Iinclude", "-Ithird_party/mongoose",
    "-DTHIN_AGENT_BUILDING_DLL", "-DTHIN_AGENT_EMBEDDED_CAM=0", "-DTHIN_AGENT_WITH_FDBUS=0",
]

# ── 已移植清单（**必须持续绿**）──
# 这些文件已通过交叉编译验证；其中若干**完全可移植**（走标准库 std::filesystem/ifstream 等，
# 因此不含 `_WIN32` 宏，动态发现抓不到）⇒ 显式登记，防止"改回 POSIX 调用"这类回归悄悄溜过门禁。
PORTED = {
    # v0.54.16（含 _WIN32 分支，动态发现亦覆盖；此处登记以防分支被删后失去保护）
    "src/core/SandboxExecutor.cpp",
    "src/core/SyntaxChecker.cpp",
    "src/core/WorkflowManager.cpp",
    "src/core/SkillRegistry.cpp",
    "src/core/AgentServiceTools.cpp",
    "src/api/discovery.cpp",
    "src/api/agent_api.cpp",
    "src/plugin/skills/code_dev.cpp",
    "src/cli/thin_agent_cli.cpp",
    # v0.54.19（含 _WIN32，动态发现亦覆盖；登记同理）
    "src/plugin/skills/code_exec.cpp",
    "src/core/PseudoTerminal.cpp",
    # v0.54.18（含 _WIN32，动态发现亦覆盖；登记同理）
    "src/core/HookSystem.cpp",
    "src/agent/StdioTransport.cpp",
    "src/core/BackgroundProcessManager.cpp",
    # v0.54.17（本轮移植：4 个）
    "src/core/PatrolProbe.cpp",
    "src/plugin/PluginLoader.cpp",
    "src/daemon/thin_agentd_main.cpp",
    "src/agent/FilesystemCheckpoint.cpp",
}

# ── 冻结表：环境类（第三方头缺失，**非代码缺陷**）──
ENV_BACKLOG = {
    "src/agent/AgentTracer.cpp":
        "error: sqlite3.h: No such file or directory",
    "src/agent/ErrorCorrectionStore.cpp":
        "error: sqlite3.h: No such file or directory",
    "src/agent/GoalManager.cpp":
        "error: sqlite3.h: No such file or directory",
    "src/agent/HttpTransport.cpp":
        "error: curl/curl.h: No such file or directory",
    "src/agent/VectorStore.cpp":
        "error: sqlite3.h: No such file or directory",
    "src/core/CronScheduler.cpp":
        "error: sqlite3.h: No such file or directory",
    "src/core/FactStore.cpp":
        "error: sqlite3.h: No such file or directory",
    "src/core/FcRunStore.cpp":
        "error: sqlite3.h: No such file or directory",
    "src/core/IntentOnnx.cpp":
        "error: onnxruntime_cxx_api.h: No such file or directory",
    "src/core/KbSearcher.cpp":
        "error: sqlite3.h: No such file or directory",
    "src/core/SessionStore.cpp":
        "error: sqlite3.h: No such file or directory",
    "src/core/TaskEngine.cpp":
        "error: sqlite3.h: No such file or directory",
    "src/core/WebhookClient.cpp":
        "error: curl/curl.h: No such file or directory",
    "src/demo/im_gateway_main.cpp":
        "error: curl/curl.h: No such file or directory",
    "src/demo/im_gateway_v2.cpp":
        "error: curl/curl.h: No such file or directory",
    "src/demo/ws_agent_main.cpp":
        "error: curl/curl.h: No such file or directory",
    "src/fdbus/FdbusDeviceControl.cpp":
        "error: fdbus/fdbus_clib.h: No such file or directory",
    "src/gateway/DiscordAdapter.cpp":
        "error: curl/curl.h: No such file or directory",
    "src/gateway/FeishuAdapter.cpp":
        "error: curl/curl.h: No such file or directory",
    "src/gateway/TelegramAdapter.cpp":
        "error: curl/curl.h: No such file or directory",
    "src/gateway/WechatAdapter.cpp":
        "error: curl/curl.h: No such file or directory",
    "src/llm/CurlHttpClient.cpp":
        "error: curl/curl.h: No such file or directory",
    "src/local/OnnxChatModel.cpp":
        "error: onnxruntime_c_api.h: No such file or directory",
    "src/plugin/skills/kb.cpp":
        "error: sqlite3.h: No such file or directory",
}

# ── 冻结表：真缺陷类（POSIX 专属 API 落在公共/平台路径上）──
DEFECT_BACKLOG = {
}
DEFECT_NOTE = {
    "sys/wait.h": "POSIX 进程等待（WIFEXITED/WEXITSTATUS/waitpid）⇒ Windows 走 "
                  "CreateProcess+WaitForSingleObject，或 _pclose 直接取退出码（WorkflowManager 已示范）",
    "sys/select.h": "POSIX select 头 ⇒ Windows 走 winsock2 的 select/WSAPoll",
    "dlfcn.h": "POSIX dlopen/dlsym ⇒ Windows LoadLibrary/GetProcAddress",
    "sys/statvfs.h": "POSIX 磁盘空间 ⇒ Windows GetDiskFreeSpaceEx",
    "mkdir": "POSIX 两参 mkdir(p, mode) ⇒ Windows 单参 _mkdir 或 fs::create_directories",
}


def collect(root):
    out = []
    for dirpath, dirnames, files in os.walk(os.path.join(root, "src")):
        dirnames[:] = [d for d in dirnames if d != "__pycache__"]
        for fn in files:
            if fn.endswith(".cpp"):
                out.append(os.path.relpath(os.path.join(dirpath, fn), root))
    return sorted(out)


def has_win32(root, rel):
    try:
        with open(os.path.join(root, rel), encoding="utf-8", errors="ignore") as f:
            return "_WIN32" in f.read()
    except OSError:
        return False


def compile_one(root, rel):
    """返回 None（编过）或首条错误文本。"""
    try:
        r = subprocess.run([COMPILER] + FLAGS + [rel], cwd=root,
                           capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        return "(编译超时 180s)"
    if r.returncode == 0:
        return None
    for line in r.stderr.splitlines():
        m = re.search(r"(fatal error|error): (.*)", line)
        if m:
            return "error: " + m.group(2)[:110]
    return "(非零退出但未见 error: 行)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--full", action="store_true", help="只盘点全仓，不判红")
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 4))
    args = ap.parse_args()
    root = os.path.abspath(args.root)

    if not shutil.which(COMPILER):
        print("SKIP: 无 %s（MinGW-w64 交叉编译器未安装）——Windows 段真编译门禁未执行" % COMPILER)
        return 0

    allcpp = collect(root)
    must = sorted(set(p for p in allcpp if has_win32(root, p)) | set(PORTED))
    frozen = set(ENV_BACKLOG) | set(DEFECT_BACKLOG)
    judged = allcpp if args.full else sorted(set(must) | frozen)
    print("== Windows 段真编译门禁：判 %d 个 TU（必须绿 %d / 冻结 %d / 全仓 %d）"
          % (len(judged), len(must), len(frozen), len(allcpp)))
    if not args.full:
        print("   必须绿名单（含 _WIN32 动态发现 ∪ PORTED %d 个）共 %d: %s"
          % (len(PORTED), len(must), ", ".join(must)))

    results = {}
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(compile_one, root, p): p for p in judged}
        for fu in cf.as_completed(futs):
            results[futs[fu]] = fu.result()

    fails = {p: e for p, e in results.items() if e}
    new_fail = {p: e for p, e in fails.items() if p not in frozen}
    cleaned = sorted(set(frozen) - set(fails))

    for p, e in sorted(new_fail.items()):
        print("  [**新增失败**] %s" % p)
        print("        %s%s" % (e, "   ← 含 _WIN32，属必须绿名单" if p in must else ""))
        for k, note in DEFECT_NOTE.items():
            if k in e:
                print("        修法提示: " + note)
    for p in cleaned:
        print("  [BACKLOG 可迁入 PORTED] %s —— 现已能编过，请迁到 PORTED（迁入后受持续门禁保护）" % p)

    if args.full:
        print("== 盘点：失败 %d / 全仓 %d ==" % (len(fails), len(allcpp)))
        for p, e in sorted(fails.items()):
            print("   %-52s %s" % (p, e))
        return 0

    must_fail = [p for p in must if p in fails]
    if new_fail:
        print("FAIL: Windows 段真编译门禁未通过（新增失败 %d 个，其中必须绿 %d 个）——"
              "含 _WIN32 的文件必须能在真 windows.h 下编过" % (len(new_fail), len(must_fail)))
        return 1
    print("PASS: 必须绿 %d/%d 全过；冻结 %d 项（环境类 %d / 真缺陷类 %d）无新增失败"
          % (len(must), len(must), len(frozen), len(ENV_BACKLOG), len(DEFECT_BACKLOG)))
    if cleaned:
        print("注意：有 %d 项 BACKLOG 已可清理（见上）" % len(cleaned))
    return 0


if __name__ == "__main__":
    sys.exit(main())
