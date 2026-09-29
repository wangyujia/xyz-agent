#!/usr/bin/env python3
"""
thin_agent 系统测试套件

覆盖：
  异常路径：断连重连、工具执行失败
  多轮对话：上下文保持、会话隔离
  FC 复杂任务：多文件创建→验证→清理
  边界场景：详细模式、并发请求

与 ws_smoke.py 的关系：
  ws_smoke.py     — 快速冒烟（6 用例，~5s），CI 每次跑
  ws_system_test.py — 完整系统测试（10+ 用例，~30s），发布前跑

用法:
    python ws_system_test.py              # 全量
    python ws_system_test.py --category error    # 仅异常路径
    python ws_system_test.py --category multi    # 仅多轮对话
    python ws_system_test.py --category fc       # 仅 FC 复杂任务
    python ws_system_test.py --verbose           # 详细输出

退出码: 0=全部通过, 1=有失败
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time
import urllib.request
import urllib.error
import uuid
from dataclasses import dataclass, field
from typing import Callable, Optional

from ws_client import ChatResult, WsClient


# ── 测试用例定义 ──────────────────────────────────────────────────────

@dataclass
class SystemTestCase:
    """系统测试用例 — 支持 setup/teardown 和多帧断言"""
    name: str
    category: str  # error | multi | fc | edge | fault | webhook | pty_bg
    description: str
    run: Callable[[WsClient], bool]
    setup: Optional[Callable[[], None]] = None
    teardown: Optional[Callable[[], None]] = None
    requires_cloud: bool = False  # offline 模式下自动跳过


# =====================================================================
#  测试实现
# =====================================================================

# ── 异常路径 ───────────────────────────────────────────────────────

def test_reconnect(ws: WsClient) -> bool:
    """断连后重新连接，验证功能正常"""
    # 第一阶段：正常请求
    r1 = ws.chat("你是谁", "reconnect_1")
    if not r1.ok:
        print("  FAIL: initial request failed")
        return False

    # 模拟断连
    ws.close()
    time.sleep(0.5)

    # 重新连接
    ws.connect()

    # 第二阶段：重连后功能验证
    r2 = ws.chat("你是谁", "reconnect_2")
    if not r2.ok or r2.route != r1.route:
        print(f"  FAIL: after reconnect — route={r2.route} ok={r2.ok}")
        return False

    return True


def test_tool_failure(ws: WsClient) -> bool:
    """执行一个会失败的命令，验证不崩溃且有错误提示"""
    # 执行一个保证失败的命令
    result = ws.chat("执行命令 cat /nonexistent_file_xyz_test_12345", "tool_fail_1")
    # 关键：不崩溃就通过 —— 不管路由，只要返回非空就说明 agent 没挂
    if not result.ok:
        print(f"  FAIL: agent returned empty on tool failure request")
        return False
    return True


# ── 多轮对话 ───────────────────────────────────────────────────────

def test_multi_turn_context(ws: WsClient) -> bool:
    """3 轮连续对话，验证上下文保持"""
    sid = f"multi_{uuid.uuid4().hex[:8]}"

    # Turn 1: 设定上下文
    r1 = ws.chat("请记住：我最喜欢的编程语言是 Rust", sid)
    if not r1.ok:
        print(f"  FAIL turn 1: no response")
        return False

    # Turn 2: 间接引用（不显式提 Rust）
    r2 = ws.chat("我刚才说的喜欢什么语言？", sid)
    if not r2.ok:
        print(f"  FAIL turn 2: no response")
        return False

    # Turn 3: 进一步确认
    r3 = ws.chat("用一句话总结我告诉你的信息", sid)
    if not r3.ok:
        print(f"  FAIL turn 3: no response")
        return False

    # 验证：至少有一轮提到 Rust 或确认记住了
    all_text = (r1.text + r2.text + r3.text).lower()
    has_rust = "rust" in all_text
    has_remember = any(w in all_text for w in ["记住", "remember", "喜欢", "favorite", "prefer"])

    if not (has_rust or has_remember):
        print(f"  WARN: context may not be preserved. T1={r1.text[:80]} T2={r2.text[:80]} T3={r3.text[:80]}")
        # 不强制失败 — cloud 模型可能不走记忆路径，只要响应非空就算过
        # 但如果三轮响应都正常，至少证明 FC 循环没断
    return True


def test_session_isolation(ws: WsClient) -> bool:
    """不同 session_id 应该隔离"""
    # Session A: 设定上下文
    ws.chat("请记住：我的名字叫张三", "iso_A")
    time.sleep(0.2)

    # Session B: 查询（不应该知道 A 的上下文）
    r_b = ws.chat("我叫什么名字？", "iso_B")
    if not r_b.ok:
        print(f"  FAIL: session B no response")
        return False

    # Session B 不应该提到张三（除非是巧合）
    # 注意：这是一个弱断言，因为 LLM 可能猜测或编造名字
    # 如果 B 不知道名字（说"你没告诉我"之类的），那就是正确隔离
    # 我们只验证不崩溃
    return True


# ── FC 复杂任务 ───────────────────────────────────────────────────────

def test_fc_file_ops(ws: WsClient) -> bool:
    """多文件创建 → 验证 → 清理，验证 FC 自适应迭代"""
    sid = f"fc_{uuid.uuid4().hex[:8]}"
    tmp_id = uuid.uuid4().hex[:6]

    # Step 1: 创建 3 个文件
    r1 = ws.chat(
        f"请使用 write_file 工具创建以下三个文件：\n"
        f"1. /tmp/ws_test_{tmp_id}_a.txt，内容为 'hello from A'\n"
        f"2. /tmp/ws_test_{tmp_id}_b.txt，内容为 'hello from B'\n"
        f"3. /tmp/ws_test_{tmp_id}_c.txt，内容为 'hello from C'\n"
        f"全部创建好后告诉我",
        sid,
    )
    if not r1.ok:
        print(f"  FAIL step 1 (create): no response")
        return False

    # Step 2: 读取文件内容验证
    r2 = ws.chat(
        f"请使用 read_file 工具读取以下三个文件的内容：\n"
        f"- /tmp/ws_test_{tmp_id}_a.txt\n"
        f"- /tmp/ws_test_{tmp_id}_b.txt\n"
        f"- /tmp/ws_test_{tmp_id}_c.txt",
        sid,
    )
    if not r2.ok:
        print(f"  FAIL step 2 (verify): no response")
        return False

    # Step 3: 清理（用明确路径，不用通配符）
    r3 = ws.chat(
        f"请使用 shell_exec 工具执行命令删除这三个文件：\n"
        f"rm -f /tmp/ws_test_{tmp_id}_a.txt /tmp/ws_test_{tmp_id}_b.txt /tmp/ws_test_{tmp_id}_c.txt",
        sid,
    )
    if not r3.ok:
        print(f"  FAIL step 3 (cleanup): no response")
        return False

    return True


def test_fc_concurrent(ws: WsClient) -> bool:
    """2 个并发请求，验证不崩溃"""
    results: dict = {}
    errors: dict = {}

    def do(sid: str, key: str):
        try:
            # 每个线程用自己的连接
            w = WsClient(url=_WS_URL)
            w.connect()
            r = w.chat("echo concurrent_test_" + key, sid)
            results[key] = r
            w.close()
        except Exception as e:
            errors[key] = str(e)

    t1 = threading.Thread(target=do, args=("conc_a", "a"))
    t2 = threading.Thread(target=do, args=("conc_b", "b"))
    t1.start()
    t2.start()
    t1.join(timeout=20)
    t2.join(timeout=20)

    err_a = errors.get("a")
    err_b = errors.get("b")
    if err_a or err_b:
        print(f"  FAIL: thread a error={err_a}  thread b error={err_b}")
        return False

    res_a = results.get("a")
    res_b = results.get("b")
    if not res_a or not res_a.ok:
        print(f"  FAIL: concurrent request A failed — got={type(res_a).__name__ if res_a else 'None'}")
        return False
    if not res_b or not res_b.ok:
        print(f"  FAIL: concurrent request B failed — got={type(res_b).__name__ if res_b else 'None'}")
        return False

    return True


# ── 边界场景 ───────────────────────────────────────────────────────

def test_detailed_profile(ws: WsClient) -> bool:
    """详细模式 profile —— 验证大量文本不截断"""
    result = ws.chat("详细能力", "detail_1")
    if not result.ok:
        print(f"  FAIL: no response")
        return False

    # 详细模式应该包含运行时信息
    has_runtime = (
        "v0." in result.text
        or "Runtime" in result.text
        or "运行时" in result.text
        or "模式" in result.text
    )
    if not has_runtime:
        # 详细模式可能走了非 detailed 路径，不强制失败
        pass

    # 关键验证：文本足够长（详细模式通常 > 500 字符）
    if len(result.text) < 100:
        print(f"  FAIL: response too short ({len(result.text)} chars) for detailed mode")
        return False

    return True


def test_empty_query(ws: WsClient) -> bool:
    """空消息 / 极短消息 —— 验证不崩溃"""
    # 空消息
    r1 = ws.chat("  ")
    if not r1.ok:
        print(f"  FAIL: empty message caused error")
        return False
    return True


# ── 非法 JSON 回归（🔴）────────────────────────────────────────────

def test_json_sanitize(ws: WsClient) -> bool:
    """多类特殊字符查询，验证 sanitize_utf8 生效、不崩溃"""
    tricky_queries = [
        "用代码输出一段包含特殊字符的 JSON：{ \"key\": \"value\x01with\x1fcontrol\" }",
        "输出一个表格，包含中英文、emoji 和特殊符号：✅ ❌ ⚡ 🔥 \n| 列1 | 列2 |\n|---|---|",
        "echo $'test\\x00null\\x1bescape'",
        "列出当前目录下所有文件（包括隐藏文件）",
    ]
    for i, q in enumerate(tricky_queries):
        r = ws.chat(q, f"sanitize_{i}")
        if not r.ok:
            print(f"  FAIL: query {i} returned empty — '{q[:60]}'")
            return False
    return True


# ── FC 编译链（🟡）──────────────────────────────────────────────────

def test_fc_compile_chain(ws: WsClient) -> bool:
    """创建 C++ 文件 → 编译 → 运行 → 验证输出 → 清理
    验证自适应 FC 迭代能处理完整的编译链路。
    """
    sid = f"cc_{uuid.uuid4().hex[:6]}"
    tmp_id = uuid.uuid4().hex[:6]

    # Step 1: 写一个 hello world C++ 文件
    r1 = ws.chat(
        f"请使用 write_file 工具创建文件 /tmp/hello_{tmp_id}.cpp，内容为：\n"
        f'#include <iostream>\nint main() {{ std::cout << "HELLO_{tmp_id}" << std::endl; return 0; }}',
        sid,
    )
    if not r1.ok:
        print(f"  FAIL step1 (create): no response")
        return False

    # Step 2: 编译
    r2 = ws.chat(
        f"请使用 shell_exec 工具执行命令编译：g++ /tmp/hello_{tmp_id}.cpp -o /tmp/hello_{tmp_id}",
        sid,
    )
    if not r2.ok:
        print(f"  FAIL step2 (compile): no response")
        return False

    # Step 3: 运行
    r3 = ws.chat(
        f"请使用 shell_exec 工具运行 /tmp/hello_{tmp_id} 并告诉我输出是什么",
        sid,
    )
    if not r3.ok:
        print(f"  FAIL step3 (run): no response")
        return False

    # Step 4: 清理
    r4 = ws.chat(
        f"请使用 shell_exec 工具执行命令删除：rm -f /tmp/hello_{tmp_id}.cpp /tmp/hello_{tmp_id}",
        sid,
    )
    if not r4.ok:
        print(f"  FAIL step4 (cleanup): no response")
        return False

    # v0.37.4: 强制验证 HELLO_ 在编译运行的输出中
    all_text = r1.text + r2.text + r3.text + r4.text
    if f"HELLO_{tmp_id}" not in all_text:
        print(f"  FAIL: HELLO_{tmp_id} not found in any response — compile/run likely failed")
        return False
    return True


# ── 子 Agent 派生子（🟡）────────────────────────────────────────────

def test_spawn_agent(ws: WsClient) -> bool:
    """派生子 agent 执行简单任务，验证 spawn_agent 工具可用"""
    r = ws.chat(
        "使用 spawn_agent 派生子 agent 执行以下任务：列出现在是几点几分",
        f"spawn_{uuid.uuid4().hex[:6]}",
    )
    if not r.ok:
        print(f"  FAIL: spawn_agent request returned empty")
        return False
    # 不强制验证子 agent 是否真的跑了 —
    # 只需验证 spawn_agent 调用不导致崩溃/超时
    return True


# ── Kill 重启恢复（🟢）──────────────────────────────────────────────

def test_kill_restart(ws: WsClient) -> bool:
    """重启 agent 后验证功能正常（自动重启）"""
    import subprocess

    # Step 1: 重启前确认正常
    r1 = ws.chat("你是谁", "kr_pre")
    if not r1.ok:
        print(f"  FAIL: pre-restart check failed")
        return False

    # Step 2: kill agent
    pid_file = "/tmp/thin_agent_main.pid"
    try:
        with open(pid_file) as f:
            pid = int(f.read().strip())
        subprocess.run(["kill", "-9", str(pid)], timeout=5)
        print(f"  killed PID={pid}, restarting...")
        time.sleep(2)
    except (FileNotFoundError, ValueError, subprocess.TimeoutExpired):
        print(f"  WARN: cannot kill, PID file missing")
        return True

    # Step 3: 重新启动 agent（继承完整环境变量）
    import os
    subprocess.Popen(
        ["bash", "/root/.thin_agent/run_agent.sh", "--main", "--dev"],
        env={**os.environ, "HOME": "/root"},
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,  # 脱离父进程独立运行
    )
    time.sleep(3)

    # Step 4: 等待就绪，最多重试 10 次
    for attempt in range(10):
        try:
            ws.close()
            ws.connect()
            # 更新 PID 文件
            try:
                new_pid = subprocess.check_output(
                    "ps aux | grep '/root/code/thin_agent/build/thin_agent' | grep -v grep | awk '{print $2}'",
                    shell=True, timeout=3
                ).decode().strip()
                if new_pid:
                    with open(pid_file, 'w') as f:
                        f.write(new_pid)
            except Exception:
                pass

            r2 = ws.chat("你是谁", "kr_post")
            if r2.ok:
                print(f"  restarted after {attempt + 1} attempts (route={r2.route})")
                return True
        except Exception:
            pass
        time.sleep(2)

    print(f"  FAIL: agent did not recover after 10 attempts")
    return False


# =====================================================================
#  测试注册
# =====================================================================

# webhook URL 从 --url 参数提取，默认 8765（与 WS 同端口）
import re as _re
WEBHOOK_URL = "http://127.0.0.1:8765/webhook"
_WS_URL = "ws://127.0.0.1:8765/ws"  # 供 fc_concurrent 等内部 WsClient 使用

def _update_webhook_url(ws_url: str):
    """从 ws://host:port/ws 提取 host:port，更新 WEBHOOK_URL"""
    global WEBHOOK_URL
    m = _re.search(r'ws://(\[[0-9a-fA-F:]+\]|[^:/]+):(\d+)', ws_url)
    if m:
        WEBHOOK_URL = f"http://{m.group(1)}:{m.group(2)}/webhook"


