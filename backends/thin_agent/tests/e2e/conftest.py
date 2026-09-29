# thin_agent e2e 驱动库
#
# 固化历轮验证踩过的坑（写在这里，防止每次重写驱动再踩一遍）：
# 1. chat_approve 的字段名是 `approved`（非 approve）
# 2. 审批语义是"单工具执行"——批准后响应是"已执行"，多步任务须发
#    "继续"驱动（drive_until_done）
# 3. 孤儿审批 TTL=5min：批准间隔过久会被惰性清扫，返回"没有待审批操作"
# 4. set_project 的 chat_id 必须与后续任务同会话（项目上下文是 per-session）
# 5. set_project 目标目录必须先存在
# 6. 沙箱路径映射：宿主 /tmp/x == 沙箱 /host_tmp/x（write_file 用宿主路径，
#    shell_exec 里引用 /tmp 文件须用 /host_tmp）
# 7. 带下划线的 chat_id（如 lang52b）会在 DB 里映射成 chat-N，断言别依赖
#    原始 id 字符串
# 8. 服务启动须等端口监听（hello 握手）后再发请求
"""e2e 公共驱动：WS 客户端封装 + approve-continue 循环 + 服务启停 fixture。"""
import json
import os
import shutil
import signal
import socket
import subprocess
import time

from websocket import create_connection

DEFAULT_WS = "ws://127.0.0.1:8765/ws"
REPO = os.environ.get("THIN_AGENT_REPO", "/root/code/thin_agent")


class AgentClient:
    """单连接 WS 客户端。所有 chat 共用一条连接（会话由 chat_id 区分）。"""

    def __init__(self, ws_url=DEFAULT_WS, timeout=15):
        self.ws = create_connection(ws_url, timeout=timeout)
        self.ws.recv()  # hello

    def close(self):
        try:
            self.ws.close()
        except Exception:
            pass

    def chat(self, text, chat_id, timeout=300):
        """发 chat 并等到 chat_result（发送失败自动重连一次）。"""
        try:
            self.ws.settimeout(timeout)
            self.ws.send(json.dumps({"type": "chat", "text": text,
                                     "chat_id": chat_id}))
        except Exception:
            self._reconnect()
            self.ws.settimeout(timeout)
            self.ws.send(json.dumps({"type": "chat", "text": text,
                                     "chat_id": chat_id}))
        while True:
            j = json.loads(self.ws.recv())
            if j.get("type") == "chat_result":
                return j

    def approve(self, chat_id, approved=True, timeout=200):
        """审批当前暂停的危险工具，等到 chat_result。"""
        self.ws.settimeout(timeout)
        self.ws.send(json.dumps({"type": "chat_approve",
                                 "approved": approved, "chat_id": chat_id}))
        while True:
            j = json.loads(self.ws.recv())
            if j.get("type") == "chat_result":
                return j

    def set_project(self, path, chat_id, mode="rw", timeout=20):
        """注意：服务端字段是 mode（ro/rw），非 access（实测踩坑：
        access 被忽略默认 ro → code_write_file 报 project_read_only）。"""
        self.ws.settimeout(timeout)
        self.ws.send(json.dumps({"type": "set_project", "path": path,
                                 "mode": mode, "chat_id": chat_id}))
        return json.loads(self.ws.recv())

    def drive_until_done(self, text, chat_id, max_rounds=10,
                         continue_msg="继续，直到给出最终结果",
                         timeout=240):
        """chat + approve-continue 循环（适配单工具审批语义）。

        返回 (最终 chat_result, 批准次数)。
        """
        j = self.chat(text, chat_id, timeout)
        approves = 0
        for _ in range(max_rounds):
            if "需要确认" not in j.get("text", ""):
                return j, approves
            j = self.approve(chat_id)
            approves += 1
            if "需要确认" not in j.get("text", ""):
                # 工具已执行但任务可能未完——驱动模型继续
                j = self.chat(continue_msg, chat_id, timeout)
        return j, approves


def wait_port(port=8765, timeout=15):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            return True
        except OSError:
            time.sleep(0.3)
    return False


def _port_occupant(port):
    """列出占用端口的进程（只读，不杀）——用于把冲突报出来给人处理。"""
    try:
        out = subprocess.run(["ss", "-ltnp"], capture_output=True, text=True).stdout
    except Exception:
        return "(ss 不可用)"
    for line in out.splitlines():
        if f":{port} " in line:
            return line.strip()
    return "(未在 ss 中定位到)"


def start_service(port=8765, log_path="/tmp/thin_e2e.log", profile="deepseek_main_demo"):
    """启动 thin_agent 服务。凭据文件 deepseek.env（DeepSeek 直连，模型 deepseek-flash），
    缺失时回退 zai.env（GLM 备份通道）。"""
    # ── v0.54.14 (R93 收官): 端口预检（**单点收口**，一处修全体 e2e 受益）──
    # 实测踩坑：`test_smoke_and_safety.py` 从不 stop_service ⇒ 每次运行都在 8765 留一个**活服务**
    # （实测残留 19 分钟后仍在跑）。此时 start_service 的新进程 `bind` 失败退出，而 `wait_port`
    # 对**旧服务**照样返回成功 ⇒ 测试静默与"别人的进程 / 旧 home / 旧二进制"对话——既是假绿之源，
    # 也让残留进程永不回收。现在：端口已被占用即**报错并打印占用者**，绝不静默连上去。
    # （本驱动不杀任何进程：清场由各用例自己的精确匹配 `_kill_stale` 或人工处理。）
    if wait_port(port, timeout=1):
        raise RuntimeError(
            "端口 %d 已被占用：%s\n本驱动不会替你杀进程——请先释放该端口"
            "（或用另一端口）。" % (port, _port_occupant(port)))
    env_file = "/root/.thin_agent/deepseek.env"
    if not os.path.exists(env_file):
        env_file = "/root/.thin_agent/zai.env"
    env = dict(os.environ)
    for line in open(env_file):
        line = line.strip()
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            env[k] = v
    env["THIN_AGENT_DEV_MODE"] = "1"
    log = open(log_path, "ab")
    proc = subprocess.Popen(
        ["./build/thin_agent", "--port", str(port),
         "--config", "config/demo.model.yaml", "--profile", profile, "--dev"],
        cwd=REPO, env=env, stdout=log, stderr=log,
        preexec_fn=os.setsid)
    if not wait_port(port):
        proc.terminate()
        raise RuntimeError(f"服务 {port} 未就绪，看 {log_path}")
    return proc


def stop_service(proc):
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        proc.wait(timeout=5)
    except Exception:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception:
            pass


def fresh_dir(path):
    shutil.rmtree(path, ignore_errors=True)
    os.makedirs(path, exist_ok=True)
    return path
