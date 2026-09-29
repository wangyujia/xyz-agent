// zing_agent: 离线队列行为级测试（v15——断线输入不丢）
// 函数级提取 + vm 真执行（同 render_test 模式）：断线发送→排队；重连→自动重发；20 上限。
const fs = require('fs');
const vm = require('vm');
let failed = 0;
const check = (name, ok) => { console.log((ok ? 'PASS: ' : 'FAIL: ') + name); if (!ok) failed++; };

const js = fs.readFileSync(__dirname + '/../web/chat.html', 'utf8').match(/<script>([\s\S]*)<\/script>/)[1];

// ── 提取待测函数段 ──
const segStart = js.indexOf('const pendingQueue');
const segEnd = js.indexOf('function quickCmd');
const seg = js.substring(segStart, segEnd);  // queueChat + flushPendingQueue

const sentFrames = [];
const sysLines = [];
const chatBox = { children: [], appendChild(c) { chatBox.children.push(c); return c; } };
const fakeWs = { readyState: 0, send(d) { sentFrames.push(d); } };
const ctx = vm.createContext({
  pendingQueue: [],
  pushMsg: (kind, text) => sysLines.push(kind + '|' + text),
  sendRaw: (payload) => { if (ctx.ws && ctx.ws.readyState === 1) { ctx.ws.send(payload); return true; } return false; },
  scheduleReconnect: () => {},
  setTimeout: (fn) => fn(),  // 同步化
  setTimeoutReal: setTimeout,
  ws: null,
});
vm.runInContext(seg, ctx);

// ── 场景1:断线排队 ──
ctx.ws = null;
vm.runInContext('queueChat("排队消息A")', ctx);
check('断线发送→排队', vm.runInContext('pendingQueue.length', ctx) === 1);
check('排队提示可见', sysLines.some(l => l.includes('排队')));

// ── 场景2:重连 flush 重发 ──
fakeWs.readyState = 1; ctx.ws = fakeWs;
vm.runInContext('flushPendingQueue()', ctx);
check('重连 flush 后队空', vm.runInContext('pendingQueue.length', ctx) === 0);
check('重发帧已上 WS', sentFrames.length === 1 && sentFrames[0].text === '排队消息A');

// ── 场景3:flush 中途又断 → 回队首 ──
sentFrames.length = 0;
vm.runInContext('pendingQueue.push("m1","m2")', ctx);
fakeWs.readyState = 0;
vm.runInContext('flushPendingQueue()', ctx);
check('flush 中断→回队首', vm.runInContext('pendingQueue.length', ctx) === 2);

// ── 场景4:20 条上限 ──
vm.runInContext('pendingQueue.length = 0', ctx);
ctx.ws = null;
vm.runInContext('for (let i = 0; i < 25; i++) queueChat("x" + i)', ctx);
check('排队上限 20', vm.runInContext('pendingQueue.length', ctx) === 20);
check('超限有提示', sysLines.some(l => l.includes('排队已满')));

console.log(failed ? 'FAILED' : 'ALL PASS');
process.exit(failed ? 1 : 0);
