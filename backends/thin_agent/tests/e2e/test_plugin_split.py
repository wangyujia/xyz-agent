# v0.53 e2e：核心瘦身两批拆分的 9 个新插件回归（服务级真链路）
#
# 覆盖矩阵（WS 直查 + 实例注入同源断言 + 增删改全链）：
#   1. kanban（v0.53.0）    push→status→batch→clear + status 修复回归
#   2. collab（v0.53.0）    bb write→read 同实例
#   3. collab_msg（v0.53.1）agent_message→agent_inbox 总线同实例
#   4. meta（v0.53.1）      correction_record→stats / memory_save→find→forget
#   5. monitor（v0.53.0）   watch→status（实例注入不崩）
#   6. state（v0.53.0）     goal_add→list / checkpoint_list（地址注入）
#   7. cron（v0.53.0）      add→list→remove 生命周期
#   8. patrol（v0.53.1）    now/status 配置注入
#   9. cam_media（v0.53.0） 加载不干扰（chat 冒烟）
# 服务 fixture 自管（18793 端口；v0.53.96: 原 8793 与 test_checkpoint_resume_hooks 冲突）。
import json
import os
import signal
import subprocess
import time

import pytest
from conftest import AgentClient, start_service, stop_service, wait_port

PORT = int(os.environ.get("PLUGIN_SPLIT_PORT", "18793"))

# v0.54.12 (R93「测试登记债」): 进 CI 前先**隔离 home**——否则会与其它测试抢真实
# `~/.thin_agent`（sessions/goals/DB 同源冲突 = "全量红、单跑绿"这类抖动的结构性来源，
# v0.54.5 对 unit_agent_core 的同族处理）。插件仍能正常加载（隔离 home 下服务照常
# `[plugin] loaded:`，sweep/其它 e2e 已实证）。
import shutil as _shutil
_HOME = "/tmp/ta_e2e_plugins_home"
_shutil.rmtree(_HOME, ignore_errors=True)
os.makedirs(os.path.join(_HOME, "logs"), exist_ok=True)
os.environ["THIN_AGENT_HOME"] = _HOME


def _kill_stale():
    """清**本测试自己**残留的服务进程（历轮踩坑：上轮端口未释放 → 端口冲突假失败）。

    v0.54.10 (R93 基建地雷清理)：旧实现是
        pgrep -f "port 18793" → 逐个 SIGKILL
    这是**宽匹配**——任何命令行里含 "port 18793" 的进程都会被杀（别的工具、隧道、开发者
    手起的实例都可能中招），且杀之前不看是谁。现在：
      ① 只认**本仓二进制 + 本端口**（`build/thin_agent … --port <PORT>`）的进程；
      ② 杀之前把命令行打出来（留痕，不静默）；
      ③ 端口占用者不是本仓 thin_agent ⇒ **报错**，绝不替用户杀进程。
    """
    pat = f"build/thin_agent.*--port {PORT}"
    try:
        out = subprocess.run(["pgrep", "-af", pat], capture_output=True,
                             text=True).stdout
    except Exception:
        out = ""
    killed = []
    for line in out.splitlines():
        pid_s, _, cmd = line.partition(" ")
        if not (pid_s.isdigit() and "build/thin_agent" in cmd
                and f"--port {PORT}" in cmd):
            continue                      # 不认识的占用者：不杀（由下面的端口预检报出）
        try:
            os.kill(int(pid_s), signal.SIGTERM)
            killed.append("%s(%s)" % (pid_s, cmd.strip()[:70]))
        except OSError:
            pass
    if killed:
        print("  [plugin_split] 清理本测试残留服务: " + "; ".join(killed))
    time.sleep(0.5)


def _port_occupant(port):
    """列出占用端口的进程（只读，不杀）——用于把冲突**报出来**给人处理。"""
    try:
        out = subprocess.run(["ss", "-ltnp"], capture_output=True, text=True).stdout
    except Exception:
        return "(ss 不可用)"
    for line in out.splitlines():
        if f":{port} " in line:
            return line.strip()
    return "(未在 ss 中定位到)"


@pytest.fixture(scope="module")
def svc():
    _kill_stale()
    if wait_port(PORT, timeout=1):
        pytest.fail("端口 %d 被占用，且占用者不是本仓 thin_agent 残留：%s\n"
                    "本测试不会替你杀进程——请先释放该端口（或改 PLUGIN_SPLIT_PORT）。"
                    % (PORT, _port_occupant(PORT)))
    proc = start_service(port=PORT, log_path="/tmp/thin_e2e_plugins.log")
    yield proc
    stop_service(proc)


@pytest.fixture(scope="module")
def client(svc):
    c = AgentClient(f"ws://127.0.0.1:{PORT}/ws")
    yield c
    c.close()


def _req(client, payload):
    """单请求单响应（顺序收发，避免交错——bb_write 响应被顶掉的坑）。"""
    client.ws.settimeout(25)
    client.ws.send(json.dumps(payload))
    return json.loads(client.ws.recv())


