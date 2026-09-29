#!/usr/bin/env python3
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
        data += s.recv(ln - len(data))
    return json.loads(data.decode("utf-8", errors="replace"))


def call(s, seq, req, expect_type):
    seq += 1
    req["cmd_id"] = f"e2e-cmd-{seq:03d}"
    req["trace_id"] = "e2e-trace-001"
    ws_send_text(s, json.dumps(req, ensure_ascii=False))
    while True:
      obj = ws_recv_json(s)
      if obj.get("type") == expect_type:
          return seq, obj


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--path", default="/ws")
    args = ap.parse_args()

    s = ws_handshake(args.host, args.port, args.path)
    seq = 0

    try:
        seq, sub = call(
            s,
            seq,
            {
                "type": "task_submit",
                "action": "not_allowed_action",
                "args": {},
                "idempotency_key": "e2e-idem-fail-1",
            },
            "task_submit_result",
        )
        tid = sub["task"]["task_id"]
        assert sub["task"]["state"] == "failed", sub
        assert int(sub["task"]["attempts"]) >= 1, sub

        seq, aud = call(
            s,
            seq,
            {"type": "task_audit", "task_id": tid, "limit": 20},
            "task_audit_result",
        )
        audits = aud["data"]["audits"]
        assert isinstance(audits, list) and len(audits) >= 2, aud

        seq, rep = call(
            s,
            seq,
            {"type": "task_replay", "task_id": tid, "idempotency_key": "e2e-idem-replay-1"},
            "task_replay_result",
        )
        new_tid = rep["data"]["replayed_task_id"]
        assert new_tid != tid, rep

        seq, can = call(
            s,
            seq,
            {"type": "task_cancel", "task_id": new_tid, "reason": "e2e stop"},
            "task_cancel_result",
        )
        assert can["task"]["exists"] is True, can

        seq, mh = call(
            s,
            seq,
            {"type": "memory_history", "limit": 10},
            "memory_history_result",
        )
        assert int(mh["history_size"]) >= 0, mh

        seq, lstf = call(
            s,
            seq,
            {"type": "task_list", "limit": 20, "state": "failed", "action": "not_allowed_action"},
            "task_list_result",
        )
        assert lstf["filters"]["state"] == "failed", lstf
        assert lstf["filters"]["action"] == "not_allowed_action", lstf
        assert int(lstf["limit_applied"]) == 20, lstf
        assert isinstance(lstf["tasks"], list), lstf
        assert int(lstf["returned_count"]) == len(lstf["tasks"]), lstf

        seq, met = call(
            s,
            seq,
            {"type": "metrics"},
            "metrics_result",
        )
        assert int(met["data"]["total_requests"]) >= 1, met
        assert float(met["data"]["avg_latency_ms"]) >= 0.0, met

        print("E2E_OK")
        print("task_id_failed", tid)
        print("audit_count", len(audits))
        print("task_id_replayed", new_tid)
    finally:
        s.close()


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"E2E_FAIL: {e}")
        sys.exit(1)
