#!/usr/bin/env python3
"""
thin_agent WS 交互式调试终端

用法:
    python ws_repl.py                    # 默认 ws://127.0.0.1:8765/ws
    python ws_repl.py --url ws://...     # 指定地址
    python ws_repl.py --raw              # 显示原始 JSON（含 thinking 帧）

命令:
    /raw         切换原始 JSON 模式
    /verbose     切换详细模式（完整 text）
    /help        显示帮助
    /q, /quit    退出

快捷键:
    Ctrl+C       退出
"""

from __future__ import annotations

import argparse
import json
import readline  # noqa: F401 — 启用行编辑
import sys

from ws_client import ChatResult, WsClient


# ── REPL ──────────────────────────────────────────────────────────────

class WsRepl:
    def __init__(self, url: str = "ws://127.0.0.1:8765/ws", timeout: float = 30.0):
        self.url = url
        self.timeout = timeout
        self.ws = WsClient(url=url, timeout=timeout)
        self.raw_mode = False
        self.verbose = False

    def run(self):
        self.ws.connect()
        print(f"╔══════════════════════════════════════════════════════╗")
        print(f"║  thin_agent WS REPL                                 ║")
        print(f"║  地址: {self.url:<43} ║")
        print(f"║  输入消息发送，/help 查看命令，Ctrl+C 退出          ║")
        print(f"╚══════════════════════════════════════════════════════╝")

        try:
            while True:
                try:
                    line = input("\n▸ ").strip()
                except EOFError:
                    break

                if not line:
                    continue

                if line.startswith("/"):
                    if self._handle_command(line):
                        break
                    continue

                self._handle_chat(line)

        except KeyboardInterrupt:
            print("\n")
        finally:
            self.ws.close()
            print("已断开。")

    def _handle_command(self, cmd: str) -> bool:
        """处理 / 命令，返回 True 表示退出"""
        parts = cmd.split(maxsplit=1)
        name = parts[0].lower()

        if name in ("/q", "/quit", "/exit"):
            return True

        if name == "/raw":
            self.raw_mode = not self.raw_mode
            print(f"  原始 JSON 模式: {'ON' if self.raw_mode else 'OFF'}")
            return False

        if name == "/verbose":
            self.verbose = not self.verbose
            print(f"  详细模式: {'ON' if self.verbose else 'OFF'}")
            return False

        if name == "/help":
            print("  命令:")
            print("    /raw        切换原始 JSON 模式（显示所有帧）")
            print("    /verbose    切换详细模式（显示完整 text）")
            print("    /help       显示此帮助")
            print("    /q, /quit   退出")
            print("")
            print("  直接输入文本即发送 chat 消息")
            return False

        # 未知命令 → 当作普通消息发送
        self._handle_chat(cmd)
        return False

    def _handle_chat(self, text: str):
        if self.raw_mode:
            frames = self.ws.recv_all(text)
            for i, f in enumerate(frames):
                t = f.get("type", "?")
                if t == "chat_result":
                    txt = f.get("text", "")
                    print(f"  [{i}] {t} text={txt[:200]}")
                elif t == "thinking":
                    # v0.53.45: 真思考流(content/live)与旧进度文案(tier/msg)双兼容
                    content = f.get("content", "")
                    if content:
                        print(f"  [{i}] {t} live={f.get('live','?')} {content[:80]}")
                    else:
                        print(f"  [{i}] {t} tier={f.get('tier','?')} msg={f.get('msg','')[:80]}")
                else:
                    print(f"  [{i}] {t} {json.dumps(f, ensure_ascii=False)[:200]}")
        else:
            result = self.ws.chat(text)
            if self.verbose:
                print(f"  route: {result.route}")
                print(f"  mode:  {result.mode}")
                print(f"  text:\n{result.text}")
                if result.tool_calls:
                    print(f"  tools: {len(result.tool_calls)} calls")
            else:
                print(f"  {result.summary(max_len=300)}")


# ── main ──────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="thin_agent WS 交互式调试终端")
    parser.add_argument("--url", default="ws://127.0.0.1:8765/ws", help="WS 地址")
    parser.add_argument("--timeout", type=float, default=30.0, help="超时(秒)")
    parser.add_argument("--raw", action="store_true", help="启动时即开启原始 JSON 模式")
    args = parser.parse_args()

    repl = WsRepl(url=args.url, timeout=args.timeout)
    if args.raw:
        repl.raw_mode = True
    repl.run()


if __name__ == "__main__":
    main()
