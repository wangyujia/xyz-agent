#!/usr/bin/env python3
"""metrics 端点 e2e（v0.53.35）——起临时服务，curl /metrics 断言
Prometheus 文本格式与关键指标；含鉴权模式（401/带 token 200）。"""
import socket, subprocess, time, sys, os, random, urllib.request
urllib.request.install_opener(urllib.request.build_opener(urllib.request.ProxyHandler({})))  # 禁代理

BIN = '/root/code/thin_agent/build/thin_agent'
env = dict(os.environ); env['THIN_AGENT_DEV_MODE'] = '1'
random.seed()  # 端口随机化——上一轮残留服务占固定端口会让新服务静默起不来

def run_case(port, token=None, send_token=True):
    args = [BIN, '--port', str(port), '--config', 'config/demo.model.yaml',
            '--profile', 'offline_demo', '--dev']
    if token: args += ['--auth-token', token]
    svc = subprocess.Popen(args, env=env, cwd='/root/code/thin_agent',
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2.5)
    # send_token：请求是否带 Authorization（与服务端 token 分离——
    # 「服务鉴权 + 请求无凭据」是独立场景）
    try:
        req = urllib.request.Request(f'http://127.0.0.1:{port}/metrics')
        if token and send_token:
            req.add_header('Authorization', f'Bearer {token}')
        text = urllib.request.urlopen(req, timeout=3).read().decode()
        return text
    except urllib.error.HTTPError as e:
        return f'HTTP_{e.code}'
    finally:
        svc.terminate()

failed = 0
def check(name, ok):
    global failed
    print(('PASS: ' if ok else 'FAIL: ') + name)
    if not ok: failed += 1

# 无鉴权：格式与指标
# v0.54.2 (R89): **不要用随机区间**。此前 `random.randint(19100,19399)` 覆盖了别的 e2e 的
# 固定端口（如 unit_agent_api_shutdown 的 19101）——并行跑时撞上就 bind 失败 → HTTP 拒绝，
# 表现为本 e2e 偶发红（R83"测试资源冲突"家族的漏网形态：判官只扫字面端口，看不见随机区间）。
# 现在：向 OS 申请临时端口（bind(0)），落在 ephemeral 段（32768+），与任何测试固定端口互斥。
def free_port():
    sk = socket.socket()
    sk.bind(("127.0.0.1", 0))
    p = sk.getsockname()[1]
    sk.close()
    return p


port1 = free_port()
m = run_case(port1)
check('关键指标 uptime', 'thin_agent_uptime_seconds' in m and '# TYPE thin_agent_uptime_seconds gauge' in m)
check('关键指标 api_calls counter', '# TYPE thin_agent_api_calls_total counter' in m)
check('关键指标 version_info 带标签', 'thin_agent_version_info{version=' in m)
check('无 0.000000（整数直出）', '0.000000' not in m)
check('sessions_active 存在', 'thin_agent_sessions_active' in m)
# TYPE 无样本 = 断格式律
lines = m.splitlines()
type_names = [l.split()[2] for l in lines if l.startswith('# TYPE ')]
sample_names = {l.split('{')[0].split()[0] for l in lines if l and not l.startswith('#')}
check('每个 TYPE 都有样本行', all(t.split()[0] in sample_names for t in type_names))

# 鉴权：401 / 带 token 200
m2 = run_case(free_port(), token='mts456', send_token=False)
check('鉴权下无 token → 401', m2 == 'HTTP_401')
m3 = run_case(free_port(), token='mts456')
check('鉴权下带 token → 指标', 'thin_agent_uptime_seconds' in m3)

print('ALL PASS' if not failed else f'{failed} FAILED')
sys.exit(1 if failed else 0)
