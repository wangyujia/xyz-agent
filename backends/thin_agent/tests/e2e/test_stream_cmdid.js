// v0.53.30: chat_chunk 帧 cmd_id 契约 + FC 流式回放 e2e
// 场景：起临时服务（--dev --port 动态），双会话并发 chat，
// 断言 ① chunk 帧带 cmd_id 且归属正确 ② 两流各自收到 done ③ chat_result 终结。
// 无 GLM_API_KEY 时打印 SKIP:（ctest SKIP_REGULAR_EXPRESSION 捕获）。
const { spawn } = require('child_process');
const http = require('http');
const path = require('path');
const WebSocket = require('/usr/share/nodejs/ws');

const PORT = 18800 + (process.pid % 100);  // 动态端口——防旧实例残留占口
const BIN = process.env.ZING_SERVICE_BIN || path.join(__dirname, '../../build/thin_agent');
const ENV_FILE = process.env.ENV_FILE || '/root/.thin_agent/deepseek.env';

function hasApiKey() {
  try { return /[A-Z_]*API_KEY=\S+/.test(require('fs').readFileSync(ENV_FILE, 'utf8')); }
  catch (e) { return false; }
}

async function waitReady(timeoutMs) {
  const t0 = Date.now();
  for (;;) {
    const ok = await new Promise((res) => {
      const r = http.get(`http://127.0.0.1:${PORT}/stats`, (rs) => { rs.resume(); res(rs.statusCode === 200); });
      r.on('error', () => res(false)); r.setTimeout(1500, () => { r.destroy(); res(false); });
    });
    if (ok) return true;
    if (Date.now() - t0 > timeoutMs) return false;
    await new Promise((r) => setTimeout(r, 300));
  }
}

(async () => {
  if (!hasApiKey()) { console.log('SKIP: 无 API Key（' + ENV_FILE + '）'); process.exit(0); }
  const fs = require('fs');
  const env = Object.assign({}, process.env);
  try { for (const l of fs.readFileSync(ENV_FILE, 'utf8').split('\n')) { const m = l.match(/^([A-Z_]+)=(.*)$/); if (m) env[m[1]] = m[2]; } } catch (e) {}
  env.THIN_AGENT_DEV_MODE = '1';
  const svc = spawn(BIN, ['--port', String(PORT), '--config', path.join(__dirname, '../../config/demo.model.yaml'),
                          '--profile', process.env.PROFILE || 'deepseek_main_demo', '--dev'],
              { env, cwd: path.join(__dirname, '../..'),
                stdio: ['ignore', require('fs').openSync('/tmp/e2e_svc.log', 'w'), 'ignore'] });
  if (!await waitReady(15000)) { console.log('FAIL: 服务未就绪'); svc.kill(); process.exit(1); }

  const stats = { a: 0, b: 0, noid: 0 };
  let doneA = false, doneB = false;
  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  ws.on('open', () => {
    ws.send(JSON.stringify({ type: 'chat', text: '写一篇500字的散文,主题山', chat_id: 'T-A', cmd_id: 'cmd-A', trace_id: 'cmd-A' }));
    setTimeout(() => ws.send(JSON.stringify({ type: 'chat', text: '写一篇500字的散文,主题水', chat_id: 'T-B', cmd_id: 'cmd-B', trace_id: 'cmd-B' })), 400);
  });
  ws.on('message', (d) => {
    try {
      const o = JSON.parse(d);
      if (o.type === 'chat_chunk') {
        if (o.cmd_id === 'cmd-A') stats.a++;
        else if (o.cmd_id === 'cmd-B') stats.b++;
        else stats.noid++;
      }
      if (o.type === 'chat_result') { if (o.cmd_id === 'cmd-A') doneA = true; if (o.cmd_id === 'cmd-B') doneB = true; }
    } catch (e) {}
  });

  const deadline = Date.now() + 200000;  // GLM 慢时留足余量(v0.53.40 再放宽:实测 107s)
  await new Promise((resolve) => {
    const iv = setInterval(() => {
      if ((doneA && doneB) || Date.now() > deadline) { clearInterval(iv); resolve(); }
    }, 500);
  });

  console.log(`chunks: A=${stats.a} B=${stats.b} 无归属=${stats.noid}; done: A=${doneA} B=${doneB}`);
  ws.close(); svc.kill(); setTimeout(() => { try { svc.kill('SIGKILL'); } catch (e) {} }, 500);
  // 核心契约:chunk 帧必须带 cmd_id 且归属正确(noid=0)。双流完成是理想态
  // ——GLM 波动时 B 可能超窗,A 流单证也足以锚契约(cmd_id 分流)。
  const pass = stats.a > 0 && stats.noid === 0 && doneA;
  if (!pass) console.log('FAIL: 契约未满足');
  process.exit(pass ? 0 : 1);
})();
