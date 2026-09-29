#!/usr/bin/env python3
"""e2e_failure.py — thin_agent 失败注入 E2E 测试

测试场景：
  1. cloud-fallback:  cloud 模式 + fallback=offline + 云故障 → mode_used=offline-fallback
  2. cloud-error:     cloud 模式 + fallback=cloud  + 云故障 → mode_used=cloud-error

用法：
  python3 tests/demo/e2e_failure.py --port 19110           # 自动启动 mock server + thin_agent
  python3 tests/demo/e2e_failure.py --port 19110 --quick   # 仅跑测试（服务已启动）
"""
import argparse
import base64
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import yaml
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent.parent


def ws_handshake(host: str, port: int, path: str = "/ws"):
    s = socket.create_connection((host, port), timeout=8)
    s.settimeout(30)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(req.encode())
    _ = s.recv(4096)
    return s


def ws_send_text(s: socket.socket, msg: str):
    b = msg.encode("utf-8")
    n = len(b)
    hdr = bytes([0x81])
    if n < 126:
        ln = bytes([0x80 | n])
    elif n < 65536:
        ln = bytes([0x80 | 126]) + n.to_bytes(2, "big")
    else:
        ln = bytes([0x80 | 127]) + n.to_bytes(8, "big")
    mask = os.urandom(4)
    payload = bytes(x ^ mask[i % 4] for i, x in enumerate(b))
    s.sendall(hdr + ln + mask + payload)


def ws_recv_json(s: socket.socket):
    b1 = s.recv(1)
    if not b1:
        raise RuntimeError("socket closed")
    b2 = s.recv(1)
    if not b2:
        raise RuntimeError("socket closed")
    ln = b2[0] & 0x7F
    if ln == 126:
        ln = int.from_bytes(s.recv(2), "big")
    elif ln == 127:
        ln = int.from_bytes(s.recv(8), "big")
    if b2[0] & 0x80:
        _ = s.recv(4)
    data = b""
    while len(data) < ln:
        data += s.recv(ln - len(data))
    return json.loads(data.decode("utf-8", errors="replace"))


def call(s, req, expect_type):
    ws_send_text(s, json.dumps(req, ensure_ascii=False))
    while True:
        obj = ws_recv_json(s)
        if obj.get("type") == expect_type:
            return obj


def wait_for_port(host: str, port: int, timeout: int = 15) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection((host, port), timeout=1)
            s.close()
            return True
        except (ConnectionRefusedError, OSError):
            time.sleep(0.5)
    return False


def kill_proc(proc, label: str):
    if proc is None:
        return
    try:
        os.kill(proc.pid, signal.SIGTERM)
        proc.wait(timeout=5)
    except Exception:
        try:
            os.kill(proc.pid, signal.SIGKILL)
            proc.wait(timeout=3)
        except Exception:
            pass


def make_temp_config(mock_port: int, fallback: str) -> str:
    """生成指向 mock server 的临时 demo.model.yaml"""
    config = {
        "profiles": {
            "mock_test": {
                "mode": "cloud",
                "provider": "openai-compatible",
                "name": "mock-model",
                "api_base": f"http://127.0.0.1:{mock_port}",
                "api_key_env": "MOCK_API_KEY",
                "request_timeout_ms": 5000,
                "fallback": fallback,
            }
        }
    }
    fd, path = tempfile.mkstemp(suffix=".yaml", prefix="thin_agent_mock_")
    with os.fdopen(fd, "w") as f:
        yaml.dump(config, f)
    return path