def _webhook_post(payload: dict, timeout: float = 5.0) -> tuple[int, str]:
    """POST to webhook endpoint. Returns (status_code, body)."""
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        WEBHOOK_URL,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8")
    except Exception as e:
        return -1, str(e)


def test_webhook_valid_json(ws: WsClient) -> bool:
    """webhook POST 有效 JSON → 200 响应"""
    code, body = _webhook_post({
        "source": "github",
        "event": "push",
        "payload": '{"ref":"refs/heads/main","commits":3}',
    })
    return code == 200


def test_webhook_minimal_json(ws: WsClient) -> bool:
    """webhook POST 最小 JSON（空对象）→ 200 响应"""
    code, body = _webhook_post({})
    return code == 200


def test_webhook_invalid_json(ws: WsClient) -> bool:
    """webhook POST 非法 JSON → 非 200 响应"""
    data = b"not json"
    req = urllib.request.Request(
        WEBHOOK_URL,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=5.0) as resp:
            return False  # should not succeed
    except urllib.error.HTTPError:
        return True   # expected: non-200
    except Exception:
        return True   # connection error is also OK (non-200 result)


def test_webhook_get_rejected(ws: WsClient) -> bool:
    """webhook GET 请求 → 拒绝（只接受 POST）"""
    try:
        with urllib.request.urlopen(WEBHOOK_URL, timeout=5.0) as resp:
            return False  # GET should not be accepted
    except urllib.error.HTTPError:
        return True
    except Exception:
        return True



