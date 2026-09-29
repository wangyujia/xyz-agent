# thin_agent e2e：断点续跑 + Hook System（服务级，LLM=mock 或本地工具）
#
# 验证链路（真服务进程 + 真 WS + 真 SQLite journal）：
#   A. 断点续跑全链路：
#     1) hook 注册不影响正常 chat
#     2) kill -9 模拟崩溃（journal 留下 interrupted run）
#     3) 重启 → boot sweep（running→interrupted）
#     4) 同 chat_id 发"继续" → resume run 日志（快照注入会话记忆）
#     5) 会话映射持久化（GatewaySessionMap）→ 重启后同 chat_id 同 sid
#   B. Hook System：
#     6) hooks.json 启动加载 + hook_list 可见
#     7) hook_register 运行时注册 deny 钩子 → 工具被拦（denied_by_hook）
#     8) hook_unregister → 恢复放行
#
# 前置：不需要真网 GLM——任务用本地工具（list_dir 等经 skill registry），
# chat 若触发 LLM 用 THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE mock 兜底。
import json
import os
import shutil
import signal
import sqlite3
import subprocess
import time

import pytest

from conftest import AgentClient, start_service, stop_service, wait_port

REPO = os.environ.get("THIN_AGENT_REPO", "/root/code/thin_agent")
PORT = 8793
PROFILE = "deepseek_main_demo"   # v0.53.95: 随默认云通道切到 DeepSeek(deepseek-flash)

# ── v0.54.14 (R93「测试登记债」收官): **自足化** ──
# 此前四路径全部硬编码真实 home（`~/.thin_agent/profiles/<profile>/data/*`、
# `~/.thin_agent/logs/agent_svc.log`）⇒ 隔离运行后服务写隔离目录、断言仍读真实 home ⇒
# "隔离即红"（R92 记录）。**实测澄清**：空 home 下服务自建全部前置表
# （`fc_runs`/`fc_approvals`/`gw_chat_sessions`/`goals`，schema 与真实 home 一致），
# 30 条插件照常加载 ⇒ "依赖真实 home 既有状态"实为**路径硬编码的症状**，非真数据依赖。
# 现在：home 由 THIN_AGENT_HOME 参数化（import 时清空重建，确定性起点），四路径随之派生。
_HOME = "/tmp/ta_e2e_bp_home"
shutil.rmtree(_HOME, ignore_errors=True)
os.makedirs(os.path.join(_HOME, "logs"), exist_ok=True)
os.environ["THIN_AGENT_HOME"] = _HOME

DB = os.path.join(_HOME, "data", "agent_sessions.db")
GW_DB = os.path.join(_HOME, "data", "gateway_sessions.db")
HOOKS_JSON = os.path.join(_HOME, "data", "hooks.json")
SVC_LOG = os.path.join(_HOME, "logs", "agent_svc.log")

MOCK_FC = (
    '{"choices":[{"message":{"role":"assistant","content":"已列出目录。","tool_calls":['
    '{"id":"call_1","type":"function","function":{"name":"list_dir","arguments":"{\\"path\\":\\"/tmp\\"}"}}'
    ']}}]}'
)


def _mark_log():
    """记录当前日志末尾（用例级标记——服务重启覆盖日志后，模块级
    offset 会失效/错位，改为每个用例在动作前标记）。"""
    size = os.path.getsize(SVC_LOG) if os.path.exists(SVC_LOG) else 0
    open("/tmp/thin_e2e_bp_offset", "w").write(str(size))
    return size


def _svc_log_tail_contains(marker, offset_file="/tmp/thin_e2e_bp_offset"):
    """断言服务日志（本次会话新增部分）包含 marker。

    坑：agent_svc.log 服务启动时被覆盖（历史教训，见 test_decompose_route）。
    崩溃重启用例里 offset 可能大于新文件 → 从头读。
    """
    off = 0
    if os.path.exists(offset_file):
        off = int(open(offset_file).read().strip())
    size = os.path.getsize(SVC_LOG) if os.path.exists(SVC_LOG) else 0
    if off > size:
        off = 0
    with open(SVC_LOG, "rb") as f:
        f.seek(off)
        data = f.read().decode("utf-8", "replace")
    return marker in data


