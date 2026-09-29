// unit_frontend_inflight_cleanup（v0.53.88）
//
// 守卫对象：ws_agent.html 的**断线收尾**——客户端的在途请求状态（思考占位气泡
// _thinkingMap / 流式气泡 window._streamMap）必须在连接关闭时被收尾，否则：
//  ①占位气泡要等各自 60s 定时器才变"超时"（连接早断了，用户看 60 秒假"思考中"）
//  ②流式气泡永久停在 streaming 态（zing webview 踩过的同款：断线须收尾流式态）
// 本测试是**源码面扫描**（纯文本断言）：行为无法在无 DOM 环境测，但"调用点被删掉"
// 是这类回归的唯一形态，扫得到。
const fs = require('fs');
const path = require('path');
const HTML = process.env.HTML_PATH ||
  path.join(__dirname, '..', '..', 'ws_agent.html');

let pass = 0, fail = 0;
function ok(name, cond) {
  if (cond) { pass++; console.log('  PASS: ' + name); }
  else { fail++; console.log('  FAIL: ' + name); }
}
function bodyOf(src, marker) {          // 取以 marker 开头、花括号配平的一段
  const i = src.indexOf(marker);
  if (i < 0) return '';
  let d = 0, j = src.indexOf('{', i);
  const start = j;
  for (; j < src.length; j++) {
    if (src[j] === '{') d++;
    else if (src[j] === '}') { d--; if (d === 0) break; }
  }
  return src.slice(start, j + 1);
}

const src = fs.readFileSync(HTML, 'utf8');
console.log('守卫：' + HTML);

ok('定义了 finalizeInflightOnDisconnect', src.includes('function finalizeInflightOnDisconnect('));

const closer = bodyOf(src, 'function finalizeInflightOnDisconnect(');
ok('收尾思考占位气泡(_thinkingMap)', closer.includes('_thinkingMap'));
ok('收尾流式气泡(window._streamMap)', closer.includes('_streamMap'));
ok('清掉 60s 占位定时器(clearTimeout)', closer.includes('clearTimeout'));
ok('清空 map 条目(delete)', closer.includes('delete'));

const onclose = bodyOf(src, 'ws.onclose = ');
ok('onclose 收尾在途', onclose.includes('finalizeInflightOnDisconnect('));
const onerror = bodyOf(src, 'ws.onerror = ');
ok('onerror 收尾在途（部分环境 error 不伴随 close）', onerror.includes('finalizeInflightOnDisconnect('));
const disc = bodyOf(src, 'function disconnectWs(');
ok('主动断开 disconnectWs 也收尾', disc.includes('finalizeInflightOnDisconnect('));
ok('主动断开与断线文案区分(user/closed)', src.includes("'user'") && src.includes("'closed'"));

console.log(fail === 0 ? `unit:frontend_inflight_cleanup PASS (${pass})`
                       : `unit:frontend_inflight_cleanup FAIL (${fail})`);
process.exit(fail === 0 ? 0 : 1);
