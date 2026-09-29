#!/usr/bin/env python3
"""WS API 全类型面扫描（R72 视角：让服务动起来）。

动机：R70 的两个真问题都是"跑出来"的（gdb 栈 / e2e 挂死），而不是读出来的。
本脚本把 AgentServiceWs 里**全部入站 handler 类型**都真打一遍，记录：
  ① 是否有响应（超时=可能挂死）
  ② 期间事件循环是否还活着（另一连接的 ping 往返）
  ③ 进程是否还活着 / 连接是否被服务端关闭
用法：python3 tests/e2e/ws_api_surface_sweep.py [--port 18796]
退出码：0=无异常（响应缺失/挂死/进程死），1=有异常
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# v0.53.96: 独占 mock 端口，避免与同批 e2e 抢 1234（-j4 并行下实测 EADDRINUSE 抖动）。
# 原先三个 e2e 都用 1234（本 sweep / test_event_loop_freeze / test_shutdown_notice），
# 并行时互相占端口 → SKIP 掉（丢覆盖）或直接崩。现各自独占，且可用 env 覆盖。
MOCK_PORT = int(os.environ.get("MOCK_PORT", "13301"))


MOCK_DELAY = float(os.environ.get("MOCK_DELAY", "0"))


class LlmMock(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        if MOCK_DELAY > 0:
            time.sleep(MOCK_DELAY)
        n = int(self.headers.get("Content-Length", 0) or 0)
        if n:
            self.rfile.read(n)
        body = "\n\n".join([
            'data: ' + json.dumps({"choices": [{"delta": {"content": "ok"}}]}),
            'data: ' + json.dumps({"choices": [{"delta": {}, "finish_reason": "stop"}]}),
            'data: ' + json.dumps({"choices": [{"delta": {}}], "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}}),
            'data: [DONE]', '',
        ])
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        self.wfile.write(body.encode())


def collect_types():
    src = open(os.path.join(REPO, "src/core/AgentServiceWs.cpp"), encoding="utf-8").read()
    return sorted(set(re.findall(r'if \(type == "([a-z_0-9]+)"\)', src)))


# 每个类型的最小合法规约（缺参=提前 return error 也算"有响应"，故可有可无）
ARGS = {
    "chat": {"text": "hi"},
    "chat_approve": {"approved": False},
    "agent_decompose": {"goal": "g"},
    "agent_decompose_resume": {"goal_id": 1},
    "agent_dag": {"goal": "g"},
    "agent_synthesize": {"goal": "g"},
    "spawn_agent": {"goal": "g"},
    "kb_add": {"path": "x.md"},
    "kb_status": {},
    "set_project": {"path": "/tmp", "mode": "ro"},
    "cron_add": {"schedule": "0 0 * * *", "prompt": "p"},
    "cron_remove": {"task_id": 1},
    "cron_reply": {"task_id": 1, "text": "t"},
    "task_submit": {"action": "health_report", "idempotency_key": "k"},
    "task_get": {"task_id": "x"},
    "task_cancel": {"task_id": "x"},
    "task_replay": {"task_id": "x"},
    "task_audit": {"task_id": "x"},
    "goal_add": {"description": "d"},
    "goal_update": {"goal_id": 1, "status": "active"},
    "goal_delete": {"goal_id": 1},
    "goal_auto_reason": {"goal_id": 1},
    "hook_register": {"event": "tool_pre", "shell_cmd": "true"},
    "hook_unregister": {"id": "hook_0"},
    "kanban_push": {"tasks": [{"title": "t"}]},
    "kanban_push_batch": {"tasks": [{"title": "t"}]},
    "kanban_run": {},
    "kanban_clear": {},
    "kanban_status": {},
    "agent_message": {"target": "a", "text": "t"},
    "agent_inbox": {"agent_id": "a"},
    "agent_inbox_drain": {"agent_id": "a"},
    "agent_broadcast": {"text": "t"},
    "bb_write": {"key": "k", "value": "v"},
    "bb_read": {"key": "k"},
    "bb_keys": {},
    "bb_clear": {},
    "role_register": {"name": "r", "description": "d"},
    "role_remove": {"name": "r"},
    "role_list": {},
    "skill_remove": {"name": "n"},
    "skill_maintain": {"stale_days": 30},
    "switch_model": {"model": "m", "gguf_path": "/nonexistent.gguf"},
    "memory_search": {"query": "q"},
    "memory_history": {"limit": 3},
    "memory_summary": {},
    "memory_recent": {},
    "correction_record": {"text": "t"},
    "correction_stats": {},
    "monitor_watch_file": {"path": "/tmp/x"},
    "monitor_start": {},
    "monitor_stop": {},
    "monitor_status": {},
    "checkpoint_save": {"label": "l"},
    "checkpoint_rollback": {"label": "l"},
    "checkpoint_list": {},
    "trace_list": {"limit": 3},
    "event_recent": {},
    "metrics": {},
    "cache_stats": {},
    "usage_stats": {},
    "web_search_config": {},
    "set_project_mode": {"mode": "ro"},
    "get_project": {},
    "orch_status": {},
    "agent_decompose_scan": {},
    "goal_proactive_enable": {"enabled": False},
    "goal_list": {},
    "cron_list": {},
    "cron_stats": {},
    "skill_list": {},
    "skill_stats": {},
    "hook_list": {},
    "task_list": {},
    "summarize": {},
    "action": {"action": "health_report"},
    "ping": {},
    "status": {},
    "chat_abort": {},
}

DRIVER = r"""
const WebSocket = require('/usr/share/nodejs/ws');
const WS = 'ws://127.0.0.1:' + process.env.WS_PORT + '/ws';
const TYPES = JSON.parse(process.env.SWEEP_TYPES);
const ARGS = JSON.parse(process.env.SWEEP_ARGS);
const TIMEOUT_MS = Number(process.env.SWEEP_TIMEOUT || 12000);
const GRACE_MS = Number(process.env.SWEEP_GRACE || 15000);  // v0.53.92 慢响应宽限

