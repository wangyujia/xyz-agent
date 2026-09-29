#!/usr/bin/env python3
"""e2e_ws_idle_attribution：`[ws-close]` 的 `idle_ms` 必须**如实反映客户端静默时长**

背景（v0.54.30 审查发现）：`idle_ms` 是关闭归因（v0.54.15 签名表）依赖的诊断字段 —— 用来判断
"这次关闭前连接是**静默已久**还是**正在活跃**"。但旧实现里 `g_conn_last_active` 会被**每 30s 的
心跳节拍**刷新（该刷新原本是为已删除的 180s reaper 压枪而加）⇒ 一个**静默 10 分钟**的客户端会被
报成 `idle_ms ≤ 30000`（**字段撒谎**，正是它会让根因分析误判"连接一直在活跃"）。

断言（强断言、确定性、不依赖心跳相位）：
  1) 客户端**静默 65s** 后优雅关闭 ⇒ `[ws-close]` 的 `idle_ms` **≥ 60000**
     （旧实现上限 ≈30000 ⇒ 必红）
  2) `last_req` 如实记录在飞请求类型（ping）
  3) 静默期间客户端**不被踢**，且收到 ≥1 次 30s 心跳（活性节拍未失约）
"""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WS_PORT = 18799
ISO_HOME = "/tmp/ws_idle_attr_home"
SVC_LOG = Path(ISO_HOME) / "logs" / "agent_svc.log"
SVC_STDOUT = Path(ISO_HOME) / "svc_stdout.log"
SILENCE_S = 65.0


def wait_port(port: int, timeout: float = 20.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if socket.socket().connect_ex(("127.0.0.1", port)) == 0:
            return True
        time.sleep(0.3)
    return False


def log_text() -> str:
    try:
        return SVC_LOG.read_text(errors="replace")
    except Exception:
        return ""


def wait_line(pattern: str, timeout: float) -> str:
    rx = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in log_text().splitlines():
            if rx.search(line):
                return line
        time.sleep(0.2)
    return ""


def main() -> int:
    if not os.path.exists(os.path.join(REPO, "build", "thin_agent")):
        print("SKIP: build/thin_agent 不存在")
        return 0
    if socket.socket().connect_ex(("127.0.0.1", WS_PORT)) == 0:
        print("SKIP: ws 端口 %d 被占用（残留实例？）" % WS_PORT)
        return 0

    try:
        import websocket  # websocket-client
    except ImportError:
        print("SKIP: 缺 websocket-client")
        return 0

    shutil.rmtree(ISO_HOME, ignore_errors=True)
    os.makedirs(os.path.join(ISO_HOME, "logs"), exist_ok=True)
    cfg = os.path.join(ISO_HOME, "profile.yaml")
    with open(cfg, "w", encoding="utf-8") as f:
        f.write("profiles:\n"
                "  lmstudio_demo:\n"
                "    mode: cloud\n"
                "    provider: lmstudio\n"
                "    name: m\n"
                '    api_base: "http://127.0.0.1:1/v1"\n'
                "    request_timeout_ms: 2000\n"
                "    fallback: offline\n")
    env = dict(os.environ)
    env["THIN_AGENT_HOME"] = ISO_HOME
    env["THIN_AGENT_DEV_MODE"] = "1"
    open(SVC_STDOUT, "w").close()
    svc = subprocess.Popen(
        ["./build/thin_agent", "--port", str(WS_PORT), "--config", cfg,
         "--profile", "lmstudio_demo", "--dev"],
        env=env, cwd=str(REPO), stdout=open(SVC_STDOUT, "a"), stderr=subprocess.STDOUT)

    failures = []
    ws = None
    try:
        if not wait_port(WS_PORT):
            print("SKIP: 服务未起来（见 %s）" % SVC_STDOUT)
            return 0
        time.sleep(0.8)
        ws = websocket.create_connection("ws://127.0.0.1:%d/ws" % WS_PORT, timeout=15)
        ws.recv()   # hello
        ws.send(json.dumps({"type": "ping", "chat_id": "idle"}))
        try:
            ws.recv()   # pong
        except Exception:
            pass

        # ── 静默期：只读不回（心跳每 30s 到达）──
        t0 = time.time()
        beats = 0
        while time.time() - t0 < SILENCE_S:
            ws.settimeout(5)
            try:
                data = ws.recv()
                if isinstance(data, bytes):
                    data = data.decode("utf-8", "replace")
                if "heartbeat" in data:
                    beats += 1
            except websocket.WebSocketTimeoutException:
                continue
            except Exception as e:
                failures.append(f"静默期间连接异常中断：{e!r}（应保持存活）")
                break

        try:
            ws.close()   # 优雅关闭（发 CLOSE 帧）
        except Exception:
            pass
        ws = None

        # 先按"对端优雅关闭"行拿到**本连接**的 id（日志里可能有无关短连接的 [ws-close]）
        ctl = wait_line(r"\[ws-ctl\] peer_close.*\blast_req=", 15)
        my_id = None
        if ctl:
            mm = re.search(r"id=(\d+)", ctl)
            my_id = mm.group(1) if mm else None
        pattern = (r"\[ws-close\] id=%s .*\bidle_ms=" % my_id) if my_id else r"\[ws-close\].*\bidle_ms="
        line = wait_line(pattern, 15)
        if not line:
            failures.append("未找到 [ws-close] 行（关闭归因缺失）")
        else:
            m = re.search(r"idle_ms=(-?\d+)", line)
            idle = int(m.group(1)) if m else None
            mreq = re.search(r"last_req=(\S*)", line)
            last_req = mreq.group(1) if mreq else ""
            print(f"  [观察] {line.strip()}")
            print(f"  [观察] 静默 {SILENCE_S:.0f}s ⇒ idle_ms={idle} / last_req={last_req} / 心跳={beats}")
            if idle is not None and idle >= 60000:
                print("PASS: idle_ms 如实反映客户端静默时长（>=60000ms）")
            else:
                failures.append(
                    f"idle_ms 不实：{idle}（静默 {SILENCE_S:.0f}s，应 >=60000ms；"
                    f"被心跳节拍刷新过 = 诊断字段撒谎）")
            if last_req == "ping":
                print("PASS: last_req 如实记录在飞请求类型（ping）")
            else:
                failures.append(f"last_req 不实：{last_req!r}（应为 'ping'）")
        if beats >= 1:
            print(f"PASS: 静默期间收到 {beats} 次心跳且未被踢（活性节拍未失约）")
        else:
            failures.append("静默期间未收到心跳（30s 节拍失约）")
    finally:
        if ws is not None:
            try:
                ws.close()
            except Exception:
                pass
        svc.terminate()
        try:
            svc.wait(timeout=10)
        except Exception:
            svc.kill()

    if failures:
        print("FAIL: idle_ms 归因契约未满足：")
        for f in failures:
            print("  - " + f)
        return 1
    print("ALL PASS: idle_ms 归因如实")
    return 0


if __name__ == "__main__":
    sys.exit(main())