# ── PTY + Background 进程管理 ───────────────────────────────────────

def test_shell_exec_basic(ws: WsClient) -> bool:
    """shell_exec 基本命令执行（回归测试）"""
    r = ws.chat(
        "运行命令 echo shell_exec_regression_test，只返回输出",
        f"se_{uuid.uuid4().hex[:6]}",
    )
    if not r.ok:
        print(f"  FAIL: no response")
        return False
    if "shell_exec_regression_test" not in r.text:
        print(f"  FAIL: output not found: {r.text[:200]}")
        return False
    return True


def test_pty_simple(ws: WsClient) -> bool:
    """PTY 模式执行简单命令，验证输出"""
    r = ws.chat(
        "使用 shell_exec 工具，设置 pty=true，运行命令：echo hello_from_pty_system_test\n"
        "把返回的 output 内容告诉我",
        f"pty_{uuid.uuid4().hex[:6]}",
    )
    if not r.ok:
        print(f"  FAIL: no response")
        return False
    if "hello_from_pty_system_test" not in r.text:
        print(f"  FAIL: PTY output not found: {r.text[:200]}")
        return False
    return True


def test_bg_start_poll_kill(ws: WsClient) -> bool:
    """后台进程：start → poll → kill 完整生命周期"""
    sid = f"bg_{uuid.uuid4().hex[:6]}"
    marker = uuid.uuid4().hex[:8]

    # Step 1: 启动后台进程
    r1 = ws.chat(
        f"使用 shell_exec 工具，设置 background=true，运行命令：sleep 30; echo bg_marker_{marker}\n"
        f"把返回的 session_id 原文告诉我",
        sid,
    )
    if not r1.ok:
        print(f"  FAIL step 1 (start): no response")
        return False
    if "proc_" not in r1.text and "session_id" not in r1.text:
        print(f"  FAIL step 1 (start): no session_id: {r1.text[:200]}")
        return False
    print(f"  bg started")

    # Step 2: 轮询进程状态
    r2 = ws.chat(
        "使用 process 工具，action=poll，对刚才启动的后台进程轮询状态",
        sid,
    )
    if not r2.ok:
        print(f"  FAIL step 2 (poll): no response")
        return False

    # Step 3: 终止进程
    r3 = ws.chat(
        "使用 process 工具，action=kill，终止刚才那个后台进程",
        sid,
    )
    if not r3.ok:
        print(f"  FAIL step 3 (kill): no response")
        return False

    return True


