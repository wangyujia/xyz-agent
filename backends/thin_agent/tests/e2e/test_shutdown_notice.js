// e2e_shutdown_notice（v0.53.93）
//
// 停机收尾守卫。修复前的行为（本测试正是为它写）：
//   ① 主线程在 join 之前就把 g_mgr_ptr 置空 → flush_pending_sends 里 `if (g_mgr_ptr)`
//      为假 → **在飞任务刚算出的结果**与**排队任务的任何通告**全部静默丢弃
//      （停机期间白烧 CPU/LLM 配额）；
//   ② worker 停机时**排空队列**（停机要等完整个队列，FC/LLM 分钟级），且排空分支
//      绕过同 session 串行（直接取队首）；
//   ③ 排队请求**零通告**：客户端只能靠 R76 的"连接中断"兜底文案。
//
// 本测试：慢 mock(6s) + 同 chat_id 的 A/B 两条请求（B 必然排队在 A 后面）→ SIGTERM：
//   C1 排队请求 B 收到**带 cmd_id 的停机通告**（code 29001）
//   C2 在飞请求 A 的**结果仍被投递**（不再因置空 mgr 而丢失）
//   C3 进程在 20s 内退出（不因排队任务而延长停机）
//   C4 服务端日志可观测：`shutdown: dropped N queued request(s)`
const { spawn } = require('child_process');
const http = require('http');
const WebSocket = require('/usr/share/nodejs/ws');

const PORT = Number(process.env.WS_PORT || 18799);
const MOCK_PORT = Number(process.env.MOCK_PORT || 13303);  // v0.53.96: 独占端口（原 1234 与另两个 e2e 冲突）
const MOCK_DELAY_MS = Number(process.env.MOCK_DELAY_MS || 6000);
const REPO = process.env.REPO || '.';
const HOME = process.env.SD_HOME || ('/tmp/sd_home_' + PORT);
require('fs').mkdirSync(HOME, { recursive: true });

let pass = 0, fail = 0;
function ok(name, cond, extra) {
  if (cond) { pass++; console.log('  PASS: ' + name); }
  else { fail++; console.log('  FAIL: ' + name + (extra ? ' — ' + extra : '')); }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ── 慢 mock：让"在飞任务"足够长，从而暴露"结果是否被投递"────────────────
let mockHits = 0;
const mock = http.createServer((req, res) => {
  let body = '';
  req.on('data', (d) => { body += d; });
  req.on('end', () => {
    mockHits++;
    setTimeout(() => {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({
        id: 'mock-1', object: 'chat.completion', created: Math.floor(Date.now() / 1000),
        model: 'mock', choices: [{ index: 0, message: { role: 'assistant', content: 'pong' },
                                  finish_reason: 'stop' }],
        usage: { prompt_tokens: 3, completion_tokens: 1, total_tokens: 4 },
      }));
    }, MOCK_DELAY_MS);
  });
});

