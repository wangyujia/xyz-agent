#!/usr/bin/env python3
# e2e_gateway_reconnect_backoff（v0.53.99, R86）
#
# 修复前行为：im_gateway_v2 的 on_timer(3s) 里 `if (!g_agent_conn) agent_connect();`
# **无退避** → agent 长期不可用时每 3s 一次连接尝试，永不收敛（日志刷屏 + 连接 churn），
# 且日志里**没有任何**"下次重试延迟"的可观测信息。
#
# 本判据（读 gw2 的真实轮转日志文件——stdout/cerr 都被重定向进日志，R81 教训）：
#   C1 出现 >=3 条 "retry in <N>ms"（断线被识别并排期）
#   C2 延迟序列**严格递增**且封顶（退避生效；修复前无此类日志 → 判红）
#   C3 15s 内重连尝试次数 <= 6（修复前每 3s 一次 ≈ 5 次/15s，但无递增序列）
#   C4 SIGTERM 干净退出（exit 0）
import os, re, shutil, signal, socket, subprocess, sys, time

REPO = os.environ.get("REPO", "/root/code/thin_agent")
# v0.54.1 (R88): 参数化——v1(thin_agent_gw) 与 v2(thin_agent_gw2) 的"连接生命周期"是孪生
# 实现，R86 只修了 v2；孪生漏网复查律要求**两个二进制都跑同一判据**。
GW = os.environ.get("GW_BIN", os.path.join(REPO, "build", "thin_agent_gw2"))
if not os.path.isabs(GW):
    GW = os.path.join(REPO, GW)
_TAG = os.path.basename(GW)
HOME_DIR = "/tmp/gw_backoff_home_" + _TAG
LOG = os.path.join(HOME_DIR, "logs", "agent_gw.log")
DEAD_AGENT = "ws://127.0.0.1:19999/ws"   # 无监听：连接必失败

fails = []
def ok(name, cond, extra=""):
    print(("PASS: " if cond else "FAIL: ") + name + ("" if cond else "  [" + str(extra) + "]"))
    if not cond: fails.append(name)

if not os.path.exists(GW):
    print("SKIP: 未找到 " + GW); sys.exit(0)

shutil.rmtree(HOME_DIR, ignore_errors=True)
os.makedirs(os.path.join(HOME_DIR, "logs"), exist_ok=True)
env = dict(os.environ)
env["THIN_AGENT_HOME"] = HOME_DIR
env["THIN_AGENT_GW_RECONNECT_BASE_MS"] = "400"    # 可测性钩子：几秒内验证档位递增
env["THIN_AGENT_GW_RECONNECT_CAP_MS"] = "3200"

proc = subprocess.Popen([GW, "--agent", DEAD_AGENT], cwd=REPO, env=env,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                        preexec_fn=os.setsid)
try:
    deadline = time.time() + 25
    while time.time() < deadline:
        if os.path.exists(LOG):
            txt = open(LOG, encoding="utf-8", errors="ignore").read()
            if len(re.findall(r"retry in (\d+)ms", txt)) >= 3:
                break
        time.sleep(0.5)
    time.sleep(1.5)   # 多收集一轮
    proc.send_signal(signal.SIGTERM)
    try:
        rc = proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill(); rc = -9
finally:
    if proc.poll() is None:
        proc.kill()

txt = open(LOG, encoding="utf-8", errors="ignore").read() if os.path.exists(LOG) else ""
delays = [int(x) for x in re.findall(r"retry in (\d+)ms", txt)]
attempts = len(re.findall(r"reconnect attempt", txt))
print("[DBG] retry 序列=" + str(delays) + " attempt 次数=" + str(attempts))

ok("C1 断线被识别并排出 >=3 档重试延迟", len(delays) >= 3, "delays=" + str(delays))
strict = len(delays) >= 3 and all(delays[i] < delays[i + 1] for i in range(len(delays) - 1))
capped = bool(delays) and delays[-1] <= 3200
ok("C2 延迟序列严格递增（退避生效）", strict, "delays=" + str(delays))
ok("C2b 封顶不超 cap", capped, "last=" + str(delays[-1] if delays else None))
ok("C3 重连尝试次数有界（<=6）", attempts <= 6, "attempts=" + str(attempts))
ok("C4 SIGTERM 干净退出", rc == 0, "rc=" + str(rc))

print("e2e:gateway_reconnect_backoff " + ("PASS" if not fails else "FAIL") + " (%d)" % (5 - len(fails)))
sys.exit(0 if not fails else 1)
