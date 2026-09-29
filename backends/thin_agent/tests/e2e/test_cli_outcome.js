// e2e_cli_outcome（v0.53.94）
//
// CLI 一次性模式的"终局契约"守卫。修复前的行为（实测）：
//   ① **没有任何回复检测**：收到答案后仍空转到 120s 上限 → `WALL=122s`（用户以为卡死）；
//   ② **退出码恒 0**：超时/断线/成功都返回 0 → 脚本/CI 无法判成败（静默失败）；
//   ③ 超时/断线不可区分；json 模式下超时 stdout **全空**（JSON 消费者拿到零输出）。
//
// 契约（本测试锁定）：
//   S1 服务端回复     → 立即退出（<10s，不再等满上限）、exit 0、stdout 含答案
//   S2 服务端不回     → `-t 2` 后 exit 2（timeout），stdout 出**结构化** cli_error
//   S3 服务端断开     → exit 3（disconnected），stdout 出结构化 cli_error
const { spawn } = require('child_process');
const WebSocket = require('/usr/share/nodejs/ws');
const REPO = process.env.REPO || '.';
const CLI = process.env.CLI_BIN || './build/thin_agent_cli';

let pass = 0, fail = 0;
function ok(name, cond, extra) {
  if (cond) { pass++; console.log('  PASS: ' + name); }
  else { fail++; console.log('  FAIL: ' + name + (extra ? ' — ' + extra : '')); }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function startFake(port, mode) {
  return new Promise((res) => {
    const wss = new WebSocket.Server({ port, host: '127.0.0.1' }, () => res(wss));
    wss.on('connection', (ws) => {
      ws.send(JSON.stringify({ type: 'hello', agent: 'fake', version: 'v0' }));
      ws.on('message', () => {
        if (mode === 'reply') {
          ws.send(JSON.stringify({ type: 'chat_result', text: 'ANSWER-OK' }));
          ws.send(JSON.stringify({ type: 'chat_chunk', chunk: '', done: true }));
        } else if (mode === 'close') {
          ws.close();
        } // silent: 不回
      });
    });
  });
}

// 必须用**异步 spawn**：spawnSync 会阻塞 node 事件循环，同进程内的假 WS 服务端
// 就无法接受连接（本轮实测踩到——三条用例全报 "Failed to connect to agent."）。
function runCli(url, timeoutSec) {
  return new Promise((res) => {
    const t0 = Date.now();
    const c = spawn(CLI, ['--connect', url, '-q', 'hi', '-j', '-t', String(timeoutSec)],
                    { cwd: REPO });
    let out = '';
    c.stdout.on('data', (d) => { out += d.toString(); });
    c.stderr.on('data', (d) => { out += d.toString(); });
    c.on('close', (code) => res({ code, out, ms: Date.now() - t0 }));
  });
}

(async () => {
  const P1 = 18821, P2 = 18822, P3 = 18823;
  const s1 = await startFake(P1, 'reply');
  const s2 = await startFake(P2, 'silent');
  const s3 = await startFake(P3, 'close');
  await sleep(300);

  // S1 正常回复：必须"立即"退出（修复前 122s）
  const r1 = await runCli('ws://127.0.0.1:' + P1 + '/ws', 120);
  ok('S1 回复后立即退出（不再空转到上限，<10s）', r1.ms < 10000, 'wall=' + r1.ms + 'ms');
  ok('S1 退出码 0（成功）', r1.code === 0, 'exit=' + r1.code);
  ok('S1 stdout 含答案', /ANSWER-OK/.test(r1.out), JSON.stringify(r1.out.slice(-120)));

  // S2 服务端不回：超时 → exit 2 + 结构化错误
  const r2 = await runCli('ws://127.0.0.1:' + P2 + '/ws', 2);
  ok('S2 超时退出码 2（可被脚本区分）', r2.code === 2, 'exit=' + r2.code);
  ok('S2 未等满 120s（-t 生效）', r2.ms < 15000, 'wall=' + r2.ms + 'ms');
  const m2 = r2.out.match(/\{"elapsed_ms":\d+,"reason":"timeout","type":"cli_error"\}/);
  ok('S2 json 模式给出**结构化** cli_error(timeout)', !!m2, JSON.stringify(r2.out.slice(-160)));

  // S3 服务端断开：exit 3 + 结构化错误
  const r3 = await runCli('ws://127.0.0.1:' + P3 + '/ws', 30);
  ok('S3 断线退出码 3（与超时区分）', r3.code === 3, 'exit=' + r3.code);
  ok('S3 未等满上限（断线立即收尾）', r3.ms < 15000, 'wall=' + r3.ms + 'ms');
  ok('S3 json 模式给出结构化 cli_error(disconnected)',
     /"reason":"disconnected"/.test(r3.out), JSON.stringify(r3.out.slice(-160)));

  s1.close(); s2.close(); s3.close();
  console.log(fail === 0 ? 'e2e:cli_outcome PASS (' + pass + ')'
                         : 'e2e:cli_outcome FAIL (' + fail + ')');
  process.exit(fail === 0 ? 0 : 1);
})().catch((e) => { console.log('DRIVER_ERR ' + e); process.exit(2); });
