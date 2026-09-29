#!/usr/bin/env python3
import argparse
import base64
import json
import os
import socket
import sys


def ws_handshake(host: str, port: int, path: str):
    s = socket.create_connection((host, port), timeout=8)
    s.settimeout(60)  # GLM reasoning model 冷启动可能需要更长超时
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--path", default="/ws")
    ap.add_argument("--text", default="请返回一句云端验证文本")
    ap.add_argument("--expect-mode", default="cloud")
    ap.add_argument("--expect-mode-alt", default="")
    ap.add_argument("--expect-substr", default="[端云协同-本地裁决]")
    ap.add_argument("--expect-substr-alt", default="")
    ap.add_argument("--expect-route", default="")
    ap.add_argument("--expect-policy", default="")
    ap.add_argument("--expect-intent-backend", default="")
    ap.add_argument("--expect-intent-backend-alt", default="")
    ap.add_argument("--expect-cloud-policy", action="store_true")
    args = ap.parse_args()

    s = ws_handshake(args.host, args.port, args.path)
    try:
        resp = call(s, {"type": "chat", "text": args.text}, "chat_result")
        mode = resp.get("mode_used")
        text = resp.get("text", "")
        decision = resp.get("decision") or {}
        if mode != args.expect_mode and (not args.expect_mode_alt or mode != args.expect_mode_alt):
            raise RuntimeError(f"mode_used mismatch: got={mode}, expect={args.expect_mode}, resp={resp}")
        if args.expect_substr and args.expect_substr not in text:
            alt = getattr(args, 'expect_substr_alt', '')
            if not alt or alt not in text:
                raise RuntimeError(f"text missing expect_substr={args.expect_substr}, text={text}")
        if args.expect_route and decision.get("route") != args.expect_route:
            raise RuntimeError(f"route mismatch: got={decision.get('route')}, expect={args.expect_route}, resp={resp}")
        if args.expect_policy and decision.get("policy") != args.expect_policy:
            raise RuntimeError(f"policy mismatch: got={decision.get('policy')}, expect={args.expect_policy}, resp={resp}")
        if args.expect_intent_backend and resp.get("intent_backend") != args.expect_intent_backend \
                and (not args.expect_intent_backend_alt or resp.get("intent_backend") != args.expect_intent_backend_alt):
            raise RuntimeError(f"intent_backend mismatch: got={resp.get('intent_backend')}, expect={args.expect_intent_backend}, resp={resp}")
        if args.expect_cloud_policy:
            cp = (resp.get("observation") or {}).get("cloud_policy")
            if not isinstance(cp, dict):
                raise RuntimeError(f"missing observation.cloud_policy: {resp}")
        if mode == "cloud" and int(resp.get("cloud_http_status", 0)) != 200:
            raise RuntimeError(f"cloud_http_status not 200: {resp}")

        print("E2E_CLOUD_OK")
        print("mode_used", mode)
        print("intent_backend", resp.get("intent_backend"))
        print("route", decision.get("route"))
        print("policy", decision.get("policy"))
        print("cloud_http_status", resp.get("cloud_http_status"))
        print("text", text)
    finally:
        s.close()


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"E2E_CLOUD_FAIL: {e}")
        sys.exit(1)
