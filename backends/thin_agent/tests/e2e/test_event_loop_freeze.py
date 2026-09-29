#!/usr/bin/env python3
"""WS 事件循环活性 e2e：长任务请求执行期间，即时请求（ping）必须仍然及时响应。

背景（长任务白名单律，家族第 5 次复发）：ws_agent_main 用一张**手维护的
长任务类型清单**决定"投递 worker 队列 / 事件循环内联执行"。清单每漏一个
类型，该请求就在 mongoose 事件循环里内联跑完 —— 期间心跳停发、accept 停摆、
全部连接冻结（v0.52.9 decompose / v0.53.19 spawn_agent / v0.53.59 orch 系）。

本测试把"事件循环是否被阻塞"变成可观测指标：mock LLM 延迟 N 秒把长任务
时长放大，另一条连接在长任务运行期间发 ping，测其往返延迟。
  - 控制组：chat（已入队）= 事件循环空闲 → ping 应立即返回
  - 探针组：agent_decompose（历史上漏网）→ 修复前 ping 被拖到任务结束

无 node/ws 或 mock 端口占用 → 打印 SKIP:（ctest SKIP_REGULAR_EXPRESSION 跳过）。
"""
import json
import os
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MOCK_PORT = int(os.environ.get("MOCK_PORT", "13302"))   # v0.53.96: 独占（原与另两个 e2e 抢 1234）
WS_PORT = 18792            # 与 lmstudio/mertics e2e 错开端口
LLM_DELAY_SEC = 6.0        # 单次 LLM 调用的 mock 延迟（放大冻结时长）
PING_BUDGET_MS = 2500      # ping 往返预算：事件循环空闲时应 <100ms
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class LlmMock(BaseHTTPRequestHandler):
    """OpenAI 兼容 mock：延迟 LLM_DELAY_SEC 后返回（FC 路径恒流式）。"""

    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        raw = self.rfile.read(n) if n > 0 else b"{}"
        try:
            req = json.loads(raw.decode("utf-8", errors="replace"))
        except Exception:
            req = {}
        time.sleep(LLM_DELAY_SEC)  # —— 冻结放大器
        if req.get("stream"):
            chunks = [
                {"choices": [{"delta": {"content": "mock"}}]},
                {"choices": [{"delta": {}, "finish_reason": "stop"}]},
                {"choices": [{"delta": {}}],
                 "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}},
            ]
            body = "".join("data: " + json.dumps(c) + "\n\n" for c in chunks) + "data: [DONE]\n\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(body.encode())
        else:
            body = json.dumps({"choices": [{"message": {"content": "mock", "finish_reason": "stop"}}]})
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body.encode())


