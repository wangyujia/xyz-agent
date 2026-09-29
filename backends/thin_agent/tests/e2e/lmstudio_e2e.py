#!/usr/bin/env python3
"""LM Studio 链路 e2e：本机 1234 起 OpenAI 兼容 mock → thin_agent(lmstudio
provider) → ws chat → 断言回复内容。验完自动清理。"""
import json, subprocess, threading, time, sys, os, signal
from http.server import HTTPServer, BaseHTTPRequestHandler

REQ = {'stream': False}
class H(BaseHTTPRequestHandler):
    def do_POST(self):
        ln = int(self.headers.get('Content-Length', 0))
        try: REQ['stream'] = json.loads(self.rfile.read(ln)).get('stream', False)
        except Exception: pass
        # LM Studio 保真：stream=true 时回 SSE（FC 恒以流式请求——非流式
        # JSON 会被 parse_sse_stream 判空→云失败→offline fallback）
        if REQ['stream']:
            chunks = [
                {'choices':[{'delta':{'content':'本地'}}]},
                {'choices':[{'delta':{'content':'模型回复OK'}}]},
                {'choices':[{'delta':{},'finish_reason':'stop'}]},
                {'choices':[{'delta':{}}],'usage':{'prompt_tokens':1,'completion_tokens':2,'total_tokens':3}},
            ]
            body = ''.join('data: ' + json.dumps(c) + '\n\n' for c in chunks) + 'data: [DONE]\n\n'
            self.send_response(200)
            self.send_header('Content-Type','text/event-stream')
            self.end_headers()
            self.wfile.write(body.encode())
        else:
            body = json.dumps({'choices':[{'message':{'content':'本地模型回复OK','finish_reason':'stop'}}]})
            self.send_response(200)
            self.send_header('Content-Type','application/json')
            self.end_headers()
            self.wfile.write(body.encode())
    def log_message(self, *a, **kw): pass

srv = HTTPServer(('127.0.0.1', 1234), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()

open('/tmp/lmstudio_test.yaml','w').write('model:\n  mode: cloud\n  provider: lmstudio\n  name: test-model\n')

env = dict(os.environ); env['THIN_AGENT_DEV_MODE']='1'
svc = subprocess.Popen(['./build/thin_agent','--port','18790','--config','config/demo.model.yaml',
                        '--profile','lmstudio_demo','--dev'], env=env,
                       stdout=open('/tmp/lms_svc.log','w'), stderr=subprocess.STDOUT, cwd='/root/code/thin_agent')
try:
    time.sleep(3)
    r = subprocess.run([os.environ.get('NODE','/root/.hermes/node/bin/node'),'-e','''
const WebSocket = require('/usr/share/nodejs/ws');
const ws = new WebSocket('ws://127.0.0.1:18790/ws');
let got = '';
ws.on('open', () => ws.send(JSON.stringify({type:'chat', text:'你好', chat_id:'lms-1', cmd_id:'lms-1', trace_id:'lms-1'})));
ws.on('message', (d) => { try { const o=JSON.parse(d); if(o.text) got=(o.text||'').slice(0,30); } catch(e){} });
setTimeout(()=>{ console.log(got.includes('本地模型回复OK') ? 'PASS: lmstudio 链路端到端（mock 1234→provider→ws 回复）' : 'FAIL: '+got); ws.close(); process.exit(0); }, 80000);
'''], capture_output=True, text=True, timeout=100)
    print(r.stdout.strip() or r.stderr.strip()[:200])
    sys.exit(0 if 'PASS' in r.stdout else 1)
finally:
    svc.kill(); srv.shutdown()