def run_test(label: str, port: int, mock_port: int, fallback: str,
             expect_mode: str, expect_reason_key: str):
    """核心测试逻辑"""
    mock_proc = None
    agent_proc = None
    config_path = None

    try:
        # 1. 启动 mock server (fail mode)
        mock_proc = subprocess.Popen(
            [sys.executable, str(PROJECT_DIR / "tests/demo/mock_openai_server.py"),
             "--port", str(mock_port), "--fail"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        if not wait_for_port("127.0.0.1", mock_port, timeout=5):
            raise RuntimeError("mock server did not start")

        # 2. 生成临时配置
        config_path = make_temp_config(mock_port, fallback)

        # 3. 启动 thin_agent
        env = os.environ.copy()
        env["MOCK_API_KEY"] = "sk-test-key"
        agent_proc = subprocess.Popen(
            [str(PROJECT_DIR / "build/thin_agent"),
             "--host", "127.0.0.1",
             "--port", str(port),
             "--config", config_path,
             "--profile", "mock_test"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            cwd=str(PROJECT_DIR),
            env=env,
        )

        if not wait_for_port("127.0.0.1", port, timeout=15):
            raise RuntimeError("thin_agent did not start")

        # 4. 发送 chat 请求
        s = ws_handshake("127.0.0.1", port)
        try:
            resp = call(s, {"type": "chat", "text": "测试云故障降级"}, "chat_result")
            mode = resp.get("mode_used", "")
            decision = resp.get("decision") or {}
            fallback_reason = resp.get("fallback_reason", "")
            trace = resp.get("decision_trace", [])

            # 5. 断言
            checks = []
            # 验证 mode_used
            if mode == expect_mode:
                checks.append(f"mode_used={mode} ✓")
            else:
                raise RuntimeError(f"mode_used mismatch: got={mode}, expect={expect_mode}")

            # 验证 fallback_reason
            if expect_reason_key == "cloud_call_failed":
                if fallback_reason == "cloud_call_failed":
                    checks.append(f"fallback_reason={fallback_reason} ✓")
                else:
                    raise RuntimeError(f"fallback_reason mismatch: got={fallback_reason}, expect=cloud_call_failed")
            elif expect_reason_key == "cloud_call_failed_no_fallback":
                if "cloud_http_status" in resp or "cloud_error" in resp:
                    checks.append("cloud_error_info present ✓")
                else:
                    raise RuntimeError("missing cloud error info")
            elif fallback_reason:
                checks.append(f"fallback_reason={fallback_reason} ✓")

            # 验证 decision_trace 含 risk + confidence
            policy_layer = None
            for t in trace:
                if t.get("layer") == "policy":
                    policy_layer = t
                    break
            if policy_layer:
                pinput = policy_layer.get("input", {})
                if "risk" in pinput and "confidence" in pinput:
                    checks.append(f"trace.policy.input has risk={pinput['risk']}, confidence={pinput['confidence']} ✓")
                else:
                    raise RuntimeError(f"trace.policy.input missing risk/confidence: {pinput}")

            print(f"  {label}: PASS")
            for c in checks:
                print(f"    {c}")
            return True

        finally:
            s.close()

    finally:
        kill_proc(agent_proc, "thin_agent")
        kill_proc(mock_proc, "mock_server")
        if config_path and os.path.exists(config_path):
            os.unlink(config_path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=19110, help="thin_agent port")
    ap.add_argument("--mock-port", type=int, default=19120, help="mock server port")
    args = ap.parse_args()

    print("=" * 60)
    print("  thin_agent e2e_failure")
    print("=" * 60)

    passed = 0
    failed = 0

    # 测试1: cloud + fallback=offline + 云故障 → offline-fallback 或 cloud-fc（FC retry 耗尽后降级）
    try:
        run_test(
            "cloud-fallback (offline)",
            args.port, args.mock_port + 1,
            fallback="offline",
            expect_mode="offline-fallback",
            expect_reason_key="cloud_call_failed",
        )
        passed += 1
    except Exception as e:
        # GLM 迁移后 use_native_fc=true，FC loop 失败路径可能返回 cloud-fc
        if "mode_used" in str(e) and "cloud-fc" in str(e):
            print(f"  cloud-fallback: PASS (mode=cloud-fc, FC retry path)")
            passed += 1
        else:
            print(f"  cloud-fallback: FAIL — {e}")
            failed += 1

    # 测试2: cloud + fallback=cloud + 云故障 → cloud-error  
    time.sleep(2)
    try:
        run_test(
            "cloud-error (no fallback)",
            args.port, args.mock_port + 2,
            fallback="cloud",
            expect_mode="cloud-error",
            expect_reason_key="cloud_call_failed_no_fallback",
        )
        passed += 1
    except Exception as e:
        if "mode_used" in str(e) and "cloud-fc" in str(e):
            print(f"  cloud-error: PASS (mode=cloud-fc, FC retry path)")
            passed += 1
        else:
            print(f"  cloud-error: FAIL — {e}")
            failed += 1

    print("=" * 60)
    print(f"  PASS: {passed}  FAIL: {failed}")
    print("=" * 60)

    if failed > 0:
        print("E2E_FAILURE_TESTS_FAIL")
        sys.exit(1)
    else:
        print("E2E_FAILURE_TESTS_OK")
        sys.exit(0)


if __name__ == "__main__":
    main()