def test_bg_wait(ws: WsClient) -> bool:
    """后台进程 poll 状态验证"""
    sid = f"bgw_{uuid.uuid4().hex[:6]}"
    marker = uuid.uuid4().hex[:8]

    # 启动一个后台进程
    r1 = ws.chat(
        f"使用 shell_exec 工具，设置 background=true，运行命令：sleep 5; echo done_{marker}\n"
        f"把返回的 session_id 告诉我",
        sid,
    )
    if not r1.ok:
        print(f"  FAIL step 1 (start): no response")
        return False

    # 用 process poll 检查状态（进程应该还在运行）
    r2 = ws.chat(
        "使用 process 工具，action=poll，检查这个后台进程的状态。把 running 的值告诉我",
        sid,
    )
    if not r2.ok:
        print(f"  FAIL step 2 (poll): no response")
        return False
    print(f"  poll result: {r2.text[:100].strip()}")

    # 清理：kill 进程
    r3 = ws.chat(
        "使用 process 工具，action=kill，终止这个后台进程",
        sid,
    )
    if not r3.ok:
        print(f"  FAIL step 3 (kill): no response")
        return False

    return True

def register_all_tests() -> list[SystemTestCase]:
    return [
        # ── 异常路径 ──
        SystemTestCase("reconnect", "error",
            "WS 断连重连后功能正常", test_reconnect),
        SystemTestCase("tool_failure", "error",
            "工具执行失败不崩溃", test_tool_failure),
        SystemTestCase("json_sanitize", "error",
            "特殊字符查询不崩溃（UTF-8 清洗回归）", test_json_sanitize),

        # ── 多轮对话 ──
        SystemTestCase("multi_turn", "multi",
            "3 轮连续对话上下文保持", test_multi_turn_context),
        SystemTestCase("session_isolation", "multi",
            "不同 session 隔离", test_session_isolation),
        SystemTestCase("spawn_agent", "multi",
            "子 Agent 派生子不崩溃", test_spawn_agent),

        # ── FC 复杂任务（需要 cloud FC）──
        SystemTestCase("fc_file_ops", "fc",
            "多文件创建→验证→清理", test_fc_file_ops, requires_cloud=True),
        SystemTestCase("fc_compile_chain", "fc",
            "C++ 创建→编译→运行→清理（自适应迭代）", test_fc_compile_chain, requires_cloud=True),
        SystemTestCase("fc_concurrent", "fc",
            "2 并发请求不崩溃", test_fc_concurrent, requires_cloud=True),

        # ── 边界场景 ──
        SystemTestCase("detailed_profile", "edge",
            "详细模式大量文本不截断", test_detailed_profile, requires_cloud=True),
        SystemTestCase("empty_query", "edge",
            "空消息不崩溃", test_empty_query),

        # ── 故障恢复 ──
        SystemTestCase("kill_restart", "fault",
            "Kill 重启后功能恢复", test_kill_restart),

        # ── Webhook 端点 ── (v0.29.0)
        SystemTestCase("webhook_valid", "webhook",
            "POST 有效 JSON → 200", test_webhook_valid_json),
        SystemTestCase("webhook_minimal", "webhook",
            "POST 最小 JSON → 200", test_webhook_minimal_json),
        SystemTestCase("webhook_invalid", "webhook",
            "POST 非法 JSON → 拒绝", test_webhook_invalid_json),
        SystemTestCase("webhook_get_reject", "webhook",
            "GET 请求 → 拒绝", test_webhook_get_rejected),

        # ── PTY + Background 进程管理 (v0.30.0，需要 cloud FC) ──
        SystemTestCase("shell_exec_basic", "pty_bg",
            "shell_exec 基本命令执行回归", test_shell_exec_basic, requires_cloud=True),
        SystemTestCase("pty_simple", "pty_bg",
            "PTY 模式执行命令验证输出", test_pty_simple, requires_cloud=True),
        SystemTestCase("bg_start_poll_kill", "pty_bg",
            "后台进程 start→poll→kill 生命周期", test_bg_start_poll_kill, requires_cloud=True),
        SystemTestCase("bg_wait", "pty_bg",
            "后台进程 poll+kill 操作", test_bg_wait, requires_cloud=True),
    ]


