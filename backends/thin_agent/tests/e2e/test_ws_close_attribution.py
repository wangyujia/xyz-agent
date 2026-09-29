#!/usr/bin/env python3
"""v0.54.7 (R93): WS **关闭归因契约** e2e —— "谁关的、关时在跑什么"必须可从服务端日志自证。

背景（A1 未定位项的症结）：R86/R88 两次 `e2e_ws_api_surface_sweep` 偶发红都报
`closedByServer=true`（客户端把"close 事件触发"当成了"服务端发起"），而服务端**无任何日志**能
证实或否证。根因在 mongoose 的语义：**收到对端 CLOSE 帧时它会 echo 回去并置 `is_draining=1`**
（mongoose.c 的 WEBSOCKET_OP_CLOSE 分支）⇒ 随后的 MG_EV_CLOSE 报的也是
`is_closing=1 by_us(reaper)=0`，与"服务端自己关"的签名**完全相同**。

本测试断言 v0.54.7 补上的两半归因（对端关闭码/原因 + 关闭瞬间在飞的请求类型）：
  1) 客户端主动 `close(1000, "bye")` ⇒ 服务端必须打出
     `[ws-ctl] peer_close ... code=1000 reason=bye last_req=ping`（**对端先关，非服务端**）
  2) 随后的 `[ws-close]` 行必须带 `last_req=ping` 与 `is_draining=1`
     （证明 "is_closing=1" 单独出现时**不能**推断成服务端主动关闭）

v0.54.15 (R93 A1 归因定案)：补上**缺失的两格**——本契约此前只钉了「对端优雅关闭」，
而 A1 谜团（R86/R88 `closedByServer=true`）对应的签名恰恰是**其余两种**。受控实验实测的
「关闭来源 → 服务端日志签名」对照表（本测试即固定此表）：

  | 关闭来源 | peer_close | is_draining | ws-error |
  |---|---|---|---|
  | A 对端优雅关闭 (CLOSE 帧 1000) | 有 | 1 | 无 |
  | B **对端无 CLOSE 帧断开 (FIN)** | 无 | **0** | 无 |
  | C 对端 RST 强断 | 无 | 0 | `socket error` |

  R86/R88 观测到的是 `is_closing=1 is_draining=0 by_us(reaper)=0` 且 ws-error 0 行 ⇒ **唯一匹配 B**
  ⇒ 那次是**对端异常断开**（Node `ws` 侧的 1006），**服务端没有任何主动关闭动作**
  （无 reaper 行、无 401 路径、无 ws-error）。「服务端主动关闭 connA」的旧结论不成立。

无 build/thin_agent 或端口被占用 → 打印 SKIP:（ctest SKIP_REGULAR_EXPRESSION 跳过）。
"""
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

import websocket

WS_PORT = 18795
ISO_HOME = "/tmp/ws_close_attr_home"
SVC_STDOUT = "/tmp/ws_close_attr_svc.out"      # 服务 stdout（几乎为空）
# v0.54.7: **服务把日志写文件，不写 stdout**！归因行在 $THIN_AGENT_HOME/logs/agent_svc.log。
# （踩坑实录：最初 grep 子进程 stdout，永远 0 行 —— 于是"0 命中"是测量假象。）
SVC_LOG = os.path.join(ISO_HOME, "logs", "agent_svc.log")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def wait_port(port, timeout=40.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        with socket.socket() as s:
            s.settimeout(0.5)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.3)
    return False


def wait_log(pattern, timeout=6.0):
    """轮询服务日志直到出现 pattern（返回命中的最后一行，未出现返回 None）。"""
    t0 = time.time()
    hit = None
    while time.time() - t0 < timeout:
        try:
            with open(SVC_LOG, encoding="utf-8", errors="replace") as f:
                for line in f:
                    if pattern in line:
                        hit = line.rstrip()
        except OSError:
            pass
        if hit:
            return hit
        time.sleep(0.2)
    return None