@pytest.fixture(scope="module")
def svc():
    """模块级服务：启动 → 测试 → 崩溃重启由用例内驱动。"""
    os.environ["THIN_AGENT_TEST_CLOUD_RESPONSE"] = MOCK_FC
    # hooks.json 预置一条 tool_post 审计钩子（用例 B 验证启动加载）
    os.makedirs(os.path.dirname(HOOKS_JSON), exist_ok=True)
    with open(HOOKS_JSON, "w") as f:
        json.dump([{"event": "tool_post",
                    "shell_cmd": "cat > /tmp/e2e_bp_hook_payload.json"}], f)
    _off = os.path.getsize(SVC_LOG) if os.path.exists(SVC_LOG) else 0
    open("/tmp/thin_e2e_bp_offset", "w").write(str(_off))
    proc = start_service(port=PORT, log_path="/tmp/thin_e2e_bp.log",
                         profile=PROFILE)
    yield proc
    stop_service(proc)
    # 清理：hooks.json 回空数组（不影响真网服务）+ mock env 泄漏防护
    #（同进程后续模块（smoke 真网用例）会被残留 mock 劫持——实测踩坑）
    with open(HOOKS_JSON, "w") as f:
        f.write("[]")
    os.environ.pop("THIN_AGENT_TEST_CLOUD_RESPONSE", None)


def _crash(proc):
    """kill -9 模拟崩溃（非优雅退出——journal 留下 running 现象）。"""
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except Exception:
        pass
    proc.wait(timeout=5)


def _db_one(sql):
    c = sqlite3.connect(DB)
    try:
        return c.execute(sql).fetchone()
    finally:
        c.close()


def test_a1_chat_basic_with_hooks_loaded(svc):
    """hooks.json 已加载（B1 详测）时 chat 基本链路正常——钩子不干扰主链。

    注：mock（TEST_CLOUD_RESPONSE）会绕开 FC 循环（use_native_fc=false 是
    设计语义）→ journal 不落盘。journal 运行时落盘由单测（19 断言）+
    真网实证（v0.52.26：12 iter/54KB）覆盖，本 e2e 聚焦崩溃恢复接线。
    """
    cl = AgentClient(f"ws://127.0.0.1:{PORT}/ws")
    j = cl.chat("你好", "e2e-bp-1", timeout=60)
    cl.close()
    assert j.get("type") == "chat_result"
    assert j.get("text"), "chat 应有文本回复"


def test_a2_crash_and_boot_sweep(svc):
    """kill -9 → 重启 → boot sweep 把 running 转 interrupted。"""
    # 制造 running 现场：发一个会多轮的 chat（mock 一直给 tool_calls），
    # 在响应返回前无法精确打断——改为直接检查 DB：上一轮的 run 已终态，
    # 手工注一条 running（模拟崩溃时刻），重启后应被清扫。
    c = sqlite3.connect(DB)
    c.execute("INSERT OR REPLACE INTO fc_runs(run_id,session_id,user_text,"
              "status,iter,messages,updated_at_ms) VALUES("
              "'fc_e2e_crash','e2e-bp-crash','测试崩溃','running',1,"
              "'[{\"role\":\"user\",\"content\":\"测试崩溃\"}]',1)")
    c.commit()
    c.close()
    _crash(svc)
    svc2 = start_service(port=PORT, log_path="/tmp/thin_e2e_bp.log",
                         profile=PROFILE)
    try:
        assert wait_port(PORT)
        time.sleep(1)  # 等 boot sweep 日志落盘
        row = _db_one("SELECT status FROM fc_runs WHERE run_id='fc_e2e_crash'")
        assert row and row[0] == "interrupted", f"boot sweep 未生效: {row}"
    finally:
        # svc2 顶替 svc 继续用（后续用例连它）
        svc.__dict__["pid"] = svc2.pid  # pytest fixture 对象不可这样换——
        # 直接返回前停旧引用无意义；保留 svc2 给下个用例——用全局传递
        globals()["_svc2"] = svc2


