# thin_agent e2e：spawn 嵌套深度护栏（服务级，LLM=mock）
#
# 验证链路（真服务进程 + 真 WS + mock LLM）：
#   spawn_agent → 子代理 LLM（mock）返回 spawn_agent 工具调用
#   → 嵌套 spawn（同 mock 持续要求 spawn）→ 第 4 层被
#     "spawn nesting limit (3) reached" 拒绝 → 错误透传到结果
#
# 与单测 unit_spawn_depth_guard 的差异：走完整服务（三条 spawn 链
# 中的真实路径：WS 顶层→ToolRegistry 嵌套），mock LLM 保证确定性的
# 嵌套行为（真 GLM 对递归类 prompt 会拒答/兜底，不可控——真网
# 首验实测）。
import json
import os

import pytest

from conftest import AgentClient, start_service, stop_service

# v0.54.5 (R92): **隔离 home**——测试数据不得写进用户真实 ~/.thin_agent，也不得与其它
# e2e 抢同一份 sessions/goals/db（"全量红、单跑绿"这类抖动的结构性来源）。服务经
# conftest.start_service（env 继承本进程）会使用同一个隔离目录。
import shutil as _shutil
_HOME = "/tmp/ta_e2e_nest_home"
_shutil.rmtree(_HOME, ignore_errors=True)
os.makedirs(os.path.join(_HOME, "logs"), exist_ok=True)
os.environ["THIN_AGENT_HOME"] = _HOME

# mock 响应：LLM 永远要求调 spawn_agent（确定性递归）
MOCK_NEST = (
    '{"choices":[{"message":{"role":"assistant","content":"",'
    '"tool_calls":[{"id":"c1","type":"function","function":{'
    '"name":"spawn_agent","arguments":"{\\"goal\\":\\"continue nesting\\"}"'
    '}}]}}]}'
)

_proc = None
_SVC_LOG = os.path.join(_HOME, "logs", "agent_svc.log")


def _ensure_mock_service():
    global _proc
    if _proc is None:
        os.environ["THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE"] = MOCK_NEST
        _proc = start_service(port=8792, log_path="/tmp/thin_e2e_nest.log")
    return _proc


@pytest.fixture(scope="session")
def mock_client():
    _ensure_mock_service()
    c = AgentClient("ws://127.0.0.1:8792/ws")
    yield c
    c.close()
    stop_service(_ensure_mock_service())
    os.environ.pop("THIN_AGENT_TEST_CLOUD_TOOLS_RESPONSE", None)  # 泄漏防护


def test_spawn_nesting_limit(mock_client):
    """第 4 层嵌套被拦+错误透传（mock 保证每层都发起 spawn）。"""
    mock_client.ws.settimeout(120)
    mock_client.ws.send(json.dumps({
        "type": "spawn_agent",
        "goal": "start nesting chain",
        "role": "developer",
        "max_turns": 6,
        "timeout_ms": 30000,
    }))
    r = None
    import json as _json
    while True:
        j = _json.loads(mock_client.ws.recv())
        if j.get("type") == "spawn_result":
            r = j
            break
    assert r.get("type") == "spawn_result"
    # v0.54.11 (R93) 断言修正——原断言**腐烂**（未登记 ⇒ 在 CI 里从不跑，烂了没人看见）：
    #   旧断言只认"护栏文案直传到顶层结果"，而 mock 链路里顶层的 6 轮先耗尽
    #   （`turns_exhausted`），护栏文案留在**子层**——且护栏拒绝当时**根本没有日志**，
    #   于是 e2e 既看不到顶层文案、也看不到服务端痕迹（R93 复核：1 failed + 日志 0 命中）。
    #   同时旧注释写着"turns 受限也算通过"，白名单里却没有 turns_exhausted 文案 ⇒ 自相矛盾。
    # 现在断言两件**机械可验**的事：
    #   ① 链路**有界终止**（护栏的设计目标就是"嵌套链不得无界扩展"——若无界会是 6^6 级爆炸）
    #   ② **护栏确实开火过**：服务端日志含 "spawn nesting limit"（v0.54.11 起护栏拒绝会打
    #      [WARN]；此前该拒绝对运维完全不可见）
    out = json.dumps(r, ensure_ascii=False)
    bounded = (r.get("error") == "turns_exhausted"
               or "nesting limit" in out
               or "处理步骤较多" in out
               or "too many active sub-agents" in out)
    assert bounded, ("嵌套链未在有界轮次内终止（护栏失效的信号）: %s" % out[:300])
    log_txt = ""
    try:
        with open(_SVC_LOG, encoding="utf-8", errors="ignore") as f:
            log_txt = f.read()
    except OSError:
        pass
    assert "spawn nesting limit" in log_txt, (
        "深度护栏从未开火：服务端日志无 'spawn nesting limit' 拒绝行"
        "（v0.54.11 起该拒绝会打 [WARN]）——若链路真无界，这里会是指数爆炸而非有界终止；"
        "请查 g_spawn_nest_depth 计数与拒绝分支是否被绕过")
    # 服务存活（嵌套链终止后不崩——此前堆损坏时代的回归锚点）
    j2 = mock_client.chat("回复：存活", "nest-alive-check", timeout=60)
    assert len(j2.get("text", "")) > 0
    # 深度拦截的单元级验证由 unit_spawn_depth_guard 覆盖
