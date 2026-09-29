# 云调用闭环验证（thin_agent v0.3.61）

本 runbook 用于在本机快速复现“真实 HTTP 云调用闭环”（通过本地 mock OpenAI-compatible 服务）。

## 1) 构建

```bash
cd /root/code/thin_agent
cmake --build build -j
```

## 2) 启动 mock 云服务（终端 A）

```bash
python3 tests/demo/mock_openai_server.py --host 127.0.0.1 --port 18080
```

预期输出：

```text
MOCK_OPENAI_LISTEN 127.0.0.1:18080
```

## 3) 启动 thin_agent（终端 B）

```bash
THIN_AGENT_CLOUD_TEST_KEY=dummy \
./build/thin_agent \
  --host 127.0.0.1 \
  --port 8765 \
  --config config/demo.model.yaml \
  --profile cloud_mock_demo
```

## 4) 执行云闭环 E2E（终端 C）

```bash
python3 tests/demo/e2e_cloud_chat.py \
  --host 127.0.0.1 \
  --port 8765 \
  --path /ws \
  --expect-mode cloud \
  --expect-substr MOCK_CLOUD_OK
```

预期输出示例：

```text
E2E_CLOUD_OK
mode_used cloud
cloud_http_status 200
text MOCK_CLOUD_OK model=mock-model echo=请返回一句云端验证文本
```

## 5) 回归基础 E2E

```bash
python3 tests/demo/e2e_ws_v02.py --host 127.0.0.1 --port 8765 --path /ws
```

预期输出包含：

```text
E2E_OK
```

## 6) 清理

停止终端 A/B 进程，或 `Ctrl+C` 退出。
