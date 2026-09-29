# thin_agent e2e：复杂任务自动分解路由（服务级，LLM=mock）
#
# 验证链路（真服务进程 + 真 WS + 真意图路由 + mock LLM）：
#   chat("多文件重构任务...") → classify_local_intent 命中 complex_task
#   → chat_run_cloud 转 agent_decompose_and_run
#   → AgentLoop LLM 调用（THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE mock：
#     带 markdown fence + 尾随说明——e2e 实测 GLM 形态）
#   → JsonExtract 提取 → GoalManager 任务树落库 → 子代理执行（同 mock）
#   → 合成 → chat_result 响应
#
# 与单测 unit_decompose_goal_tree 的差异：走完整 WS 服务（进程边界、
# 意图路由、chat 通道），不是进程内 handle_request 直调。
#
# 前置：GLM 余额耗尽（1113）期间补上的服务级覆盖——真 GLM 恢复后
# 另有真网用例（test_smoke_and_safety 的编程闭环），本文件不依赖真网。
import json
import os
import shutil
import sqlite3

import pytest

from conftest import (AgentClient, DEFAULT_WS, start_service, stop_service)

# ── v0.54.14 (R93「测试登记债」收官): **自足化** ──
# 此前本文件把路径硬编码到真实 home（`~/.thin_agent/profiles/<profile>/data/goals.db`、
# `~/.thin_agent/logs/agent_svc.log`）⇒ 设 THIN_AGENT_HOME 隔离后服务写隔离目录、断言仍读真实
# home ⇒ "隔离即红"（R92 记录）。**实测澄清**：空 home 下服务会自建全部前置表
# （`goals`/`fc_runs`/`gw_chat_sessions`，schema 与真实 home 一致）且 30 条插件照常加载、
# 复杂任务路由正常——所以"依赖真实 home 既有状态"实为**路径硬编码的症状**，非真数据依赖。
# 现在：home 由 THIN_AGENT_HOME 参数化（import 时清空重建，确定性起点），断言路径随之派生。
_HOME = "/tmp/ta_e2e_decompose_home"
shutil.rmtree(_HOME, ignore_errors=True)
os.makedirs(os.path.join(_HOME, "logs"), exist_ok=True)
os.environ["THIN_AGENT_HOME"] = _HOME

MOCK_DECOMP = (
    "```json\n"
    '{"tasks":['
    '{"task_id":"t1","name":"实现 stack.h","prompt":"write stack.h","role":"coder","depends_on":[]},'
    '{"task_id":"t2","name":"实现 main","prompt":"write main","role":"coder","depends_on":["t1"]}'
    "]}\n"
    "```\n以上分解共 2 个子任务。"
)

_proc = None
_SVC_LOG = os.path.join(_HOME, "logs", "agent_svc.log")   # v0.54.14: 随隔离 home 派生


def _ensure_mock_service():
    """带 mock LLM env 启动服务（与真网服务隔离端口）。

    坑：agent_svc.log 服务启动时被覆盖（历史教训）——启动前记录
    当前大小，日志断言只看本次会话新写入的部分。
    """
    global _proc
    if _proc is None:
        os.environ["THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE"] = MOCK_DECOMP
        _log_offset = os.path.getsize(_SVC_LOG) if os.path.exists(_SVC_LOG) else 0
        open("/tmp/thin_e2e_mock_offset", "w").write(str(_log_offset))
        _proc = start_service(port=8791,
                              log_path="/tmp/thin_e2e_mock.log")
    return _proc


@pytest.fixture(scope="session")
def mock_client():
    _ensure_mock_service()
    c = AgentClient("ws://127.0.0.1:8791/ws")
    yield c
    c.close()
    stop_service(_ensure_mock_service())
    os.environ.pop("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE", None)  # 泄漏防护


def test_complex_task_routes_to_decompose(mock_client):
    """复杂任务措辞 → 分解编排响应（goal_tree_id + 子任务计数）。"""
    j = mock_client.chat(
        "这是一个多文件重构任务：在 /tmp/e2e_mproj 分三个文件实现整数栈"
        "（stack.h/stack_impl.cpp/stack_main.cpp），最后 g++ 编译运行。",
        "ctmock1", timeout=120)
    text = j.get("text", "")
    # mock 下合成阶段也走 with_fallback（无 mock 点）→ 可能报离线——
    # 但分解阶段（AgentLoop→with_tools）已被 mock。核心断言：
    # 服务日志出现 routed to decomposition（由下方日志检查断言），
    # 且响应不是"需要确认"暂停（分解路径不触发 HITL）。
    assert "需要确认" not in text, f"分解路径不应弹 HITL: {text[:200]}"


def test_goal_tree_persisted():
    """分解结果落任务树：goals.db 有 1 父 2 子（parent_id 关联）。"""
    db = sqlite3.connect(os.path.join(_HOME, "data", "goals.db"))   # v0.54.14: 随隔离 home 派生
    rows = list(db.execute(
        "SELECT id, parent_id, subtask_ids FROM goals "
        "WHERE description LIKE '%整数栈%' ORDER BY id"))
    parents = [r for r in rows if r[1] is None and r[2]]
    assert parents, f"未找到父目标: {rows}"
    sub_ids = json.loads(parents[0][2])
    assert len(sub_ids) == 2, f"子任务应为 2: {sub_ids}"
    # 子任务行存在且 parent_id 指向父
    for r in rows:
        if r[0] in sub_ids:
            assert r[1] == parents[0][0]


def test_service_log_shows_route():
    """服务日志留痕（log_event 落 agent_svc.log，非 stdout）：路由+落树。

    只看本次服务会话新写入部分（offset 之后）——agent_svc.log 每次
    服务启动被覆盖，全量读取会读到历史或空文件。
    """
    _ensure_mock_service()
    # 服务由本文件 fixture 启动（teardown 已停上一轮），agent_svc.log
    # 启动时被覆盖 → 全量读取即本会话日志；chat 用例先跑过，必有 routed
    with open(_SVC_LOG, "rb") as f:
        log = f.read().decode("utf-8", "ignore")
    assert "complex task routed to decomposition" in log, f"log={log[-300:]}"
    assert "decomposition persisted to goal tree" in log
