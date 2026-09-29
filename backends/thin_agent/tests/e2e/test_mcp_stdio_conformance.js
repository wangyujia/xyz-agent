// e2e_mcp_stdio_conformance（v0.53.92）
//
// 启动真 thin_agent_mcp_server，按脚本走一遍 stdio JSON-RPC 会话，逐行校验协议：
//   C1 **每一行 stdout 都必须是合法非空 JSON**（空行/日志混入 = 协议污染，严格客户端
//      会报 invalid JSON 甚至断开）——这是本条 e2e 的核心守卫
//   C2 notification（无 id）**不得有响应**：notifications/initialized、ping(通知)
//   C3 **字符串 id 必须原样回显**（修复前 `int id = req.value("id",0)` 会抛 type_error
//      → 客户端拿到 -32603 + id=null，会话彻底错位）
//   C4 parse error → code -32700 且 **id 必须是 null**（修复前写 0，与 catch 分支自相矛盾）
//   C5 未知方法 → -32601 + id 回显；tools/call 未知工具 → -32602（协议错误 + id 回显）
//   C6 initialize 结果含 protocolVersion / capabilities.tools / serverInfo
//   C7 stdin 关闭后进程干净退出（exit 0）
//   C8（v0.54.0, R87）**通知形式的 tools/list / tools/call 不得应答**——R80 立的律此前
//      只覆盖 ping/initialized/resources/list/未知方法，tools/* 漏了：通知会回一帧
//      `{"id":null,...}`（严格客户端会话错位）。判据：全流程只有 parse error 那一帧
//      id 为 null。
//   C9 通知形式的 tools/call **不执行**（stderr 必须出现 WARN——分支在 dispatch 之前返回）
//   C10 resources/read → **-32002 Resource not found**（修复前 -32601 Method not found，
//      与已声明的 resources 能力自相矛盾）
//   C11 initialize 声明的 capabilities 与实现的路由面一致（含 resources）
const { spawn } = require('child_process');

const BIN = process.env.MCP_BIN || 'build/thin_agent_mcp_server';
const CONFIG = process.env.MCP_CONFIG || 'config/chat_policy.json';
const SEP = '<<<';
const NL = '\n';

let pass = 0, fail = 0;
function ok(name, cond, extra) {
  if (cond) { pass++; console.log('  PASS: ' + name); }
  else { fail++; console.log('  FAIL: ' + name + (extra ? ' — ' + extra : '')); }
}

const child = spawn(BIN, ['--config', CONFIG], { stdio: ['pipe', 'pipe', 'pipe'] });
let out = '', errOut = '';
child.stdout.on('data', (d) => { out += d.toString(); });
child.stderr.on('data', (d) => { errOut += d.toString(); });

// 注意：刻意用"包含空行的发送脚本"（连续两个换行）验证服务端不把空行当请求、
// 也不会因此多写一行输出
const script = [
  JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'initialize',
    params: { protocolVersion: '2024-11-05', capabilities: {}, clientInfo: { name: 'conf', version: '1' } } }),
  JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }),      // 通知 → 不应答
  JSON.stringify({ jsonrpc: '2.0', id: 'abc', method: 'tools/list' }),          // 字符串 id
  'not a json at all',                                                          // parse error
  JSON.stringify({ jsonrpc: '2.0', id: 3, method: 'no/such/method' }),          // -32601
  JSON.stringify({ jsonrpc: '2.0', method: 'ping' }),                           // 通知 → 不应答
  JSON.stringify({ jsonrpc: '2.0', id: 4, method: 'tools/call',
    params: { name: '__definitely_not_a_tool__', arguments: {} } }),            // -32602
  JSON.stringify({ jsonrpc: '2.0', method: 'tools/list' }),                     // 通知 → 不应答
  JSON.stringify({ jsonrpc: '2.0', method: 'tools/call',                        // 通知 → 不应答且不执行
    params: { name: 'bb_write', arguments: { key: 'mcp_notif_probe', value: 'x' } } }),
  JSON.stringify({ jsonrpc: '2.0', id: 5, method: 'resources/read',
    params: { uri: 'thin://does-not-exist' } }),                                // -32002
  '',                                                                           // 空行 → 应被忽略且不产生输出
].join(NL) + NL;

