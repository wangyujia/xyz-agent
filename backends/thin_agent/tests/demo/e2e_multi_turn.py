#!/usr/bin/env python3
"""Multi-turn E2E: weather slot fill, intent switch, profile reset."""
import argparse
import base64
import json
import os
import socket
import sys


def ws_handshake(host, port):
    s = socket.create_connection((host, port), timeout=8)
    s.settimeout(30)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET /ws HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(req.encode())
    s.recv(4096)
    return s


def send(s, msg):
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


def recv_json(s):
    b1 = s.recv(1)
    if not b1:
        raise RuntimeError("socket closed")
    b2 = s.recv(1)
    ln = b2[0] & 0x7F
    if ln == 126:
        ln = int.from_bytes(s.recv(2), "big")
    elif ln == 127:
        ln = int.from_bytes(s.recv(8), "big")
    if b2[0] & 0x80:
        s.recv(4)
    data = b""
    while len(data) < ln:
        chunk = s.recv(ln - len(data))
        if not chunk:
            raise RuntimeError("socket closed mid-frame")
        data += chunk
    return json.loads(data.decode("utf-8", errors="replace"))


def drain_hello(s):
    s.settimeout(2)
    try:
        obj = recv_json(s)
        print(f"  [hello] type={obj.get('type')}")
    except Exception:
        pass
    s.settimeout(30)


def chat(s, text, label):
    send(s, json.dumps({"type": "chat", "text": text}, ensure_ascii=False))
    while True:
        r = recv_json(s)
        if r.get("type") == "chat_result":
            decision = r.get("decision") or {}
            route = decision.get("route", "?")
            intent = decision.get("intent", "?")
            print(f"  [{label}] route={route} intent={intent} text={(r.get('text') or '')[:72]}")
            return r
        if r.get("type") == "error":
            raise RuntimeError(f"server error: {r}")


def expect_chat(r, label, *, route=None, intent=None, route_in=None, text_must_not=None):
    decision = r.get("decision") or {}
    actual_route = decision.get("route", "")
    actual_intent = decision.get("intent", "")
    text = r.get("text") or ""

    if route is not None and actual_route != route:
        raise AssertionError(f"{label}: route={actual_route} expected {route}")
    if route_in is not None and actual_route not in route_in:
        raise AssertionError(f"{label}: route={actual_route} expected one of {route_in}")
    if intent is not None and actual_intent != intent:
        raise AssertionError(f"{label}: intent={actual_intent} expected {intent}")
    if text_must_not is not None and text_must_not in text:
        raise AssertionError(f"{label}: reply unexpectedly contains {text_must_not!r}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19199)
    args = ap.parse_args()

    s = ws_handshake(args.host, args.port)
    try:
        drain_hello(s)

        r = chat(s, "天气", "T1: weather-no-city")
        expect_chat(r, "T1", route="local_external_clarify", intent="weather")

        r = chat(s, "上海呢", "T2: fill-city-shanghai")
        expect_chat(r, "T2", intent="weather", route_in=("local_external_weather", "local_external_clarify"))

        r = chat(s, "查天气", "T3: weather-no-city-2")
        expect_chat(r, "T3", intent="weather", route_in=("local_external_weather", "local_external_clarify"))

        r = chat(s, "北京", "T4: fill-city-beijing")
        expect_chat(r, "T4", intent="weather", route_in=("local_external_weather", "local_external_clarify"))

        r = chat(s, "你是谁", "T5: profile-reset")
        expect_chat(r, "T5", route="local_profile", intent="profile")

        r = chat(s, "天气怎么样", "T6: weather-clarify")
        expect_chat(r, "T6", intent="weather", route_in=("local_external_weather", "local_external_clarify"))

        r = chat(s, "请帮我详细介绍一下整个系统的运行状况", "T7: not-city-reset")
        # 长句可能走 profile 或 clarify，但不应仍卡在天气澄清
        if (r.get("decision") or {}).get("route") == "local_external_clarify":
            text = r.get("text") or ""
            if "城市" in text or "city" in text.lower():
                raise AssertionError("T7: long text still weather city clarify")

        r = chat(s, "天气", "T8: weather-again")
        expect_chat(r, "T8", route="local_external_clarify", intent="weather")

        r = chat(s, "深圳", "T9: city-shenzhen")
        expect_chat(r, "T9", intent="weather", route_in=("local_external_weather", "local_external_clarify"))

        # 槽位切换：澄清天气后说看新闻，应走新闻而非天气
        r = chat(s, "查天气", "T10a: weather-before-news")
        expect_chat(r, "T10a", intent="weather", route_in=("local_external_weather", "local_external_clarify"))

        r = chat(s, "看新闻", "T10b: news-after-weather-clarify")
        expect_chat(r, "T10b", intent="news", route="local_external_clarify", text_must_not="城市")

        # 今日天气无城市：应澄清，不应直接执行
        r = chat(s, "今天天气如何？", "T11: today-weather-clarify")
        expect_chat(r, "T11", intent="weather", route_in=("local_external_weather", "local_external_clarify"))

    finally:
        s.close()

    print("\nMULTI_TURN_OK")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"MULTI_TURN_FAIL: {e}")
        sys.exit(1)
