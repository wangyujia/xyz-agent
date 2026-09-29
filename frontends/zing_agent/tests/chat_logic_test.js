// zing_agent: chat.html JS 纯函数单测（node 提取法——同 ws_agent renderLiveStats 验证模式）
// 覆盖：pushMsgTo 会话定向（②跨会话不串写）/ _sid 审批路由（③）/
// localStorage 容错 / sendRaw sid 定向 / mdToHtml 转义（XSS 面）。
// node 无 DOM——提取纯逻辑函数以 jsdom-lite 手工桩验证。
const fs = require('fs');
const path = require('path');
const html = fs.readFileSync(path.join(__dirname, '..', 'web', 'chat.html'), 'utf8');
const m = html.match(/<script>([\s\S]*)<\/script>/);
if (!m) { console.log('FAIL: 无 script 块'); process.exit(1); }
let js = m[1];

// ── 提取目标纯函数/状态（regex 切块——依赖 DOM 的函数提供桩）──
function extractFn(name) {
  const re = new RegExp('(?:function ' + name + '\\(|const ' + name + ' = )');
  return re.test(js);
}

let failed = 0;
function check(ok, name) {
  console.log((ok ? 'PASS: ' : 'FAIL: ') + name);
  if (!ok) failed++;
}

// 1) 关键函数存在性（二三轮修复的结构锚）
check(extractFn('pushMsgTo'), 'pushMsgTo 存在（②会话定向修复）');
check(extractFn('sendRaw'), 'sendRaw 存在');
check(/sendRaw\(payload, sid\)/.test(js), 'sendRaw 双参签名（③审批 sid 定向）');
check(/streamingEl = \{ row, bubble[\s\S]*?sid: activeId \}/.test(js.replace(/'/g, "'")),
      'streamingEl 记录 sid（②流式跨会话修复）');
check(/const asid = m\._sid \|\| activeId/.test(js), '审批卡绑定 m._sid（③跨会话路由修复）');
check(/kvSet\('zing_active'/.test(js), 'switchSession 持久化 zing_active（KV 桥）');

// 2) mdToHtml XSS 面：危险输入不产裸标签（提取函数跑真值）
{
  const fnMatch = js.match(/function mdToHtml\([^)]*\) \{[\s\S]*?\n\}/);
  const escMatch = js.match(/function esc\([^)]*\) \{[\s\S]*?\n\}/);
  if (!fnMatch || !escMatch) { check(false, 'mdToHtml/esc 可提取'); }
  else {
    const ev = new Function(escMatch[0] + '\n' + fnMatch[0] + '; return mdToHtml;')();
    const out1 = ev('<script>alert(1)<\/script>');
    check(!/<script>/i.test(out1), 'mdToHtml 转义 <script>（XSS）');
    const out2 = ev('**bold** and `code`');
    check(out2.includes('<strong>') || out2.includes('<em>') || out2.includes('<code>') || out2 === '**bold** and `code`',
          'mdToHtml 正常 markdown 输出（不炸）');
  }
}

// 3) 心跳逻辑结构（三轮③）
check(/setInterval\(\(\) => \{[\s\S]*?type: 'ping'/.test(js) || /type: 'ping'/.test(js),
      '30s 心跳 ping 帧存在');
check(/lastFrameTs > 60000/.test(js), '60s 判死触发重连存在');

// 4) 重连退避（三轮确认项——防退化）
check(/Math\.min\(1000 \* Math\.pow\(2, reconnectAttempts\+\+\), 30000\)/.test(js),
      '指数退避 1s→30s 上限');

// 5) 会话消息配额（200 条 cap）
check(/slice\(-200\)/.test(js), '本地消息 200 条 cap');

// 6) 主题三态（v6）
check(/data-theme="dark"/.test(js.replace(/'/g,'"')) || /\[data-theme=\"dark\"\]/.test(html), '深色 [data-theme="dark"] 定义存在');
check(/--side-bg: linear-gradient/.test(html) && /--side-bg: linear-gradient/.test(html.split('[data-theme="dark"]')[1] || ''), '侧栏/深色两套 --side-bg token');
check(/function applyTheme|const applyTheme|applyTheme =/.test(js) && /THEMES = \['system', 'light', 'dark'\]/.test(js), 'applyTheme+三态定义');
check(/zing_theme/.test(js), '主题 localStorage 记忆');
check(/prefers-color-scheme/.test(js) && /addEventListener\('change'/.test(js), '系统主题实时跟随');
check(/themeBtn/.test(js) && /cycleTheme/.test(js), '切换按钮+循环切换');
// 无遗留硬编码颜色（CSS 规则体内——:root/[data-theme] 定义区之外）
{
  const css = html.split('<style>')[1].split('</style>')[0];
  const stripped = css.replace(/:root \{[\s\S]*?\n    \}/, '').replace(/\[data-theme="dark"\] \{[\s\S]*?\n    \}/, '');
  const hard = stripped.match(/#[0-9a-fA-F]{6}\b/g) || [];
  // 例外：代码块深底（pre/tool-detail 双主题通用深底）+user avatar 渐变
  const allowed = hard.filter(c => !['#0f172a', '#dbe7ff', '#c9d8f0', '#5b8a3c', '#4a7a30'].includes(c.toLowerCase()));
  // v14: rgba() 半透明也纳入守护——白色系叠加深蓝侧栏两主题通用 ✔,
  // 但未来非通用色（非白）必须 token 化
  // v14: rgba 守护——白色系(叠深蓝侧栏)+box-shadow 行(投影/光晕,低危视觉债)
  // 放行;其余(背景/边框/文字色)必须 token 化
  const nonShadowRgba = stripped.split('\n').filter(l =>
    /rgba\(\s*(?!255,\s*255,\s*255)[\d]+,\s*[\d]+,\s*[\d]+/.test(l) && !/box-shadow/.test(l));
  check(allowed.length === 0, 'CSS 规则体无硬编码颜色（token 全覆盖，例外=代码块深底/avatar 渐变）' + (allowed.length ? ' 残留:' + allowed.join(',') : ''));
check(nonShadowRgba.length === 0, '非阴影 rgba() 必须走 token（白色系/box-shadow 放行）' + (nonShadowRgba.length ? ' 残留:' + nonShadowRgba.join(' | ').slice(0, 100) : ''));
}

// 7) KV 持久层（v7：C++ ConfigStore 桥——webview 不用 localStorage）
check(typeof (js.match(/function kvSet\(k, v\)/)) !== 'object' || /function kvSet/.test(js), 'kvSet 存在');
check(/const KV_HAS_BRIDGE = \(typeof window\.cfgGetAll === 'function'\)/.test(js), '桥探测 KV_HAS_BRIDGE');
check(/typeof all === 'object' && all !== null/.test(js), 'cfgGetAll 返回对象兼容（webview 自动反序列化）');
check(!/localStorage\.setItem\('zing_/.test(js), '业务键不再写 localStorage（全部走 KV 桥）');
check(/kvPending\.push/.test(js), '预载前写请求排队回放');

// 8) v8 审计修复锚
check(/kvDel\('zing_msgs_' \+ id\);  \/\/ v8: 无条件清/.test(js), 'delSession 无条件清消息键（非活跃会话不泄漏）');
check(/streamingEl\.sid === activeId/.test(js), 'appendStream 会话归属渲染（跨会话不串流显示）');

// 9) v9 六轮审计修复锚
check(/ws\.onopen = \(\) => \{[\s\S]{0,200}lastFrameTs = Date\.now\(\)/.test(js), 'onopen 重置判死时钟（重连后首 tick 不误杀）');
check(/if \(streamingEl\) \{[\s\S]{0,250}prev\.row\.remove\(\)/.test(js), 'startStream 顶替旧流先落库再移除（孤儿流式行+文本丢失）');

// 10) v10 七轮锚
check(js.includes("pushMsg('system', '❌ ' + msg)"), 'error 帧用户可见（agent not ready/执行异常不再石沉大海）');
check(/sessions\.length > 50[\s\S]{0,150}kvDel\('zing_msgs_' \+ d\.id\)/.test(js), '会话 50 上限+淘汰清消息键（KV 无限增长防护）');

// 11) v10 error 帧行为级（vm 真执行 handleIncoming）
{
  const fs2 = require('fs'), vm2 = require('vm');
  const jsFull = fs2.readFileSync(__dirname + '/../web/chat.html', 'utf8').match(/<script>([\s\S]*)<\/script>/)[1];
  const fn = jsFull.substring(jsFull.indexOf('function handleIncoming'), jsFull.indexOf('/* ══════════════ 斜杠补全'));
  const pushed = [];
  const ctx = vm2.createContext({
    sessions: [{ id: 'a', name: 'x', chat_id: 'c1' }], activeId: 'a',
    hideTyping: () => {}, showTyping: () => {}, endStream: () => {}, appendStream: () => {},
    pushMsg: (kind, text) => pushed.push([kind, text]), pushMsgTo: () => {}, setConn: () => {},
    kvGet: () => null, kvSet: () => {}, kvDel: () => {}, streamingEl: null, String, JSON,
  });
  vm2.runInContext(fn, ctx);
  vm2.runInContext("handleIncoming({type:'error', message:'agent not ready'})", ctx);
  check(pushed.length === 1 && pushed[0][0] === 'system' && String(pushed[0][1]).includes('agent not ready'),
    'error 帧→用户可见 system 消息（vm 行为级）');
}

// 12) v11 八轮锚
check(js.includes("目标会话已不存在"), 'sendRaw 会话缺失防护（审批卡残留→批准失灵可见化）');

// 13) v15 C++ 宿主锚（discovery JSON 转义）
{
  const cpp = fs.readFileSync(__dirname + '/../src/zing_agent.cpp', 'utf8');
  check(/static std::string json_escape/.test(cpp) && cpp.includes('json_escape(entry->hostname)') && cpp.includes('json_escape(entry->ip)'),
    'discovery 网络串 JSON 转义（组播报文注入面）');
}

// 14) v16 KV 回放 del 路径行为级（vm）——kvDel 在预载前排队 [k,null]
{
  const vm3 = require('vm');
  const jsFull = fs.readFileSync(__dirname + '/../web/chat.html', 'utf8').match(/<script>([\s\S]*)<\/script>/)[1];
  const seg = jsFull.substring(jsFull.indexOf('const KV_HAS_BRIDGE'), jsFull.indexOf('/* ══════════════ 主题'));
  const calls = [];
  const ctx3 = vm3.createContext({
    window: { cfgGetAll: async () => ({}), cfgSet: (s) => calls.push(['set', s]), cfgDel: (s) => calls.push(['del', s]) },
    localStorage: { getItem: () => null, setItem: (k, v) => calls.push(['ls', k]), removeItem: (k) => calls.push(['lrm', k]) },
    console,
  });
  vm3.runInContext(seg + '\nkvDel("dead_session"); kvSet("a","1"); kvLoad();', ctx3);
  // 回放异步（kvLoad await cfgGetAll 后 flush）——延后断言
  setTimeout(() => {
    const types = calls.map(c => c[0]);
    check(types.indexOf('del') !== -1 && types.indexOf('del') < types.indexOf('set'),
      'kvPending 回放保序（del 先于后到的 set）');
    console.log(failed ? 'FAILED' : 'ALL PASS');
    process.exit(failed ? 1 : 0);
  }, 50);
}
// 注：断言收尾在上方 setTimeout 内（exit 由 timer 统一执行）