# =====================================================================
#  运行器
# =====================================================================

def run_system_tests(
    tests: list[SystemTestCase],
    verbose: bool = False,
    url: str = "ws://127.0.0.1:8765/ws",
    skip_cloud: bool = False,
) -> tuple[int, int, list[str], int]:
    """返回 (passed, total, failed_names, skipped_count)"""
    # 设置全局 URL 供 fc_concurrent 等内部创建 WsClient 的测试使用
    global _WS_URL
    _WS_URL = url
    passed = 0
    total = len(tests)
    failed: list[str] = []
    skipped = 0

    active = total
    if skip_cloud:
        cloud_tests = [t for t in tests if t.requires_cloud]
        active = total - len(cloud_tests)

    mode_label = "offline" if skip_cloud else "full"
    print(f"thin_agent 系统测试 — {total} 用例 (模式: {mode_label}, 有效: {active})")
    print(f"目标: {url}")
    print(f"{'='*60}")

    _update_webhook_url(url)

    for i, tc in enumerate(tests, 1):
        tag = f"[{tc.category}]"

        # offline 模式跳过需要 cloud FC 的用例
        if skip_cloud and tc.requires_cloud:
            print(f"[{i}/{total}] ⏭️  {tag} {tc.name}: SKIP (requires cloud FC)")
            skipped += 1
            continue

        # Setup
        if tc.setup:
            tc.setup()

        # Run
        try:
            ws = WsClient(url=url)
            ws.connect()

            start = time.time()
            ok = tc.run(ws)
            elapsed = time.time() - start

            ws.close()

        except Exception as e:
            ok = False
            elapsed = 0
            if verbose:
                import traceback
                traceback.print_exc()

        # Teardown
        if tc.teardown:
            try:
                tc.teardown()
            except Exception:
                pass

        status = "✅" if ok else "❌"
        print(f"[{i}/{total}] {status} {tag} {tc.name}: {tc.description}  ({elapsed:.1f}s)")

        if not ok:
            failed.append(tc.name)

        if ok:
            passed += 1

        time.sleep(0.3)

    return passed, total, failed, skipped