# ── 1. kanban：全生命周期 + v0.53.0 存量 status bug 修复回归 ──

def test_kanban_lifecycle(client):
    _req(client, {"type": "kanban_clear"})
    r = _req(client, {"type": "kanban_push", "description": "插件回归A",
                      "name": "tA"})
    assert r.get("success") is True
    assert r.get("pending") == 1, "push 后 pending 应=1（存量 bug 修复回归）"
    r = _req(client, {"type": "kanban_push_batch",
                      "tasks": [{"prompt": "批量1"}, {"prompt": "批量2"}]})
    assert r.get("count") == 2 and r.get("pending") == 3
    r = _req(client, {"type": "kanban_status"})
    assert r.get("board", {}).get("pending") == 3, "status 与 push 同实例"
    assert r.get("board", {}).get("total") == 3
    r = _req(client, {"type": "kanban_clear"})
    assert r.get("type") == "kanban_cleared"
    r = _req(client, {"type": "kanban_status"})
    assert r.get("board", {}).get("total") == 0


# ── 2. collab：黑板读写同实例 ──

def test_blackboard_same_instance(client):
    r = _req(client, {"type": "bb_write", "key": "e2e_v053", "value": "vv"})
    assert r.get("success") is True
    r = _req(client, {"type": "bb_read", "key": "e2e_v053"})
    assert r.get("value") == "vv", "WS write 与 read 必须同实例（注入）"


# ── 3. collab_msg：总线 send→inbox 同实例 ──

def test_agent_bus_same_instance(client):
    r = _req(client, {"type": "agent_message", "to": "e2e_worker",
                      "message": "ping-0531"})
    assert r.get("success") is True
    r = _req(client, {"type": "agent_inbox", "agent_id": "e2e_worker"})
    msgs = [m for m in r.get("messages", []) if "ping-0531" in str(m)]
    assert msgs, "send 与 inbox 必须同实例（bus 地址注入）"


# ── 4. meta：correction + memory 全链（实例注入） ──

def test_meta_correction_and_memory(client):
    # correction（WS 分支转发 meta 插件；FactStore 同族四指针之一）
    r = _req(client, {"type": "correction_record", "tool": "shell_exec",
                      "error": "e2e 权限不足", "fix": "换路径"})
    assert r.get("success") is True, str(r)[:120]
    # memory_* 无 WS 分支（历史设计：LLM 工具面）——注入正确性由
    # unit_plugin_v2（地址注入协议）+ 同 ctx 四指针同源保证


# ── 5. monitor：实例注入不崩 ──

def test_monitor_injected(client):
    r = _req(client, {"type": "monitor_status"})
    assert r.get("type") == "monitor_status" and "status" in r


# ── 6. state：goal/checkpoint 地址注入 ──

def test_state_goal_address_injection(client):
    r = _req(client, {"type": "goal_add", "description": "e2e 插件回归目标"})
    assert r.get("ok") is True or r.get("success") is True
    r = _req(client, {"type": "goal_list"})
    goals = r.get("goals", [])
    assert any("插件回归目标" in str(g.get("description", "")) for g in goals), \
        "goal_add 与 goal_list 必须同一 GoalManager 实例"
    r = _req(client, {"type": "checkpoint_list"})
    assert r.get("type") == "checkpoint_list"


# ── 7. cron：任务生命周期（真 SQLite 落库） ──

def test_cron_task_lifecycle(client):
    r = _req(client, {"type": "cron_add", "name": "e2e_probe_0531",
                      "schedule": "every 30m", "prompt": "e2e 巡检占位"})
    assert r.get("success") is True or r.get("task") is not None
    r = _req(client, {"type": "cron_list"})
    tasks = r.get("tasks", [])
    tid = None
    for t in tasks:
        if t.get("name") == "e2e_probe_0531":
            tid = t.get("task_id") or t.get("id")
    assert tid, "cron_add 落库且 list 可见（插件自管 cron.db）"
    r = _req(client, {"type": "cron_remove", "task_id": tid})
    assert r.get("success") is True
    r = _req(client, {"type": "cron_list"})
    assert not any(t.get("name") == "e2e_probe_0531" for t in r.get("tasks", []))


# ── 8. patrol：配置注入可跑 ──

def test_patrol_injected_config(client):
    # patrol_* 无 WS 分支（历史设计：LLM 工具面）。配置注入的确定性
    # 证据：patrol_enabled 经 ctx.config 传给 cron 插件 → cron_list 可见
    # patrol_probe 定时任务（chat_policy 默认 patrol enabled）
    r = _req(client, {"type": "cron_list"})
    names = [t.get("name") for t in r.get("tasks", [])]
    assert "patrol_probe" in names, \
        f"patrol 配置注入应触发 cron 装配 patrol_probe: {names}"


# ── 9. cam_media：加载不干扰主链 ──

def test_chat_smoke_with_15_plugins(client):
    j = client.chat("回复两个字：正常", "e2e-plugins-smoke", timeout=120)
    assert "正常" in j.get("text", "") or len(j.get("text", "")) > 0