NODE_DRIVER = r"""
const WebSocket = require('/usr/share/nodejs/ws');
const WS = 'ws://127.0.0.1:' + process.env.WS_PORT + '/ws';

function open(name) {
  return new Promise((res, rej) => {
    const ws = new WebSocket(WS);
    ws.on('open', () => res(ws));
    ws.on('error', rej);
  });
}
function waitOpen(ws) { return new Promise((r) => ws.on('open', r)); }

(async () => {
  const a = await open('A');   // 长任务连接
  const b = await open('B');   // 探针连接
  const results = {};

  // 探针：B 发 ping，测往返延迟（事件循环活性）
  function probePing(label) {
    return new Promise((res) => {
      const t0 = Date.now();
      const onMsg = (d) => {
        try {
          const o = JSON.parse(d);
          if (o.type === 'pong') {
            b.off('message', onMsg);
            results[label] = Date.now() - t0;
            res();
          }
        } catch (e) {}
      };
      b.on('message', onMsg);
      b.send(JSON.stringify({type: 'ping', chat_id: 'freeze-probe'}));
    });
  }

  function waitType(ws, types, timeoutMs) {
    return new Promise((res) => {
      const t0 = Date.now();
      const onMsg = (d) => {
        let o; try { o = JSON.parse(d); } catch (e) { return; }
        if (types.includes(o.type)) { ws.off('message', onMsg); res(Date.now() - t0); }
        else if (Date.now() - t0 > timeoutMs) { ws.off('message', onMsg); res(null); }
      };
      ws.on('message', onMsg);
    });
  }

  // ── 控制组：chat（已入队 → 事件循环应空闲）──
  const chatDone = waitType(a, ['chat_result'], 40000);
  a.send(JSON.stringify({type: 'chat', text: 'hello', chat_id: 'freeze-A', cmd_id: 'c1'}));
  await new Promise((r) => setTimeout(r, 1200));
  await probePing('ping_during_chat_ms');
  await chatDone;

  // ── 探针组：agent_decompose（长任务白名单历史漏网类型）──
  const decDone = waitType(a, ['decompose_result'], 60000);
  a.send(JSON.stringify({type: 'agent_decompose', goal: 'freeze probe', chat_id: 'freeze-A', cmd_id: 'd1'}));
  await new Promise((r) => setTimeout(r, 1200));
  await probePing('ping_during_decompose_ms');
  results.decompose_ms = await decDone;

  console.log('RESULT ' + JSON.stringify(results));
  a.close(); b.close();
  process.exit(0);
})().catch((e) => { console.log('DRIVER_ERROR ' + e); process.exit(2); });
"""


def wait_port(port, timeout=40.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        with socket.socket() as s:
            s.settimeout(0.5)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.3)
    return False


def main():
    node = os.environ.get("NODE", "/root/.hermes/node/bin/node")
    if not os.path.exists(node):
        node = "node"
    if not os.path.exists("/usr/share/nodejs/ws/index.js"):
        print("SKIP: node ws 模块不可用")
        return 0
    # 端口自检（1234 被占用=可能是用户真实 LM Studio，跳过而非误红）
    if socket.socket().connect_ex(("127.0.0.1", MOCK_PORT)) == 0:
        print("SKIP: mock 端口 %d 已被占用" % MOCK_PORT)
        return 0

    srv = ThreadingHTTPServer(("127.0.0.1", MOCK_PORT), LlmMock)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    env = dict(os.environ)
    env["THIN_AGENT_DEV_MODE"] = "1"
    env["WS_PORT"] = str(WS_PORT)
    svc = subprocess.Popen(
        ["./build/thin_agent", "--port", str(WS_PORT), "--config", "config/demo.model.yaml",
         "--profile", "lmstudio_demo", "--dev"],
        env=env, cwd=REPO, stdout=open("/tmp/freeze_svc.log", "w"), stderr=subprocess.STDOUT)
    try:
        if not wait_port(WS_PORT):
            print("SKIP: ws 服务未起来（见 /tmp/freeze_svc.log）")
            return 0
        time.sleep(1.0)
        r = subprocess.run([node, "-e", NODE_DRIVER], capture_output=True, text=True,
                           timeout=180, env=env)
        out = (r.stdout or "") + (r.stderr or "")
        line = [l for l in out.splitlines() if l.startswith("RESULT ")]
        if not line:
            print("SKIP: 驱动未产出结果: " + out[-400:].replace("\n", " "))
            return 0
        res = json.loads(line[0][len("RESULT "):])
        pc = res.get("ping_during_chat_ms")
        pd = res.get("ping_during_decompose_ms")
        ok = pc is not None and pd is not None and pc < PING_BUDGET_MS and pd < PING_BUDGET_MS
        msg = ("ping 事件循环活性：chat 期间 %sms / agent_decompose 期间 %sms（预算 %dms，"
               "mock LLM 单次 %ss）" % (pc, pd, PING_BUDGET_MS, LLM_DELAY_SEC))
        print(("PASS: " if ok else "FAIL: ") + msg)
        return 0 if ok else 1
    finally:
        svc.kill()
        try:
            svc.wait(timeout=10)
        except Exception:
            pass
        srv.shutdown()


if __name__ == "__main__":
    sys.exit(main())
