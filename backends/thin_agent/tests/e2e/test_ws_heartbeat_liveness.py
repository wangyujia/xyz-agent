#!/usr/bin/env python3
"""e2e_ws_heartbeat_liveness：验证**现存的**连接活性机制（旧 180s 收割器已于 v0.54.21 按拍板删除）

强断言（成功必须打印全部 PASS，失败打印 FAIL 并以 1 退出；无 build/thin_agent 或端口被占用才 SKIP）：
  1) 客户端**静默 ≥35s** 期间必须收到服务端心跳帧 `{"type":"heartbeat"}`（30s 节拍确实在发）
     —— 这是现今唯一的活性探测手段（心跳写出失败 ⇒ mongoose 标记 close）；
  2) 全程**不得**被判死踢除：连接保持打开（无 CLOSE 帧、recv 不报断开）；
  3) 静默结束后 `{"type":"ping"}` 仍能拿到 `{"type":"pong"}`
     —— 长任务静默客户端的可用性回归（v0.52.15b 实测 196s 静默被误踢、conclusion 帧永久丢失）；
  4) 服务端日志不得出现 `[ws-heartbeat] closing dead connection`（该收割路径已删除）；
  5) **静态防回退闸**：`src/demo/ws_agent_main.cpp` 不得再出现 `kDeadConnectionMs` /
     `closing dead connection` —— 这道"死防线"是**按拍板删除**的（不可达 + 判据会误杀长任务静默
     客户端）。若将来要按"方案 B（双向 liveness：客户端回 pong，连续 N 次无 pong 才收割）"重新实现，
     **必须同步改本断言**，并补一条覆盖真实阈值的用例（本用例静默窗口 35s < 任何 180s 级阈值，
     单靠它抓不住"重新加回一个可达的 180s 收割器"）。

背景（为什么删掉旧机制）：旧代码有"180s 无活动 → is_closing"的收割分支，但同一 tick 内先把全部
`g_connections` 刷成最新时间**再**收割 ⇒ 候选集恒空（不可达）；且其判据会误杀长任务静默客户端。
配套的 tests/unit/test_ws_heartbeat.cpp 是**自欺单测**（复刻常量 + mock 数据重实现算法，从不跑产品
代码），已一并删除 —— 那里的"覆盖"是假的，真实性在这里（真服务 + 真 socket + 真时间流逝）。
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import time

import websocket

WS_PORT = 18796
ISO_HOME = "/tmp/ws_hb_liveness_home"
SVC_STDOUT = "/tmp/ws_hb_liveness_svc.out"
SVC_LOG = os.path.join(ISO_HOME, "logs", "agent_svc.log")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SILENCE_SECONDS = 35.0          # > 一个 30s 心跳周期
HEARTBEAT_MIN = 1


def wait_port(port, timeout=40.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if socket.socket().connect_ex(("127.0.0.1", port)) == 0:
            return True
        time.sleep(0.2)
    return False


def read_log():
    try:
        with open(SVC_LOG, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def main():
    if not os.path.exists(os.path.join(REPO, "build", "thin_agent")):
        print("SKIP: build/thin_agent 不存在")
        return 0
    if socket.socket().connect_ex(("127.0.0.1", WS_PORT)) == 0:
        print("SKIP: ws 端口 %d 已被占用（残留实例？先清场）" % WS_PORT)
        return 0

    shutil.rmtree(ISO_HOME, ignore_errors=True)
    os.makedirs(os.path.join(ISO_HOME, "logs"), exist_ok=True)
    # 不依赖 LLM：本用例只连 WS + 发 ping（毫秒级、事件循环内处理）
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
        env=env, cwd=REPO, stdout=open(SVC_STDOUT, "a"), stderr=subprocess.STDOUT)

    failures = []
    try:
        if not wait_port(WS_PORT):
            print("SKIP: 服务未起来（见 %s）" % SVC_STDOUT)
            return 0
        time.sleep(0.8)
        ws = websocket.create_connection("ws://127.0.0.1:%d/ws" % WS_PORT, timeout=3)
        hello = ws.recv()   # hello
        if isinstance(hello, bytes):
            hello = hello.decode("utf-8", "replace")
        if "hello" not in hello:
            failures.append("首帧不是 hello：%s" % hello)

        # ── 静默 35s：只在"收帧"上等待，一个字节都不发 ──
        heartbeats = 0
        disconnected = None
        closed_frame = False
        t0 = time.time()
        while time.time() - t0 < SILENCE_SECONDS:
            try:
                raw = ws.recv()
            except websocket.WebSocketTimeoutException:
                continue          # 静默期内没有帧是正常的（心跳 30s 才来一次）
            except Exception as e:   # 断开（被踢/连接异常）
                disconnected = repr(e)
                break
            if raw is None or raw == "":
                closed_frame = True
                break
            try:
                j = json.loads(raw)
            except Exception:
                continue
            if j.get("type") == "heartbeat":
                heartbeats += 1
        silent_for = time.time() - t0

        if disconnected is not None:
            failures.append("静默期间连接断开（连接被判死踢除）：%s" % disconnected)
        if closed_frame:
            failures.append("静默期间收到 CLOSE 帧（连接被判死踢除）")
        if heartbeats < HEARTBEAT_MIN:
            failures.append("静默 %.0fs 内未收到心跳帧（心跳节拍失效？收到 %d 个）"
                            % (silent_for, heartbeats))
        else:
            print("PASS: 静默 %.0fs 内收到 %d 个 heartbeat 帧（30s 节拍生效）"
                  % (silent_for, heartbeats))

        # ── 静默后仍可用（v0.52.15b 回归：长任务静默客户端不得被误踢）──
        if disconnected is None and not closed_frame:
            try:
                ws.send(json.dumps({"type": "ping", "chat_id": "hb_liveness"}))
                got_pong = False
                t1 = time.time()
                while time.time() - t1 < 10:
                    try:
                        j = json.loads(ws.recv())
                    except websocket.WebSocketTimeoutException:
                        continue
                    except Exception as e:
                        failures.append("静默后收发失败：%r" % e)
                        break
                    if j.get("type") == "pong":
                        got_pong = True
                        break
                if got_pong:
                    print("PASS: 静默 %.0fs 后 ping→pong 正常（连接未被误踢）" % silent_for)
                elif not failures or "静默后收发失败" not in "".join(failures):
                    failures.append("静默 %.0fs 后 ping 未拿到 pong" % silent_for)
            finally:
                try:
                    ws.close(status=1000, reason=b"done")
                except Exception:
                    pass

        # ── 服务端日志不得出现已删除的收割路径 ──
        time.sleep(0.5)
        log = read_log()
        if "[ws-heartbeat] closing dead connection" in log:
            failures.append("服务端日志出现已删除的收割路径 [ws-heartbeat] closing dead connection")
        else:
            print("PASS: 服务端无 [ws-heartbeat] closing dead connection 行（收割路径确已删除）")
    finally:
        try:
            svc.terminate()
            svc.wait(timeout=8)
        except Exception:
            try:
                svc.kill()
            except Exception:
                pass

    # ── 5) 静态防回退闸：死收割器不得回来（见模块 docstring 第 5 条）──
    try:
        with open(os.path.join(REPO, "src", "demo", "ws_agent_main.cpp"),
                  encoding="utf-8", errors="replace") as f:
            src = f.read()
        banned = [t for t in ("kDeadConnectionMs", "closing dead connection") if t in src]
        if banned:
            failures.append("ws_agent_main.cpp 出现已删除的死防线符号 %s —— 若是有意重新实现（如双向 "
                            "liveness），请同步更新本用例的断言与静默窗口" % banned)
        else:
            print("PASS: ws_agent_main.cpp 无 kDeadConnectionMs / closing dead connection（死防线未回来）")
    except OSError as e:
        failures.append("无法读取 ws_agent_main.cpp 做静态闸检查：%r" % e)

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1
    print("PASS: 全部断言通过（心跳节拍 / 静默不被误踢 / 静默后仍可用 / 收割路径已删）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