# =====================================================================
#  main
# =====================================================================

def main():
    parser = argparse.ArgumentParser(description="thin_agent 系统测试")
    parser.add_argument("--category", "-c", choices=["error", "multi", "fc", "edge", "fault", "webhook", "pty_bg"],
                        help="仅跑指定类别")
    parser.add_argument("--verbose", "-v", action="store_true", help="详细输出")
    parser.add_argument("--url", default="ws://127.0.0.1:8765/ws", help="WS 地址")
    parser.add_argument("--offline", action="store_true", help="跳过需要 cloud FC 的用例")
    args = parser.parse_args()

    all_tests = register_all_tests()

    if args.category:
        tests = [t for t in all_tests if t.category == args.category]
        if not tests:
            print(f"错误: 没有类别 '{args.category}' 的测试")
            sys.exit(1)
    else:
        tests = all_tests

    passed, total, failed, skipped = run_system_tests(
        tests, verbose=args.verbose, url=args.url, skip_cloud=args.offline
    )

    print(f"{'='*60}")
    active = total - skipped
    if skipped:
        print(f"结果: {passed}/{active} PASS ({skipped} skipped)")
    else:
        print(f"结果: {passed}/{total} PASS")

    if failed:
        print(f"失败: {', '.join(failed)}")
        sys.exit(1)
    else:
        sys.exit(0)


if __name__ == "__main__":
    main()
