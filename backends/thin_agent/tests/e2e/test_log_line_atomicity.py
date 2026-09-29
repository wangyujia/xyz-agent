#!/usr/bin/env python3
"""e2e_log_line_atomicity：日志行**原子性** —— 并发读者不得看到"半行"

两阶段取证（**第 2 阶段才是证明**）：
  阶段1 并发读者：读线程持续读日志并只认完整行 —— 注意它**天生抓不住撕裂**（撕裂行总是文件尾部，
  而尾部未写完的那段按设计要丢弃），故仅作冒烟守卫，**不作为原子性证明**（变异实验实测：把日志退回
  多段 `<<` 写出，该阶段 4 连绿 = 检测不到）。
  阶段2 **write() 探针（LD_PRELOAD）**：拦截 write()，对写向 agent_svc.log 的每一笔判定是否以 '\n'
  结束 —— 这才是"一行 = 一次 write()"的**机制级判据**（变异实验实测：退回多段 `<<` 必现 PART = 变红）。

背景（R94 实锤，v0.54.24 修）：本服务日志曾用无缓冲 `std::cerr` 逐段 `<<` 写出（`cerr` 默认
`unitbuf`）⇒ **一行 = 多次 write()**；并被 stdio 的 4KB 缓冲在跨边界时进一步拆开。并发读者（e2e 断言 /
`tail -f` / 日志采集器）因此能读到**半行**——实测到 `… last_req=` 后换行再接 `ping`，还造成一条**假红**。
修法：① 整行拼好、**单次** `cerr.write()`（`TA_LOG_LINE`，与 LogEvent.h 同一约定）；② 日志文件
`_IONBF`（一次 fwrite = 一次 write）。

强断言（成功打印全部 PASS；失败打印 FAIL 并以 1 退出；无 build/thin_agent 或端口被占用才 SKIP）：
  1) 读线程在服务运行期间**持续并发读**同一日志，它观测到的**每一个完整行**必须逐字存在于最终日志
     （撕裂行不可能逐字存在于最终日志 ⇒ 一定被抓住）；
  2) 观测到的行数 ≥ 20（否则本用例空转 = 等于没测）；
  3) 最终日志每个完整行都以 `[` 开头且含 `]`（格式完好，无残片）。
"""
import json
import os
import shutil
import socket
import struct
import subprocess
from pathlib import Path
import threading
import time

import websocket

WS_PORT = 18797
ISO_HOME = "/tmp/ws_log_atomic_home"
SVC_STDOUT = "/tmp/ws_log_atomic_svc.out"
SVC_LOG = os.path.join(ISO_HOME, "logs", "agent_svc.log")
PROBE_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools", "write_line_probe.c")
PROBE_SO = "/tmp/ta_write_line_probe.so"
PROBE_TRACE = "/tmp/ta_write_line_trace.txt"


