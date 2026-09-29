# thin_agent WS 客户端测试工具包

基于 WebSocket 协议的 thin_agent 系统测试与调试工具集。

## 文件

| 文件 | 用途 | 耗时 |
|:---|:---|:---|
| `ws_client.py` | 核心库 — `WsClient` 类，封装连接/发送/接收/断言 | — |
| `ws_smoke.py` | 冒烟测试 — 6 用例快速回归 | ~5s |
| `ws_system_test.py` | 系统测试 — 8 用例全场景覆盖（异常/多轮/FC/边界） | ~90s |
| `ws_repl.py` | 交互式调试终端 — 手动发送消息，观察响应 | — |

## 测试分层

| 层级 | 工具 | 场景 | 运行时机 |
|:---|:---|:---|:---|
| **冒烟** | `ws_smoke.py` | Happy Path 基础功能 | CI 每次提交 |
| **系统** | `ws_system_test.py` | 异常路径 / 多轮对话 / FC 复杂任务 / 并发 / 边界 | 发布前 |
| **单元** | ctest（C++） | 函数级正确性 | CI 每次提交 |

## 安装

```bash
pip install -r requirements.txt
```

## 快速开始

### 冒烟测试（~5s）

```bash
# 全量冒烟（6 项）
python ws_smoke.py

# 快速检查（3 项）
python ws_smoke.py --quick

# 仅 Profile 场景
python ws_smoke.py --profile

# 详细输出
python ws_smoke.py --verbose
```

### 系统测试（~90s）

```bash
# 全量（8 用例：异常路径 + 多轮对话 + FC + 边界）
python ws_system_test.py

# 按类别运行
python ws_system_test.py --category error    # 异常路径（断连重连、工具失败）
python ws_system_test.py --category multi    # 多轮对话（上下文保持、会话隔离）
python ws_system_test.py --category fc       # FC 复杂任务（多文件操作、并发）
python ws_system_test.py --category edge     # 边界场景（空消息、详细模式）

# 详细输出
python ws_system_test.py --verbose
```

### 交互式调试

```bash
python ws_repl.py
```

进入后直接输入消息，如 `你是谁`、`天气 北京`。

命令：
- `/raw` — 切换原始 JSON 模式（显示 thinking 帧等中间帧）
- `/verbose` — 切换详细模式（显示完整 text）
- `/help` — 帮助
- `/q` — 退出

### 编程调用

```python
from ws_client import WsClient

with WsClient() as ws:
    # 基本用法
    result = ws.chat("你是谁")
    print(result.text)

    # 断言
    ws.assert_route("你是谁", "local_profile")
    ws.assert_text_contains("你是谁", "编码智能体")

    # 获取原始 JSON
    raw = ws.raw_chat("目前有哪些技能")
    print(raw["decision"]["route"])
```

## 冒烟测试覆盖

| 测试 | 查询 | 验证点 |
|:---|:---|:---|
| `profile_who` | 你是谁 | route=local_profile, 含"编码智能体" |
| `profile_skills_only` | 目前有哪些技能 | route=local_profile, 只列技能无开场白 |
| `profile_skills_variant` | 有什么功能 | route=local_profile, skills_only |
| `external_weather` | 天气 北京 | route=local_external_weather |
| `fc_echo` | echo hello world | cloud_fc 或 offline fallback |
| `fc_file_list` | 列出当前目录下的 cpp 文件 | FC 迭代正常 |

## 系统测试覆盖

| 类别 | 测试 | 验证点 |
|:---|:---|:---|
| **error** | `reconnect` | WS 断连重连后功能正常 |
| | `tool_failure` | 工具执行失败不崩溃 |
| | `json_sanitize` | 特殊字符查询不崩溃（UTF-8 清洗回归） |
| **multi** | `multi_turn` | 3 轮连续对话上下文保持 |
| | `session_isolation` | 不同 session_id 隔离 |
| | `spawn_agent` | 子 Agent 派生子不崩溃 |
| **fc** | `fc_file_ops` | 多文件创建→验证→清理（自适应迭代） |
| | `fc_compile_chain` | C++ 创建→编译→运行→清理（自适应迭代+自动延期） |
| | `fc_concurrent` | 2 并发请求不崩溃 |
| **edge** | `detailed_profile` | 详细模式大量文本不截断 |
| | `empty_query` | 空消息不崩溃 |
| **fault** | `kill_restart` | Kill 重启后功能恢复 |

## 扩展

在 `ws_smoke.py` 的 `make_test_cases()` 中添加新用例：

```python
tests.append(TestCase(
    name="my_test",
    query="你的查询",
    check=lambda r: r.has_route("expected_route") and "expected_text" in r.text,
    description="描述",
))
```

## 依赖

- Python 3.8+
- `websocket-client`（pip install websocket-client）
- thin_agent 运行在 `ws://127.0.0.1:8765/ws`