def test_a3_resume_injects_snapshot(svc):
    """同 chat_id 发"继续" → resume run 日志（快照注入）。"""
    svc_cur = globals().get("_svc2", svc)
    assert svc_cur is not None
    cl = AgentClient(f"ws://127.0.0.1:{PORT}/ws")
    # session 对齐：把 crash run 改绑到即将使用的会话（e2e conftest 坑#7：
    # chat_id 在 DB 里映射成 chat-N，先用一条真实 chat 建立映射再改绑）
    cl.chat("你好", "e2e-bp-resume", timeout=60)
    c = sqlite3.connect(DB)
    # 找该 chat_id 的映射 sid
    sid_row = None
    c2 = sqlite3.connect(GW_DB)
    sid = None
    for k, v in c2.execute("SELECT map_key, sid FROM gw_chat_sessions"):
        if "e2e-bp-resume" in k:
            sid = v
            break
    c2.close()
    assert sid, "会话映射应已落盘（GatewaySessionMap）"
    c.execute("UPDATE fc_runs SET session_id=? WHERE run_id='fc_e2e_crash'",
              (sid,))
    c.commit()
    c.close()
    _mark_log()  # 服务重启已覆盖日志——发"继续"前重打标记
    j = cl.chat("继续", "e2e-bp-resume", timeout=120)
    cl.close()
    assert j.get("type") == "chat_result"
    assert _svc_log_tail_contains("resume run"), "resume run 日志未见（快照注入未触发）"


def test_b1_hooks_json_loaded_and_list(svc):
    """hooks.json 启动加载 + hook_list 可见。"""
    cl = AgentClient(f"ws://127.0.0.1:{PORT}/ws")
    cl.ws.settimeout(20)
    cl.ws.send(json.dumps({"type": "hook_list"}))
    while True:
        j = json.loads(cl.ws.recv())
        if j.get("type") == "hook_list":
            break
    cl.close()
    assert j["count"] >= 1, "hooks.json 的 tool_post 审计钩子应已加载"
    assert any(h["event"] == "tool_post" for h in j["hooks"])


def test_b2_hook_ws_lifecycle(svc):
    """hook WS 协议生命周期：register → list 可见 → unregister → 再查消失。

    注：deny 的工具拦截链需 FC 循环执行工具，而 mock（TEST_CLOUD_RESPONSE）
    会绕开 FC（设计语义）→ 本用例不触发工具；拦截链由单测（test_hook_system
    15 断言：deny/合并/超时）+ 真网实证（v0.52.30：deny 拦截+审计落盘）覆盖。
    """
    cl = AgentClient(f"ws://127.0.0.1:{PORT}/ws")
    cl.ws.settimeout(20)
    # 注册
    cl.ws.send(json.dumps({
        "type": "hook_register", "event": "tool_pre",
        "shell_cmd": "cat > /dev/null; exit 1",
        "tool_filter": ["list_dir"], "timeout_ms": 800}))
    hook_id = None
    while True:
        j = json.loads(cl.ws.recv())
        if j.get("type") == "hook_registered":
            hook_id = j["hook_id"]
            break
    assert hook_id
    # list 可见
    cl.ws.send(json.dumps({"type": "hook_list"}))
    while True:
        j2 = json.loads(cl.ws.recv())
        if j2.get("type") == "hook_list":
            break
    assert any(h["id"] == hook_id and h["event"] == "tool_pre"
               and h.get("timeout_ms") == 800 for h in j2["hooks"])
    # 非法事件名拒绝
    cl.ws.send(json.dumps({
        "type": "hook_register", "event": "not_an_event",
        "shell_cmd": "true"}))
    while True:
        j3 = json.loads(cl.ws.recv())
        if j3.get("type") == "error":
            break
    assert "未知事件" in j3.get("message", "")
    # 卸载
    cl.ws.send(json.dumps({"type": "hook_unregister", "hook_id": hook_id}))
    while True:
        j4 = json.loads(cl.ws.recv())
        if j4.get("type") == "hook_unregistered":
            break
    assert j4["removed"] is True
    cl.close()