child.stdin.write(script);
child.stdin.end();

child.on('close', (code) => {
  const rawLines = out.split(NL);
  const lines = rawLines.filter((l) => l !== '');        // 去掉尾部/中间空行后逐行解析
  const blankLines = rawLines.slice(0, rawLines.length - 1).filter((l) => l === '').length;

  ok('C1 stdout 无空行（协议污染守卫）', blankLines === 0, 'blank=' + blankLines);
  let allJson = true, parsed = [];
  for (const l of lines) { try { parsed.push(JSON.parse(l)); } catch (e) { allJson = false; } }
  ok('C1 每一行都是合法 JSON', allJson, 'lines=' + lines.length);
  ok('C2+C3 响应条数=6（四条通知无响应）', parsed.length === 6, 'got=' + parsed.length);
  // v0.54.0 (R87): 通知不得应答 → 除 parse error 外**没有** id===null 的帧
  const nullIdFrames = parsed.filter((p) => p.id === null);
  ok('C8 通知未产生 id:null 帧（仅 parse error 一帧）', nullIdFrames.length === 1,
     'nullId=' + nullIdFrames.length);

  const byId = (v) => parsed.find((p) => p.id === v);
  const init = byId(1);
  ok('C6 initialize 回显数字 id', !!init, 'id=1 缺失');
  ok('C6 result 含 protocolVersion', !!(init && init.result && init.result.protocolVersion));
  ok('C6 capabilities.tools 存在', !!(init && init.result && init.result.capabilities && init.result.capabilities.tools));
  ok('C6 serverInfo 存在', !!(init && init.result && init.result.serverInfo && init.result.serverInfo.name));

  const strId = byId('abc');
  ok('C3 字符串 id 原样回显（未退化成 0）', !!strId, 'id="abc" 缺失');
  ok('C3 字符串 id 结果正常（tools 数组）', !!(strId && strId.result && Array.isArray(strId.result.tools)));

  const pe = parsed.find((p) => p.error && p.error.code === -32700);
  ok('C4 parse error 存在 (-32700)', !!pe);
  ok('C4 parse error 的 id 是 null（不是 0）', !!pe && pe.id === null, pe ? 'id=' + JSON.stringify(pe.id) : '');

  const nf = byId(3);
  ok('C5 未知方法 -32601 + id 回显', !!(nf && nf.error && nf.error.code === -32601));
  const badTool = byId(4);
  ok('C5 tools/call 未知工具 -32602 + id 回显', !!(badTool && badTool.error && badTool.error.code === -32602));

  const rr = byId(5);
  ok('C10 resources/read → -32002 Resource not found（非 -32601）',
     !!(rr && rr.error && rr.error.code === -32002), rr ? 'err=' + JSON.stringify(rr.error && rr.error.code) : 'missing');
  ok('C11 capabilities.resources 已声明（与实现的路由面一致）',
     !!(init && init.result && init.result.capabilities && init.result.capabilities.resources));
  ok('C9 通知形式的 tools/call 未执行（WARN 可观测）',
     errOut.includes('tools/call as notification'), 'stderr 无 WARN');
  ok('C7 进程干净退出 (exit 0)', code === 0, 'exit=' + code);
  ok('C7 stderr 不污染 stdout（诊断信息走 stderr）', !out.includes('thin_agent_mcp_server:'));

  console.log(fail === 0 ? 'e2e:mcp_stdio_conformance PASS (' + pass + ')'
                         : 'e2e:mcp_stdio_conformance FAIL (' + fail + ')');
  if (fail !== 0) console.log('--- stdout ---' + NL + out + NL + '--- stderr ---' + NL + errOut);
  process.exit(fail === 0 ? 0 : 1);
});