// v0.54.15 (R93 A1 归因定案): 客户端侧的**错误/关闭原因必须留痕**。
// 此前 openWs 只挂 `ws.on('error', rej)`——open 之后出错时 promise 已 settle，Node 对已 settle 的
// promise 调 reject 是 **no-op** ⇒ 连接为什么断在整条排查链里**从未被记录过**（A1 谜团的客户端
// 那一半一直是空白）。现在无论 promise 状态都落盘。
const CLIENT_ERRORS = [];
const CLIENT_CLOSES = [];
function openWs() {
  return new Promise((res, rej) => {
    const ws = new WebSocket(WS);
    ws.on('open', () => res(ws));
    ws.on('error', (e) => {
      CLIENT_ERRORS.push(String((e && e.message) || e));
      rej(e);
    });
    ws.on('close', (code, reason) => {
      CLIENT_CLOSES.push({code, reason: reason ? reason.toString() : ''});
    });
    setTimeout(() => rej(new Error('open timeout')), 8000);
  });
}
// 关闭码 → 归因（与服务端 `is_draining`/`peer_close` 签名配套解读；详见
// tests/e2e/test_ws_close_attribution.py 的「关闭来源 → 服务端日志签名」对照表）
function classifyClose(code) {
  if (code === 1006) return '对端异常断开（无 CLOSE 帧 ⇒ **非服务端主动关闭**）';
  if (code === 1000) return '正常关闭（谁发起不明，须看服务端 is_draining）';
  if (code === 1001) return 'going away（任一方主动关闭）';
  if (code === 1002) return '协议错误（服务端会记 peer_close code=1002）';
  if (code === 1007) return '非法 UTF-8（服务端会记 peer_close code=1007）';
  if (code === 1005) return '无状态码';
  return '其它（code=' + code + '）';
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  console.error('DRIVER_START types=' + TYPES.length + ' ws=' + WS);
  const a = await openWs();
  console.error('DRIVER connA');
  const b = await openWs();
  console.error('DRIVER connB');
  let closedByServer = false;   // 历史字段名（**误名**）：实为"connA 的 close 事件触发了"
  // v0.54.4 (R91): 记录 **关闭码与原因**。此前只记一个布尔，导致"服务端主动关闭"这一
  // 结论无法细分（1000 正常关闭 / 1001 going away / 1006 异常断开 / 1005 无码），
  // 也无法与服务端 [ws-close] 日志（by_us/is_closing/idle_ms）对齐。
  let closeCode = -1, closeReason = '';
  let SERVER_TAIL_ON_CLOSE = [];
  a.on('close', (code, reason) => {
    closedByServer = true;
    closeCode = code;
    closeReason = reason ? reason.toString() : '';
    // v0.54.15: **关闭瞬间**抓服务端日志尾——那一刻的 [ws-close]/[ws-ctl]/[ws-error] 行就是本次
    // 关闭的签名。延后抓会被**后续连接的正常关闭污染**（实测：收尾 retry 连接的 c.close() 打出的
    // `peer_close code=-1` 会把归因带偏成"对端发 CLOSE 帧"）。
    try {
      const lp = String(process.env.SWEEP_HOME || '') + '/logs/agent_svc.log';
      SERVER_TAIL_ON_CLOSE = require('fs').readFileSync(lp, 'utf8').split('\n')
        .filter((l) => /\[ws-close\]|\[ws-ctl\]|\[ws-error\]|\[ws-heartbeat\]/.test(l)).slice(-30);
    } catch (e) { SERVER_TAIL_ON_CLOSE = ['(日志读取失败: ' + e + ')']; }
    // v0.54.15: 把**在飞请求**也归入连接级事件。旧实现只在**下一轮迭代开头**标 closed，
    // 于是被标的是"关闭后发现的**下一个**类型"，而真正在飞的项既不是 closed 也收不到回复
    // ⇒ 被当硬 BAD 且**不参与新连接重试**（A1 谜团里那条"判据解读陷阱"的实体化）。
    try {
      const inflight = rows.slice().reverse().find((r) => !r.replied);
      if (inflight) { inflight.closed = true; inflight.closedInflight = true; }
    } catch (e) {}
  });

  const results = [];
  const probeB = () => new Promise((res) => {
    const t0 = Date.now();
    const onMsg = (d) => { let o; try { o = JSON.parse(d); } catch (e) { return; }
      if (o.type === 'pong') { b.off('message', onMsg); res(Date.now() - t0); } };
    b.on('message', onMsg);
    b.send(JSON.stringify({type: 'ping', chat_id: 'sweep-probe'}));
    setTimeout(() => { b.off('message', onMsg); res(-1); }, 8000);
  });

  // v0.53.92: 全局监听 + 待决表。慢响应不该靠"每类型硬等宽限"或"硬编码慢类型清单"，
  // 而是**收尾排空（drain）**：全类型发完后统一等未决项，慢而有答=SLOW，始终不答=FAIL。
  const rows = [];
  const byCmd = new Map();          // cmd → row
  const sentAt = new Map();         // cmd → ms 时间戳
  a.on('message', (d) => {
    let o; try { o = JSON.parse(d); } catch (e) { return; }
    const row = o.cmd_id ? byCmd.get(o.cmd_id) : null;
    if (row && !row.replied) {
      row.replied = true;
      row.respType = String(o.type || '');
      row.latency = Date.now() - sentAt.get(o.cmd_id);
      row.late = row.latency > TIMEOUT_MS;
    }
  });

  for (const t of TYPES) {
    const cmd = 'sw-' + t;
    const payload = Object.assign({type: t, chat_id: 'sweep-' + t, cmd_id: cmd}, ARGS[t] || {});
    const row = {type: t, replied: false, respType: '', latency: -1, loopAliveMs: -2, late: false};
    rows.push(row);
    byCmd.set(cmd, row);
    if (closedByServer) { row.closed = true; break; }
    console.error('DRIVER send ' + t);
    sentAt.set(cmd, Date.now());
    a.send(JSON.stringify(payload));
    // 一级预算：仅等待"够快"的响应（慢的留给 drain 阶段，不阻塞后续类型）
    for (let i = 0; i < Math.ceil(TIMEOUT_MS / 100); i++) {
      if (row.replied) break;
      await sleep(100);
    }
    const receivedFast = row.replied;
    row.loopAliveMs = await probeB();     // 该请求之后事件循环是否仍活（挂死判据）
    require('fs').writeSync(2, '  [progress] ' + t + ' replied=' + row.replied +
                  ' lat=' + row.latency + ' probe=' + row.loopAliveMs +
                  (receivedFast ? '' : ' (待决)') + '\n');
  }

  // ── 收尾排空：慢而有答 → SLOW（late）；始终不答 → replied=false（判 FAIL）──
  // v0.53.99 (R86): 排空预算改为**按进展驱动**，不再用固定 40s。
  // 触发原因（实测，R80 家族复发）：满负载（-j4 全量 129 项）下慢类型（goal_auto_reason）
  // 超过固定预算 → 被判 BAD 假红。判"慢 ≠ 挂死"的依据应当是**是否仍在推进**（待决数
  // 是否下降），而不是掐一个时间常数——固定预算在负载变化时必然要么假红要么白等。
  // 规则：每轮 roundMs(默认 10s)；待决数**下降**→继续；**不下降**（一轮无进展）→判卡住停止。
  const pending0 = rows.filter((r) => !r.replied && !r.closed).length;
  const roundMs = Number(process.env.SWEEP_DRAIN_ROUND || 10000);
  const maxRounds = Number(process.env.SWEEP_DRAIN_ROUNDS || 12);
  if (pending0 > 0) {
    console.error('DRIVER drain: 待决 ' + pending0 + ' 项，按进展驱动（每轮 ' + (roundMs / 1000) +
                  's，最多 ' + maxRounds + ' 轮）');
    let prev = pending0;
    for (let round = 0; round < maxRounds; ++round) {
      const t0 = Date.now();
      while (rows.some((x) => !x.replied && !x.closed) && Date.now() - t0 < roundMs) await sleep(200);
      const left = rows.filter((x) => !x.replied && !x.closed).length;
      const alive = await probeB();   // 真挂死判据 = 探针（R80 律：探针活着就不是挂死）
      console.error('DRIVER drain 轮 ' + (round + 1) + ': 待决 ' + left + ' 探针 ' + alive + 'ms');
      if (left === 0) break;
      if (alive < 0) {   // 事件循环真的不响应 → 挂死，停止等待并如实报 BAD
        console.error('DRIVER drain: 事件循环无响应（探针超时），停止等待');
        break;
      }
      // 探针活着 ⇒ 服务仍在工作。多项目时"一轮无进展"说明排队停摆 → 停；
      // 单项目时无进展是**正常**的（就是这一条慢，R80 实测 goal_auto_reason 12~30s），
      // 继续等到轮次用尽（人为预算不掩盖真挂死，也不把慢判成死）。
      if (left >= prev && left > 1) {
        console.error('DRIVER drain: 多项无进展（剩 ' + left + ' 项），停止等待');
        break;
      }
      prev = left;
    }
  }
  // ── v0.54.1 (R88): **连接级事件不得冒充 handler 故障** ──
  // 若中途服务端关闭了连接 A（实测：满负载 -j4 下偶发，`closedByServer=true`），剩余类型
  // 会被标 closed —— 那是**连接生命周期事件**，不等于"handler 不响应/挂死"。判据：
  // 新开一条连接把这些类型重发一次（上限 6 项 × 8s）：能答 ⇒ 记 RECONNECT（不计 FAIL，
  // 但打印出来）；仍不答 ⇒ 保持 BAD（真故障）。
  const closedRows = rows.filter((r) => r.closed && !r.replied);
  if (closedRows.length > 0) {
    const retryRows = closedRows.slice(0, 6);
    try {
      const c = await openWs();
      console.error('DRIVER retry on fresh conn: ' + retryRows.length + '/' + closedRows.length + ' 类型');
      let okCount = 0;
      for (const r of retryRows) {
        const cmd = 'rt-' + r.type;
        await new Promise((res) => {
          const t0 = Date.now();
          const onMsg = (d) => {
            try {
              const j = JSON.parse(d.toString());
              if (j.cmd_id === cmd) {
                r.replied = true; r.respType = j.type || ''; r.latency = Date.now() - t0;
                r.reconnected = true; okCount++;
                c.off('message', onMsg); res();
              }
            } catch (e) {}
          };
          c.on('message', onMsg);
          c.send(JSON.stringify(Object.assign({type: r.type, chat_id: 'sweep-' + r.type, cmd_id: cmd}, ARGS[r.type] || {})));
          setTimeout(() => { c.off('message', onMsg); res(); }, 8000);
        });
      }
      console.error('DRIVER retry 结果: ' + okCount + '/' + retryRows.length + ' 在新连接上有答');
      try { c.close(); } catch (e) {}
    } catch (e) { console.error('DRIVER retry 开连失败: ' + e); }
  }

  for (const r of rows) {
    if (!r.replied && !r.closed) r.loopAliveMs = r.loopAliveMs < 0 ? r.loopAliveMs : await probeB();
  }
  try {
    require('fs').writeFileSync(process.env.SWEEP_OUT,
      rows.map((r) => JSON.stringify(r)).join('\n') + '\n');
  } catch (e) {}
  console.log('SWEEP ' + JSON.stringify({results: rows, closedByServer, closeCode, closeReason,
    closeClass: closedByServer ? classifyClose(closeCode) : '',
    clientErrors: CLIENT_ERRORS, clientCloses: CLIENT_CLOSES,
    serverTailOnClose: SERVER_TAIL_ON_CLOSE}));
  process.exit(0);
})().catch((e) => { console.log('DRIVER_ERR ' + e); process.exit(2); });
"""


def _classify_close_lines(lines, scope):
    """按签名给出一组服务端日志行的归因（scope 只用于打印时标出证据范围）。"""
    reaper = [l for l in lines if "by_us(reaper)=1" in l]
    peer = [l for l in lines if "[ws-ctl] peer_close" in l]
    err = [l for l in lines if "[ws-error]" in l]
    if reaper:
        return "[%s] 服务端 reaper 主动关闭（真·服务端发起）: %s" % (scope, reaper[-1].strip()[-95:])
    if peer:
        return "[%s] 对端发 CLOSE 帧（服务端仅 echo ⇒ 非服务端主动）: %s" % (scope, peer[-1].strip()[-95:])
    if err:
        return "[%s] 对端 RST 强断（socket error ⇒ 非服务端主动）: %s" % (scope, err[-1].strip()[-95:])
    return ("[%s] 对端无 CLOSE 帧断开（FIN/1006）——服务端无任何主动关闭动作"
            "（该范围内 0 条 peer_close / 0 条 ws-error / 0 条 reaper）" % scope)


def _attribute_server_close(log_path, tail=None):
    """从服务端日志给出「connA 为何关闭」的**可判定归因**（v0.54.15 / R93 A1 定案）。

    签名对照表（受控实验实测，已由 tests/e2e/test_ws_close_attribution.py 机械固定）：
      有 peer_close 行                 → 对端发 CLOSE 帧（服务端仅 echo ⇒ **非服务端主动**）
      无 peer_close + 有 ws-error 行   → 对端 RST 强断（**非服务端主动**）
      无 peer_close + 无 ws-error      → 对端无 CLOSE 帧断开（FIN/1006）——**服务端无主动动作**
      by_us(reaper)=1                  → 服务端 reaper 主动关闭（真·服务端发起）

    **证据范围**：优先用 `tail`（驱动在**关闭瞬间**抓的服务端日志尾，由 SWEEP 报文带回来）——
    实测教训：用全区日志会把**收尾 retry 连接正常 close 打出的 `peer_close code=-1`** 当成 connA 的
    归因（"提到"≠"本次"）。缺 tail 时退回全区日志并在输出里标出范围。
    """
    if tail:
        lines = [l for l in tail if l and not l.startswith("(")]
        if lines:
            return _classify_close_lines(lines, "关闭瞬间日志尾")
    try:
        with open(log_path, encoding="utf-8", errors="ignore") as f:
            lines = f.readlines()
    except OSError:
        return "无法读取服务端日志（归因不可用）"
    return _classify_close_lines(lines, "全区日志·退路")


def wait_port(port, timeout=45.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        with socket.socket() as s:
            s.settimeout(0.5)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.3)
    return False


def main():
    global MOCK_PORT   # v0.54.4 (R91): 端口被占时会改写（见下），须显式 global 否则变局部变量
    try:
        sys.stdout.reconfigure(line_buffering=True)  # 被 kill 时不留空输出（R72 踩）
    except Exception:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18796)
    ap.add_argument("--timeout", type=int, default=3000)
    ap.add_argument("--mock-delay", type=float, default=0.0)
    args = ap.parse_args()
    node = os.environ.get("NODE", "/root/.hermes/node/bin/node")
    types = collect_types()
    print("入站 handler 类型 %d 个" % len(types))

    # v0.54.4 (R91): **端口被占不再静默 SKIP**（SKIP=测试静默通过=谎绿）。改为自动挑一个
    # 空闲端口继续跑。原实现遇占即 SKIP，等于"并行时这个 e2e 直接不测"。
    if socket.socket().connect_ex(("127.0.0.1", MOCK_PORT)) == 0:
        _orig = MOCK_PORT
        for cand in range(13311, 13340):
            if socket.socket().connect_ex(("127.0.0.1", cand)) != 0:
                MOCK_PORT = cand
                break
        print("NOTE: mock 端口 %d 被占用 → 改用 %d（不再 SKIP）" % (_orig, MOCK_PORT))
    # 目标端口若被残留实例占用（R70 实锤：自死锁/被杀的实例不崩不退一直占口，
    # 新实例 bind 失败即 exit），必须明确报 SKIP 而不是"驱动未产出结果"
    if socket.socket().connect_ex(("127.0.0.1", args.port)) == 0:
        print("SKIP: ws 端口 %d 已被占用（残留实例？先清场再跑）" % args.port)
        return 0
    globals()["MOCK_DELAY"] = args.mock_delay  # 放大 LLM 时长用（冻结窗/LockHeld 探测）
    srv = ThreadingHTTPServer(("127.0.0.1", MOCK_PORT), LlmMock)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    env = dict(os.environ)
    env["THIN_AGENT_DEV_MODE"] = "1"
    # v0.54.1 (R88): **隔离 THIN_AGENT_HOME**。此前本 e2e 用默认 home（~/.thin_agent）跑，
    # 与其它 e2e 及人工调试共用：①goal 表会不断累积（goal_auto_reason 逐目标串行跑完整
    # chat 管线 → 累积后单请求 42s+，把 drain 窗口拉到分钟级）②日志/会话表/DB 互相污染
    # （实测同一份 agent_svc.log 里混着多个测试实例的启动记录，无法定位问题）。
    _iso_home = os.environ.get("SWEEP_HOME", "/tmp/sweep_iso_home")
    shutil.rmtree(_iso_home, ignore_errors=True)
    os.makedirs(os.path.join(_iso_home, "logs"), exist_ok=True)
    env["THIN_AGENT_HOME"] = _iso_home
    env["WS_PORT"] = str(args.port)
    env["SWEEP_TYPES"] = json.dumps(types)
    env["SWEEP_ARGS"] = json.dumps({k: v for k, v in ARGS.items() if k in types})
    env["SWEEP_TIMEOUT"] = str(args.timeout)
    env["SWEEP_OUT"] = "/tmp/sweep_results.jsonl"
    # 用 lmstudio_demo profile + 本机 mock：LLM 路径真跑但零网络依赖
    #
    # v0.53.99 (R86) 修：此前这里写了 /tmp/sweep_profile.yaml **但从未把它传给服务**
    # （--config 传的是 config/demo.model.yaml，其 lmstudio_demo 的 api_base 为空 →
    # 默认 127.0.0.1:1234）⇒ mock 起在 13301 却**从未被调用**，所谓"LLM 路径真跑"实际跑的是
    # **LLM 连接失败重试链**：表现为 goal_auto_reason 单请求 42s（实测 42257ms）、
    # 延迟随机波动 → 判据在负载稍高时假红（"测试辅助静默失败"族——模块注释与事实不符）。
    # 现在写一份**profile 名匹配**的配置并真正传给服务，mock 生效后 LLM 路径是真跑的。
    cfg = "/tmp/sweep_profile.yaml"
    open(cfg, "w").write(
        "profiles:\n"
        "  lmstudio_demo:\n"
        "    mode: cloud\n"
        "    provider: lmstudio\n"
        "    name: m\n"
        '    api_base: "http://127.0.0.1:%d/v1"\n'
        "    request_timeout_ms: 30000\n"
        "    max_completion_tokens: 512\n"
        "    fallback: offline\n" % MOCK_PORT)
    proc = subprocess.Popen(
        ["./build/thin_agent", "--port", str(args.port), "--config", cfg,
         "--profile", "lmstudio_demo", "--dev"],
        env=env, cwd=REPO, stdout=open("/tmp/sweep_svc_%d.log" % args.port, "w"), stderr=subprocess.STDOUT)
    try:
        if not wait_port(args.port):
            print("SKIP: 服务未起来（见 /tmp/sweep_svc_<port>.log）")
            return 0
        time.sleep(1.0)
        open("/tmp/sweep_results.jsonl", "w").close()
        # v0.53.92 两级预算：一级 args.timeout（默认 3s）+ 宽限 15s；驱动总超时按
        # "少量慢类型"估算（避免 80 类型 × 18s 的总时长爆炸）
        r = subprocess.run([node, "-e", DRIVER], capture_output=True, text=True,
                           timeout=(args.timeout + 15000) / 1000.0 * 8 + 300, env=env)
        out = (r.stdout or "") + (r.stderr or "")
        line = [l for l in out.splitlines() if l.startswith("SWEEP ")]
        alive = proc.poll() is None
        if not line:
            print("FAIL: 驱动未产出结果（服务存活=%s）: %s" % (alive, out[-500:].replace("\n", " ")))
            return 1
        data = json.loads(line[0][len("SWEEP "):])
        # FAIL 判据（v0.53.92 收紧语义）：①连接被服务端关闭 ②探针失败（事件循环冻结）
        # ③一级+宽限（默认 3s+15s）后仍无任何响应 = 静默丢弃。慢响应（late）不算 FAIL。
        bad = [x for x in data["results"] if not x.get("replied") or x.get("closed")
               or x.get("loopAliveMs", 1) < 0]
        # SLOW = 一级预算内没回但最终回了（late）：信息性，不计 FAIL
        slow = sorted([x for x in data["results"] if x.get("late") or x.get("latency", 0) > 3000],
                      key=lambda x: -(x.get("latency") or 0))
        # v0.54.15 (R93 A1 归因定案)：把 "连接被服务端关闭" 从**误名**改成**可判定的归因**。
        # 旧文案只是"a 的 close 事件触发了"——受控实验证明 1006（对端无 CLOSE 帧断开）也长这样，
        # 而此时服务端**没有任何主动关闭动作**。现在叠加三层证据：客户端关闭码/error + 服务端签名。
        print("服务进程存活=%s connA 关闭事件=%s 关闭码=%s 原因=%r"
              % (alive, data["closedByServer"], data.get("closeCode"), data.get("closeReason")))
        if data.get("closedByServer"):
            print("  客户端侧: 关闭码归因=%s | error 事件=%s | close 记录=%s"
                  % (data.get("closeClass") or "(未分类)",
                     data.get("clientErrors") or "无", data.get("clientCloses") or "无"))
            print("  服务端侧: %s" % _attribute_server_close(
                os.path.join(_iso_home, "logs", "agent_svc.log"),
                tail=data.get("serverTailOnClose")))
        print("无响应/被关/探针超时: %d 项" % len(bad))
        for x in bad[:20]:
            print("   BAD", json.dumps(x, ensure_ascii=False))
        if slow:
            print("慢响应（超一级预算、宽限内回复） %d 项:" % len(slow))
            for x in slow[:10]:
                print("   SLOW", x["type"], x.get("latency"), "ms", "late=" + str(x.get("late")))
        # v0.53.99 (R86) 回归守卫：goal_auto_reason 曾对**每个** goal 串行跑完整 chat/FC
        # 管线（无上限）→ 实测单请求 42~44s（随目标数线性增长）。限量后应在秒级；
        # 这里加一条 20s 硬上限，防止"限量"被改回无界（该缺陷的表现就是分钟级延迟）。
        ga = [x for x in data["results"] if x.get("type") == "goal_auto_reason"]
        ga_lat = ga[0].get("latency", -1) if ga else -1
        ga_ok = (not ga) or (0 <= ga_lat < 20000)
        if not ga_ok:
            print("   FAIL goal_auto_reason 延迟 %s ms 超 20s（批量串行无上限回归？）" % ga_lat)
        reconn = [x for x in data["results"] if x.get("reconnected")]
        if reconn:
            print("连接级事件（connA 关闭；新连接上这些类型有答——不计 FAIL） %d 项:"
                  % len(reconn))
            for x in reconn[:6]:
                print("   RECONNECT", x["type"], x.get("latency"), "ms",
                      "（关闭时在飞）" if x.get("closedInflight") else "")
        # v0.54.1: BAD 的定义收紧为"**真**不响应"：新连接上重发仍不答才算（连接级事件已单列）
        bad = [x for x in bad if not x.get("reconnected")]
        # v0.54.4 (R91): **归因埋点守卫**——本 e2e 结束后服务端日志必须含 [ws-close] 归因行
        # （by_us/is_closing/idle_ms）。R88 那次"closedByServer=true"在服务端毫无痕迹、无法归因；
        # 埋点补齐后加此断言，防止后人删掉埋点又回到"故障只能猜"的状态。
        _svc_log = os.path.join(_iso_home, "logs", "agent_svc.log")
        _close_lines = []
        try:
            with open(_svc_log, encoding="utf-8", errors="ignore") as f:
                _close_lines = [l for l in f if "[ws-close]" in l]
        except OSError:
            pass
        print("服务端关闭归因行 %d 条（样例：%s）"
              % (len(_close_lines), _close_lines[-1].strip()[-90:] if _close_lines else "无"))
        ws_close_guard = len(_close_lines) > 0
        if not ws_close_guard:
            print("FAIL: 服务端日志无 [ws-close] 归因行——R91 埋点被删除或未生效"
                  "（关闭原因将无法归因，R88 的 closedByServer 谜团会重演）")
        ok = alive and not bad and ga_ok and ws_close_guard
        print(("PASS: 全类型面扫描无挂死/无崩溃（%d 类型）" % len(data["results"])) if ok else "FAIL: 见上")
        return 0 if ok else 1
    finally:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass
        srv.shutdown()


if __name__ == "__main__":
    sys.exit(main())