def _drain_until_pong(ws, timeout=10.0):
    """读到 pong 为止（确保服务端已把该连接的 last_req 记为 ping）。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            j = json.loads(ws.recv())
        except Exception:
            return False
        if j.get("type") == "pong":
            return True
    return False


def _wait_segment(pattern, offset, timeout=6.0):
    """轮询日志：返回"从 offset 起的全部新增行"文本；新增区间内未出现 pattern 则 None。

    只看**新增区间**是关键——服务端日志里历史行（例如上一个用例的 peer_close）会污染
    `pattern in 全区日志` 这类判断（本测试第一版就踩过"读到别人的行"）。
    """
    t0 = time.time()
    while time.time() - t0 < timeout:
        seg = []
        try:
            with open(SVC_LOG, "rb") as f:
                f.seek(offset)
                raw = f.read()
            # v0.54.23: **只认完整行**。服务端日志是无缓冲 std::cerr 逐段 << 写出的，一行 = 多次
            # write()，并发读会抓到**半行**（实测：`… last_req=` + 换行 + `ping`，同一次输出里还出现
            # `: g` 这种撕裂残片）⇒ 实测假红。规则：末尾没有 '\n' 的那一段丢弃，等它写完整再判。
            text = raw.decode("utf-8", "replace")
            lines = text.split("\n")
            if not text.endswith("\n"):
                lines = lines[:-1]          # 丢弃可能未写完的末行
            seg = [l.rstrip() for l in lines if l.strip()]
        except OSError:
            pass
        if any(pattern in l for l in seg):
            return "\n".join(seg)
        time.sleep(0.2)
    return None


def main():
    if not os.path.exists(os.path.join(REPO, "build", "thin_agent")):
        print("SKIP: build/thin_agent 不存在")
        return 0
    if socket.socket().connect_ex(("127.0.0.1", WS_PORT)) == 0:
        print("SKIP: ws 端口 %d 已被占用（残留实例？先清场）" % WS_PORT)
        return 0

    shutil.rmtree(ISO_HOME, ignore_errors=True)
    os.makedirs(os.path.join(ISO_HOME, "logs"), exist_ok=True)
    # 不依赖 LLM：本用例只发 ping（毫秒级、事件循环内处理）
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
        ws = websocket.create_connection("ws://127.0.0.1:%d/ws" % WS_PORT, timeout=15)
        ws.recv()  # hello
        ws.send(json.dumps({"type": "ping", "chat_id": "attr"}))
        t0 = time.time()
        pong = False
        while time.time() - t0 < 10:
            try:
                j = json.loads(ws.recv())
            except Exception:
                break
            if j.get("type") == "pong":
                pong = True
                break
        if not pong:
            failures.append("ping 未拿到 pong（服务未就绪？）")

        # 客户端主动关闭（带码与原因）——服务端必须能区分"对端先关"
        ws.close(status=1000, reason=b"bye")

        peer = wait_log("[ws-ctl] peer_close")
        if peer is None:
            failures.append("未见 [ws-ctl] peer_close（对端关闭码/原因未落盘 = 归因缺失）")
        else:
            print("  peer_close 行: " + peer)
            if "code=1000" not in peer:
                failures.append("peer_close 未记到 code=1000：%s" % peer)
            if "reason=bye" not in peer:
                failures.append("peer_close 未记到 reason=bye：%s" % peer)
            if "last_req=ping" not in peer:
                failures.append("peer_close 未记到在飞请求 last_req=ping：%s" % peer)

        close_line = wait_log("[ws-close]")
        if close_line is None:
            failures.append("未见 [ws-close] 行")
        else:
            print("  ws-close 行: " + close_line)
            if "last_req=ping" not in close_line:
                failures.append("ws-close 未记在飞请求：%s" % close_line)
            if "is_draining=1" not in close_line:
                failures.append("ws-close 未记 is_draining（对端关闭的表征）：%s" % close_line)

        # ── v0.54.15 用例 B：对端**不发 CLOSE 帧**直接断开（FIN）—— A1 谜团的签名来源 ──
        # 期望：无 peer_close、is_draining=0、无 ws-error。这正是"服务端主动关闭"与
        # "对端异常断开"在 is_closing 上无法区分的原因（判据必须看 is_draining + peer_close）。
        off_b = os.path.getsize(SVC_LOG) if os.path.exists(SVC_LOG) else 0
        wb = websocket.create_connection("ws://127.0.0.1:%d/ws" % WS_PORT, timeout=15)
        wb.recv()
        wb.send(json.dumps({"type": "ping", "chat_id": "attr-b"}))
        _drain_until_pong(wb)
        wb.sock.close()          # 绕过 CLOSE 帧：直接关 TCP（= Node ws 的 1006 场景）
        seg_b = _wait_segment("[ws-close]", off_b)
        if seg_b is None:
            failures.append("用例B：未见新的 [ws-close] 行（对端异常断开未落盘）")
        else:
            print("  用例B（无 CLOSE 帧断开）ws-close: " + seg_b[-110:])
            if "[ws-ctl] peer_close" in seg_b:
                failures.append("用例B：无 CLOSE 帧断开却出现 peer_close 行（归因语义错）")
            if "[ws-error]" in seg_b:
                failures.append("用例B：FIN 断开不该产生 ws-error（那是 RST 的特征）")
            if "is_draining=0" not in seg_b:
                failures.append("用例B：未记 is_draining=0 —— 该字段正是分辨"
                                "「对端先关」与「服务端主动关」的唯一可靠依据：%s" % seg_b)
            if "by_us(reaper)=0" not in seg_b:
                failures.append("用例B：误报 by_us(reaper)=1（reaper 不该参与）：%s" % seg_b)
            if "last_req=ping" not in seg_b:
                failures.append("用例B：未记在飞请求 last_req=ping：%s" % seg_b)

        # ── v0.54.15 用例 C：对端 RST 强断（SO_LINGER=0）—— 必须与 B 区分开 ──
        off_c = os.path.getsize(SVC_LOG) if os.path.exists(SVC_LOG) else 0
        wc = websocket.create_connection("ws://127.0.0.1:%d/ws" % WS_PORT, timeout=15)
        wc.recv()
        wc.send(json.dumps({"type": "ping", "chat_id": "attr-c"}))
        _drain_until_pong(wc)
        wc.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        wc.sock.close()
        seg_c = _wait_segment("[ws-error]", off_c)
        if seg_c is None:
            failures.append("用例C：RST 强断未见 [ws-error] 行（socket 错误无痕 = 运维盲区）")
        else:
            print("  用例C（RST 强断）: " + seg_c[-110:])
            if "socket error" not in seg_c:
                failures.append("用例C：ws-error 未记 socket error：%s" % seg_c)
    finally:
        try:
            svc.terminate()
            svc.wait(timeout=5)
        except Exception:
            try:
                svc.kill()
            except Exception:
                pass

    if failures:
        print("FAIL: 关闭归因契约未满足：")
        for f in failures:
            print("  - " + f)
        return 1
    print("PASS: 关闭归因契约满足（对端关闭码/原因 + 在飞请求均可从服务端日志自证）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
