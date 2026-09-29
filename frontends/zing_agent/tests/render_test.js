// zing_agent: renderMessages 行为级测试（v12——流式行/thinking 点存活于重绘）
// vm 真执行：模拟流进行中 pushMsgTo 触发全量重绘，断言流式行仍在 DOM
// 尾部且消息无双倍渲染。
const fs = require('fs');
const vm = require('vm');
let failed = 0;
const check = (ok, name) => { console.log((ok ? 'PASS: ' : 'FAIL: ') + name); if (!ok) failed++; };

const js = fs.readFileSync(__dirname + '/../web/chat.html', 'utf8').match(/<script>([\s\S]*)<\/script>/)[1];

// ── DOM 桩（够用即可：appendChild/innerHTML/scrollTop）──
function makeDom() {
  const children = [];
  return {
    children,
    get innerHTML() { return ''; },
    set innerHTML(v) { children.length = 0; },   // 模拟清空
    appendChild(el) { children.push(el); return el; },
    scrollTop: 0, scrollHeight: 100,
  };
}
const box = makeDom();
// 元素桩
function el(cls) { return { className: cls, removed: false, remove() { this.removed = true; } }; }

const streamRow = el('msg assistant streaming');
const sessions = [{ id: 'a', name: '新对话', chat_id: 'c1' }];
const ctx = vm.createContext({
  sessions, activeId: 'a',
  streamingEl: { row: streamRow, bubble: { textContent: '' }, text: '部分回复', cmdId: 'zing-1', sid: 'a' },
  typingEl: null,
  // renderMessages 依赖链
  loadMsgs: () => [{ kind: 'user', text: 'hi' }, { kind: 'assistant', text: 'ok' }],
  buildMsgEl: (m) => el('msg ' + m.kind),
  activeSession: () => sessions[0],
  $: () => box,
});
const fn = js.substring(js.indexOf('function renderMessages'), js.indexOf('function buildMsgEl'));
vm.runInContext(fn, ctx);
vm.runInContext('renderMessages()', ctx);
const kids = box.children;
check(kids.length === 3, `消息2条+流式行1=3（实际 ${kids.length}——双倍渲染回归）`);
check(kids[2] === streamRow, '流式行在尾部存活（重绘不再吃掉）');
check(!streamRow.removed, '流式行未被 remove');

// ── thinking 点存活 ──
box.children.length = 0;
vm.runInContext('typingEl = { className: "typing-row", removed: false, remove(){ this.removed = true; } }', ctx);
vm.runInContext('renderMessages()', ctx);
check(box.children.some(c => c.className === 'typing-row'), 'thinking 点存活于重绘');
check(!vm.runInContext('typingEl.removed', ctx), 'thinking 点未被 remove');

// ── 空会话+流式中：不显示空提示（应有流式行）──
const ctx3 = vm.createContext({
  sessions, activeId: 'a',
  streamingEl: { row: el('stream'), bubble: {}, text: 'x', cmdId: 'z', sid: 'a' },
  typingEl: null,
  loadMsgs: () => [],
  buildMsgEl: (m) => el('msg'),
  activeSession: () => sessions[0],
  $: () => box,
});
vm.runInContext(fn, ctx3);
box.children.length = 0;
vm.runInContext('renderMessages()', ctx3);
check(box.children.length === 1, `空历史+流式中=仅流式行（实际 ${box.children.length}）`);

// ── H6/H9: 断线/手动断开的流式收尾（vm 行为级）──
{
  const pushed = [];
  const box2 = { children: [], set innerHTML(v) { this.children.length = 0; }, appendChild(e) { this.children.push(e); return e; }, scrollTop: 0, scrollHeight: 100 };
  const deadRow = { removed: false, remove() { this.removed = true; } };
  let reconnectScheduled = false;
  const ctx2 = vm.createContext({
    streamingEl: { row: deadRow, bubble: {}, text: '流式中断前积累', cmdId: 'z1', sid: 'a' },
    typingEl: { remove() {} },
    sessions: [{ id: 'a', name: 'x', chat_id: 'c1' }], activeId: 'a',
    hideTyping: function () { this.typingElGone = true; },
    pushMsgTo: (sid, kind, text) => pushed.push([sid, kind, text]),
    setConn: () => {}, scheduleReconnect: () => { reconnectScheduled = true; },
    renderMessages: () => {},
    $: () => box2,
  });
  // 提取 onclose 闭包体：直接构造等价调用——从源码提取 onclose 赋值段
  const m = js.match(/ws\.onclose = \(\) => \{([\s\S]*?)\n  \};/);
  check(!!m, '源码含 onclose 闭包（可提取）');
  if (m) {
    vm.runInContext(m[1], ctx2);
    check(deadRow.removed, '断线时流式 row 已移除');
    check(pushed.length === 1 && pushed[0][1] === 'assistant' && String(pushed[0][2]).includes('连接中断'),
      '断线时半截文本落库+中断标注');
    check(reconnectScheduled, '断线后仍调度重连');
  }
}

console.log(failed ? 'FAILED' : 'ALL PASS');
process.exit(failed ? 1 : 0);
