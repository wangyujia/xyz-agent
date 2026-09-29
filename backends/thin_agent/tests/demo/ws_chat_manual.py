#!/usr/bin/env python3
"""Manual WebSocket chat smoke against a running thin_agent ws server."""
import argparse
import base64
import json
import os
import socket
import sys


def ws_handshake(host: str, port: int, path: str):
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
        chunk = s.recv(ln - len(data))
        if not chunk:
            raise RuntimeError("socket closed mid-frame")
        data += chunk
    return json.loads(data.decode("utf-8", errors="replace"))


def chat(s: socket.socket, seq: int, text: str):
    seq += 1
    req = {
        "type": "chat",
        "text": text,
        "cmd_id": f"manual-cmd-{seq:03d}",
        "trace_id": "manual-trace-001",
    }
    ws_send_text(s, json.dumps(req, ensure_ascii=False))
    while True:
        obj = ws_recv_json(s)
        t = obj.get("type")
        if t == "chat_result":
            return seq, obj
        if t == "error":
            raise RuntimeError(f"server error: {obj}")


def summarize(label: str, text: str, obj: dict):
    d = obj.get("decision", {})
    route = d.get("route", "?")
    intent = d.get("intent", "?")
    policy = d.get("policy", "?")
    conf = d.get("confidence", "?")
    backend = obj.get("intent_backend", "?")
    mode = obj.get("mode_used", "?")
    reply = (obj.get("text") or "").replace("\n", " ")
    slots = d.get("slots", {})
    print(f"\n=== {label} ===")
    print(f"输入: {text}")
    print(f"route={route} intent={intent} policy={policy} conf={conf}")
    print(f"intent_backend={backend} mode_used={mode}")
    if slots:
        print(f"slots={json.dumps(slots, ensure_ascii=False)}")
    print(f"reply: {reply[:160]}{'...' if len(reply) > 160 else ''}")
    return route, intent


def drain_hello(s: socket.socket):
    s.settimeout(2)
    try:
        hello = ws_recv_json(s)
        ver = hello.get("version") or hello.get("session_id") or hello.get("type")
        print(f"connected: {hello.get('type')} {ver}")
    except Exception:
        pass
    s.settimeout(30)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--path", default="/ws")
    args = ap.parse_args()

    results = []
    seq = 0
    s = ws_handshake(args.host, args.port, args.path)
    try:
        drain_hello(s)

        singles = [
            ("profile: who r u", "who r u", "local_profile", "profile"),
            ("profile: 你有哪些能力", "你有哪些能力", "local_profile", "profile"),
            ("profile: who are you", "who are you", "local_profile", "profile"),
            ("weather clarify: 查天气", "查天气", "local_external_clarify", "weather"),
            ("today weather clarify: 今天天气如何？", "今天天气如何？", "local_external_clarify", "weather"),
            ("status: 什么模型", "什么模型", "local_status", "status"),
        ]
        for label, text, exp_route, exp_intent in singles:
            seq, obj = chat(s, seq, text)
            route, intent = summarize(label, text, obj)
            ok = route == exp_route and intent == exp_intent
            results.append((label, ok, route, intent, exp_route, exp_intent))

        print("\n--- 多轮天气 (同一会话) ---")
        mt = [
            ("mt1", "今天天气如何？"),
            ("mt2", "上海"),
            ("mt3", "深圳的呢"),
            ("mt4", "需要"),
        ]
        for label, text in mt:
            seq, obj = chat(s, seq, text)
            route, intent = summarize(label, text, obj)
            results.append((label, True, route, intent, None, None))

        print("\n--- 槽位切换：查天气后看新闻 (同一会话) ---")
        seq, obj = chat(s, seq, "查天气")
        summarize("slot: 查天气", "查天气", obj)
        seq, obj = chat(s, seq, "看新闻")
        route, intent = summarize("slot: 看新闻", "看新闻", obj)
        ok = intent == "news" and "城市" not in (obj.get("text") or "")
        results.append(("slot_news_after_weather", ok, route, intent, "local_external_clarify", "news"))

        s.close()
        s = ws_handshake(args.host, args.port, args.path)
        drain_hello(s)
        seq, obj = chat(s, 0, "How's the weather of Shanghai")
        route, intent = summarize("english weather", "How's the weather of Shanghai", obj)
        ok = intent == "weather" and route in ("local_external_weather", "local_external_clarify")
        results.append(("en_weather", ok, route, intent, "local_external_weather|clarify", "weather"))
    finally:
        s.close()

    print("\n========== 汇总 ==========")
    pass_n = fail_n = 0
    for label, ok, route, intent, exp_route, exp_intent in results:
        if exp_route is not None:
            status = "PASS" if ok else "FAIL"
            if ok:
                pass_n += 1
            else:
                fail_n += 1
            print(f"[{status}] {label}: route={route} intent={intent} (期望 route={exp_route} intent={exp_intent})")
        else:
            print(f"[INFO] {label}: route={route} intent={intent}")

    print(f"\n断言通过: {pass_n}/{pass_n + fail_n}")
    if fail_n:
        sys.exit(1)


if __name__ == "__main__":
    main()
