#!/usr/bin/env python3
"""
thin_agent WebSocket 测试客户端库

轻量级 WS 客户端封装，支持：
- 自动握手（跳过 hello 帧）
- 同步 chat 请求/响应（等待 chat_result）
- 超时控制
- 结果格式化输出

用法:
    from ws_client import WsClient

    with WsClient() as ws:
        result = ws.chat("你是谁")
        print(f"route={result.route} text={result.text[:100]}")

        # 断言模式
        ws.assert_route("你是谁", "local_profile")
        ws.assert_text_contains("你是谁", "编码智能体")
"""

from __future__ import annotations

import json
import sys
import time
from dataclasses import dataclass, field
from typing import Optional

import websocket


# ── 数据模型 ──────────────────────────────────────────────────────────

@dataclass
class ChatResult:
    """chat 请求的完整返回"""
    raw: dict
    text: str = ""
    route: str = ""
    mode: str = ""
    profile_mode: str = ""
    tool_calls: list = field(default_factory=list)

    @classmethod
    def from_raw(cls, data: dict) -> "ChatResult":
        decision = data.get("decision", {})
        observation = data.get("observation", {})
        return cls(
            raw=data,
            text=data.get("text", ""),
            route=decision.get("route", ""),
            mode=data.get("mode_used", ""),
            profile_mode=observation.get("profile_mode", ""),
            tool_calls=data.get("tool_calls", []),
        )

    @property
    def is_local(self) -> bool:
        return self.mode in ("local-agent", "offline", "offline-fallback")

    @property
    def is_cloud(self) -> bool:
        return "cloud" in self.mode or self.mode in ("chat_result",)

    @property
    def ok(self) -> bool:
        return bool(self.text)

    def contains(self, substr: str) -> bool:
        return substr in self.text

    def has_route(self, route: str) -> bool:
        return self.route == route

    def summary(self, max_len: int = 120) -> str:
        """单行摘要"""
        text_preview = self.text[:max_len].replace("\n", "\\n")
        return f"route={self.route} mode={self.mode} text={text_preview}"


# ── WS 客户端 ─────────────────────────────────────────────────────────

class WsClient:
    """thin_agent WebSocket 客户端"""

    def __init__(self, url: str = "ws://127.0.0.1:8765/ws", timeout: float = 30.0):
        self.url = url
        self.timeout = timeout
        self._ws: Optional[websocket.WebSocket] = None
        self._skipped_handshake = False
        # cloud 模式 URL 自动增大超时（GLM FC 多轮调用需要更长）
        if "cloud" in url or ":19" in url:
            self.timeout = max(self.timeout, 120.0)

    # ── 上下文管理 ──

    def __enter__(self) -> "WsClient":
        self.connect()
        return self

    def __exit__(self, *args):
        self.close()

    def connect(self):
        """建立连接并跳过握手帧"""
        self._ws = websocket.create_connection(self.url, timeout=self.timeout)
        self._ws.settimeout(self.timeout)
        # 跳过 hello 帧
        self._ws.recv()
        self._skipped_handshake = True

    def close(self):
        if self._ws:
            self._ws.close()
            self._ws = None

    # ── 核心 API ──

    def chat(self, text: str, chat_id: str = "test") -> ChatResult:
        """发送 chat 消息，等待 chat_result 返回"""
        self._send({"type": "chat", "text": text, "chat_id": chat_id})
        return self._recv_chat_result()

    def raw_chat(self, text: str, chat_id: str = "test") -> dict:
        """发送 chat 消息，返回原始 JSON（不解析为 ChatResult）"""
        self._send({"type": "chat", "text": text, "chat_id": chat_id})
        for _ in range(5000):  # v0.53.49: 流式 chunk 数百帧,真超时靠 socket
            frame = self._recv_raw()
            if frame.get("type") == "chat_result":
                return frame
        raise TimeoutError("chat_result not received after 50 frames")

    def recv_all(self, text: str, chat_id: str = "test") -> list[dict]:
        """发送 chat 消息，收集所有中间帧 + chat_result"""
        self._send({"type": "chat", "text": text, "chat_id": chat_id})
        frames = []
        for _ in range(5000):  # v0.53.49: 同上
            frame = self._recv_raw()
            frames.append(frame)
            if frame.get("type") == "chat_result":
                break
        return frames

    # ── 断言辅助 ──

    def assert_route(self, text: str, expected_route: str, chat_id: str = "test") -> ChatResult:
        """断言路由并返回结果"""
        result = self.chat(text, chat_id)
        assert result.route == expected_route, \
            f"Expected route={expected_route}, got route={result.route}\n  text={result.summary()}"
        return result

    def assert_text_contains(
        self, text: str, expected: str, chat_id: str = "test"
    ) -> ChatResult:
        """断言返回文本包含指定字符串"""
        result = self.chat(text, chat_id)
        assert expected in result.text, \
            f"Expected text to contain '{expected}', got:\n  {result.summary()}"
        return result

    def assert_text_not_contains(
        self, text: str, unexpected: str, chat_id: str = "test"
    ) -> ChatResult:
        """断言返回文本不包含指定字符串"""
        result = self.chat(text, chat_id)
        assert unexpected not in result.text, \
            f"Expected text NOT to contain '{unexpected}', got:\n  {result.summary()}"
        return result

    def assert_ok(self, text: str, chat_id: str = "test") -> ChatResult:
        """断言返回非空"""
        result = self.chat(text, chat_id)
        assert result.ok, f"Expected non-empty text\n  {result.summary()}"
        return result

    # ── 内部 ──

    def _send(self, data: dict):
        assert self._ws, "Not connected"
        self._ws.send(json.dumps(data, ensure_ascii=False))

    def _recv_raw(self) -> dict:
        assert self._ws, "Not connected"
        return json.loads(self._ws.recv())

    def _recv_chat_result(self, max_frames: int = 5000) -> ChatResult:
        # v0.53.49: 50→5000——真流式一回复数百 chunk 帧,50 帧必假超时;
        /// 真超时由 socket timeout(self.timeout)兜底,帧数只防无限循环
        for _ in range(max_frames):
            frame = self._recv_raw()
            if frame.get("type") == "chat_result":
                return ChatResult.from_raw(frame)
        raise TimeoutError(f"chat_result not received after {max_frames} frames")


# ── 命令行工具 ───────────────────────────────────────────────────────

def main():
    """命令行入口：ws_client.py <text>"""
    if len(sys.argv) < 2:
        print("Usage: python ws_client.py <message>")
        print("       python ws_client.py --repl")
        sys.exit(1)

    if sys.argv[1] == "--repl":
        repl_mode()
        return

    text = " ".join(sys.argv[1:])
    with WsClient() as ws:
        result = ws.chat(text)
        print(result.summary(max_len=500))


def repl_mode():
    """交互式调试终端"""
    print("thin_agent WS REPL — 输入消息，Ctrl+C 退出")
    print(f"连接: ws://127.0.0.1:8765/ws")
    ws = WsClient()
    ws.connect()
    try:
        while True:
            text = input("\n> ").strip()
            if not text:
                continue
            if text in ("/q", "/quit", "/exit"):
                break
            result = ws.chat(text)
            print(result.summary(max_len=500))
    except KeyboardInterrupt:
        print()
    finally:
        ws.close()


if __name__ == "__main__":
    main()