(async () => {
  // 绑定失败必须**明确报错**（原先无自检：端口被占时直接抛 EADDRINUSE 栈，
  // 表现为 -j4 并行下的随机崩溃——v0.53.96 实测复现：sweep 占 1234 时本测试即崩）
  await new Promise((res, rej) => {
    mock.once('error', (e) => rej(new Error('mock 端口 ' + MOCK_PORT + ' 绑定失败: ' + e.message)));
    mock.listen(MOCK_PORT, '127.0.0.1', res);
  });

  const srv = spawn('./build/thin_agent',
    ['--port', String(PORT), '--config', 'config/demo.model.yaml', '--profile', 'lmstudio_demo', '--dev'],
    { cwd: REPO, env: Object.assign({}, process.env,
        { THIN_AGENT_DEV_MODE: '1', THIN_AGENT_HOME: HOME }) });
  let srvOut = '';
  srv.stdout.on('data', (d) => { srvOut += d.toString(); });
  srv.stderr.on('data', (d) => { srvOut += d.toString(); });

  // 就绪检测
  let ready = false;
  for (let i = 0; i < 60; i++) {
    await sleep(500);
    const net = require('net');
    ready = await new Promise((res) => {
      const s = net.connect(PORT, '127.0.0.1');
      s.on('connect', () => { s.destroy(); res(true); });
      s.on('error', () => res(false));
      setTimeout(() => { s.destroy(); res(false); }, 500);
    });
    if (ready) break;
  }
  ok('C0 服务就绪（mock 已起）', ready, 'port=' + PORT);

  const ws = new WebSocket('ws://127.0.0.1:' + PORT + '/ws');
  const frames = [];
  ws.on('message', (d) => { try { frames.push(JSON.parse(d.toString())); } catch (e) {} });
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  await sleep(300);

  // 同 chat_id 两条：A 在飞（mock 6s），B 排队
  ws.send(JSON.stringify({ type: 'chat', chat_id: 'sd-x', cmd_id: 'A', text: 'hello A' }));
  await sleep(500);
  ws.send(JSON.stringify({ type: 'chat', chat_id: 'sd-x', cmd_id: 'B', text: 'hello B' }));
  await sleep(1200);          // 让 B 确实进入队列（A 仍在飞）

  const t0 = Date.now();
  srv.kill('SIGTERM');

  // 等进程退出
  let exitCode = null, exitSignal = null;
  const exitCode2 = await new Promise((res) => {
    srv.on('close', (c, sig) => { exitSignal = sig; res(c); });
    setTimeout(() => res('TIMEOUT'), 25000);
  });
  exitCode = exitCode2;
  const exitMs = Date.now() - t0;
  await sleep(300);

  const bNotice = frames.find((f) => f.cmd_id === 'B' && f.type === 'error');
  ok('C1 排队请求 B 收到带 cmd_id 的停机通告',
     !!bNotice, 'frames=' + JSON.stringify(frames.map((f) => f.type + ':' + (f.cmd_id || '-'))));
  ok('C1 通告 code=29001 且说明原因',
     !!bNotice && bNotice.code === 29001 && /shutting down/.test(String(bNotice.message || '')),
     bNotice ? JSON.stringify(bNotice) : '');
  // C2 必须看**终结果**帧（thinking/chat_chunk 是停机前就到的流式进度，不构成"结果送达"）
  const aResult = frames.filter((f) => f.cmd_id === 'A' &&
    f.type !== 'thinking' && f.type !== 'chat_chunk');
  ok('C2 在飞请求 A 的**终结果**仍被投递（未因置空 mgr 丢失）', aResult.length > 0,
     'A all=' + frames.filter((f) => f.cmd_id === 'A').map((f) => f.type).join(',') +
     ' result=' + aResult.map((f) => f.type).join(','));
  // v0.53.96: 判据只锁"**快速退出**"（本缺陷的表现是修前 25s 不退出=TIMEOUT）。
  // 不锁"退出码必须 0"——那会把"被信号终结"（exitCode=null/signal=SIGTERM）判红，
  // 与"是否被队列拖住"无关（优雅性已由 C1/C2/C4 覆盖）。v0.53.96 实测：三 e2e 并发
  // （CPU 争用）时进程 10.8s 内退出但 exitCode=null → 判据假红。
  ok('C3 进程快速退出（不排空队列 → 停机不随队列延长，<20s）',
     exitCode !== 'TIMEOUT' && exitMs < 20000,
     'exit=' + exitCode + ' signal=' + exitSignal + ' ms=' + exitMs);
  // std::cout/cerr 被 RotatingLogBuf 重定向进 $THIN_AGENT_HOME/logs/agent_svc.log
  let logText = '';
  try {
    const logsDir = require('path').join(HOME, 'logs');
    const files = require('fs').readdirSync(logsDir).filter((f) => f.endsWith('.log'));
    for (const f of files) logText += require('fs').readFileSync(require('path').join(logsDir, f), 'utf8');
  } catch (e) { logText = 'READ_ERR ' + e.message; }
  ok('C4 服务端日志可观测（dropped N queued request(s)）',
     /shutdown: dropped \d+ queued request\(s\)/.test(logText),
     (logText.slice(-200) || srvOut.slice(-200)).replace(/\n/g, ' | '));

  try { ws.close(); } catch (e) {}
  mock.close();
  console.log(fail === 0 ? 'e2e:shutdown_notice PASS (' + pass + ')'
                         : 'e2e:shutdown_notice FAIL (' + fail + ')');
  process.exit(fail === 0 ? 0 : 1);
})().catch((e) => { console.log('DRIVER_ERR ' + e); process.exit(2); });