def build_probe():
    """编译 write() 探针（无 gcc 则阶段 2 不可用）。"""
    if not os.path.exists(PROBE_SRC):
        return False
    r = subprocess.run(["gcc", "-shared", "-fPIC", "-O0", "-o", PROBE_SO, PROBE_SRC, "-ldl"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return r.returncode == 0 and os.path.exists(PROBE_SO)
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TRAFFIC_SECONDS = 8.0
MIN_OBSERVED_LINES = 20


def wait_port(port, timeout=40.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if socket.socket().connect_ex(("127.0.0.1", port)) == 0:
            return True
        time.sleep(0.2)
    return False


class LogReader(threading.Thread):
    """持续读日志文件，只记录**完整行**（末尾无换行的那段丢弃）。"""

    def __init__(self, path):
        super().__init__(daemon=True)
        self.path = path
        self.stop_flag = False
        self.observed = []          # 观测到的完整行（可能重复）
        self.reads = 0

    def run(self):
        while not self.stop_flag:
            try:
                with open(self.path, "rb") as f:
                    raw = f.read()
            except OSError:
                continue
            self.reads += 1
            text = raw.decode("utf-8", "replace")
            lines = text.split("\n")
            if not text.endswith("\n"):
                lines = lines[:-1]      # 丢弃可能未写完的末行
            for l in lines:
                if l.strip():
                    self.observed.append(l.rstrip())
            # v0.54.24: **限速**（约 250 次读/秒）。无 sleep 的紧循环会抢走整机 CPU（实测 1.3 万读/秒），
            # 在 `ctest -j4` 全量下把别的**时序敏感**用例拖成偶发红（本轮 unit_agent_core 即为疑似受害者）。
            # 原子性的**机制级**判据在阶段2（write() 探针），不依赖读频率；本例只需足以覆盖并发窗口即可。
            time.sleep(0.004)


def main():
    if not os.path.exists(os.path.join(REPO, "build", "thin_agent")):
        print("SKIP: build/thin_agent 不存在")
        return 0
    if socket.socket().connect_ex(("127.0.0.1", WS_PORT)) == 0:
        print("SKIP: ws 端口 %d 已被占用（残留实例？）" % WS_PORT)
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
    probe_ok = build_probe()
    if probe_ok:
        open(PROBE_TRACE, "w").close()
        env["LD_PRELOAD"] = PROBE_SO
        env["TA_WRITE_TRACE"] = PROBE_TRACE
    else:
        print("WARN: write() 探针未编译成功 ⇒ 阶段2（机制级判据）不可用，仅阶段1 冒烟")
    open(SVC_STDOUT, "w").close()
    svc = subprocess.Popen(
        ["./build/thin_agent", "--port", str(WS_PORT), "--config", cfg,
         "--profile", "lmstudio_demo", "--dev"],
        env=env, cwd=REPO, stdout=open(SVC_STDOUT, "a"), stderr=subprocess.STDOUT)

    failures = []
    reader = None
    try:
        if not wait_port(WS_PORT):
            print("SKIP: 服务未起来（见 %s）" % SVC_STDOUT)
            return 0
        time.sleep(0.8)

        reader = LogReader(SVC_LOG)
        reader.start()

        conns = []
        for i in range(3):
            ws = websocket.create_connection("ws://127.0.0.1:%d/ws" % WS_PORT, timeout=5)
            ws.recv()
            conns.append(ws)

        # 并发产生日志：多连接反复 ping（每次 ping 都落 last_req 相关行/心跳行），并周期性开关连接
        deadline = time.time() + TRAFFIC_SECONDS
        n_round = 0
        while time.time() < deadline:
            for ws in conns:
                ws.send(json.dumps({"type": "ping", "chat_id": "atomic"}))
                try:
                    ws.recv()
                except Exception:
                    pass
            n_round += 1
            if n_round % 3 == 0:
                # 关掉一个（触发 peer_close + ws-close）、再补一个（触发新连接的日志）
                ws = conns.pop(0)
                try:
                    ws.close()
                except Exception:
                    pass
                ws = websocket.create_connection("ws://127.0.0.1:%d/ws" % WS_PORT, timeout=5)
                ws.recv()
                conns.append(ws)

        # 一记 RST 强断（触发 [ws-error] socket error 行）
        wc = websocket.create_connection("ws://127.0.0.1:%d/ws" % WS_PORT, timeout=5)
        wc.recv()
        wc.send(json.dumps({"type": "ping", "chat_id": "atomic-rst"}))
        try:
            wc.recv()
        except Exception:
            pass
        wc.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        wc.sock.close()
        time.sleep(1.2)      # 等 [ws-error]/[ws-close] 落盘

        for ws in conns:
            try:
                ws.close()
            except Exception:
                pass
        time.sleep(0.8)

        reader.stop_flag = True
        reader.join(timeout=3)
        time.sleep(0.3)

        try:
            with open(SVC_LOG, "rb") as f:
                final_raw = f.read().decode("utf-8", "replace")
        except OSError:
            final_raw = ""
        final_lines = [l.rstrip() for l in final_raw.split("\n") if l.strip()]
        final_set = set(final_lines)

        observed = reader.observed
        print("  读线程: %d 次读、观测 %d 行（去重 %d）；最终日志 %d 行"
              % (reader.reads, len(observed), len(set(observed)), len(final_lines)))

        if len(set(observed)) < MIN_OBSERVED_LINES:
            failures.append("观测行数不足（%d < %d）：用例空转，不构成有效验证"
                            % (len(set(observed)), MIN_OBSERVED_LINES))
        else:
            print("PASS: observed_lines>=%d (%d)" % (MIN_OBSERVED_LINES, len(set(observed))))

        torn = [l for l in observed if l not in final_set]
        if torn:
            print("FAIL: 并发读者观测到 %d 行**无法在最终日志中逐字找到**（= 半行/撕裂）" % len(torn))
            for l in torn[:5]:
                print("   撕裂样本: [%s]" % l)
            failures.append("并发读者看到撕裂行 %d 条（例：%s）" % (len(torn), torn[0][:120]))
        else:
            print("PASS: 并发读者观测到的每一行都能在最终日志中逐字找到（无半行）")

        bad_fmt = [l for l in final_lines if not (l.startswith("[") and "]" in l)]
        if bad_fmt:
            print("FAIL: 最终日志存在格式残片 %d 条" % len(bad_fmt))
            for l in bad_fmt[:3]:
                print("   残片: [%s]" % l)
            failures.append("最终日志存在格式残片 %d 条" % len(bad_fmt))
        else:
            print("PASS: 最终日志每行格式完好（以 [ 开头且含 ]）")
        # ── 阶段2：write() 探针判据（机制级）──
        if probe_ok:
            line_n = part_n = 0
            try:
                with open(PROBE_TRACE, encoding="utf-8", errors="replace") as f:
                    for l in f:
                        if l.startswith("LINE "):
                            line_n += 1
                        elif l.startswith("PART "):
                            part_n += 1
            except OSError:
                pass
            print("  探针: 写日志的 write() 共 %d 笔（LINE=%d, PART=%d）" % (line_n + part_n, line_n, part_n))
            if line_n + part_n < 10:
                failures.append("探针观测到的写次数过少（%d < 10）：用例空转" % (line_n + part_n))
            elif part_n > 0:
                failures.append("日志行非原子：%d 笔 write() 不以换行结束（行被拆开写出，"
                                "并发读者可能看到半行）" % part_n)
            else:
                print("PASS: 每一笔写向日志的 write() 都以换行结束（一行 = 一次 write()）")
    finally:
        if reader is not None:
            reader.stop_flag = True
        try:
            svc.terminate()
            svc.wait(timeout=10)
        except Exception:
            try:
                svc.kill()
            except Exception:
                pass
        # v0.54.29: **SIGTERM 退出路径**守卫（本轮新增视角：进程生命周期）
        #   ① 必须**优雅退出**（returncode==0；若是 -15 说明是被信号打死，未走 flag→return 路径）
        #   ② 日志末尾必须仍是**完整行**（析构时把 pending_ 补换行落盘 ⇒ 不丢内容、不留半行）
        #   ③ 内容不得被截断/清空
        try:
            rc = svc.returncode
            if rc == 0:
                print("PASS: SIGTERM 后优雅退出（returncode=0）")
            else:
                failures.append(f"SIGTERM 后未优雅退出：returncode={rc}（应为 0）")
            final_log = Path(SVC_LOG).read_text(errors="replace")
            if final_log.endswith("\n"):
                print("PASS: SIGTERM 后日志末行完整（pending_ 已补换行落盘）")
            else:
                failures.append("SIGTERM 后日志末行不完整（析构未把 pending_ 落盘）")
            if final_log.count("[ws-") >= 1:
                print("PASS: SIGTERM 后日志内容完好（未被截断/清空）")
            else:
                failures.append("SIGTERM 后日志内容异常（疑似被清空）")
        except Exception as e:   # noqa: BLE001
            failures.append(f"SIGTERM 退出检查异常：{e}")

    if failures:
        print("FAIL: 日志行原子性契约未满足：")
        for f in failures:
            print("  - " + f)
        return 1
    print("ALL PASS: 日志行原子性（并发读者未见半行）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
