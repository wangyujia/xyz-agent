#!/usr/bin/env python3
import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    server_version = "thin-agent-mock-openai/0.1"
    fail_mode: bool = False  # class-level: set to True to simulate cloud failure

    def log_message(self, format, *args):
        return

    def _send_json(self, code: int, payload: dict):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if Handler.fail_mode:
            self._send_json(500, {"error": {"message": "simulated cloud failure", "type": "server_error"}})
            return

        if self.path not in ("/v1/chat/completions", "/chat/completions"):
            self._send_json(404, {"error": "not_found"})
            return

        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer "):
            self._send_json(401, {"error": {"message": "missing bearer token"}})
            return

        n = int(self.headers.get("Content-Length", "0") or "0")
        raw = self.rfile.read(n) if n > 0 else b"{}"
        try:
            req = json.loads(raw.decode("utf-8", errors="replace"))
        except Exception:
            self._send_json(400, {"error": {"message": "bad json"}})
            return

        model = req.get("model", "")
        messages = req.get("messages", [])
        user_text = ""
        if isinstance(messages, list):
            for m in reversed(messages):
                if isinstance(m, dict) and m.get("role") == "user":
                    user_text = str(m.get("content", ""))
                    break

        # v0.39.2: system prompt capture for integration testing
        sys_prompt_file = getattr(Handler, "capture_sys_prompt_file", None)
        if sys_prompt_file and isinstance(messages, list) and len(messages) > 0:
            sys_msg = messages[0]
            if isinstance(sys_msg, dict) and sys_msg.get("role") == "system":
                sp = str(sys_msg.get("content", ""))
                with open(sys_prompt_file, "w") as f:
                    f.write(sp)

        if "route-s-demo" in user_text:
            content = json.dumps({
                "strategy": "answer_direct",
                "intent": "status",
                "confidence": 0.91,
                "local_route_hint": "local_status",
                "response_draft": "建议走本地状态查询",
                "risk": "low",
                "reason": "mock_policy_local_status"
            }, ensure_ascii=False)
        elif "task-capture-demo" in user_text:
            content = json.dumps({
                "strategy": "answer_direct",
                "intent": "task_capture",
                "confidence": 0.88,
                "local_route_hint": "local_task_inline_capture_photo",
                "response_draft": "建议本地执行拍照任务",
                "risk": "low",
                "reason": "mock_policy_local_task_capture"
            }, ensure_ascii=False)
        elif "task-record-demo" in user_text:
            content = json.dumps({
                "strategy": "answer_direct",
                "intent": "task_start_recording",
                "confidence": 0.86,
                "local_route_hint": "local_task_inline_start_recording",
                "response_draft": "建议本地执行开始录制任务",
                "risk": "low",
                "reason": "mock_policy_local_task_recording"
            }, ensure_ascii=False)
        elif "reject-high-risk-demo" in user_text:
            content = json.dumps({
                "strategy": "reject",
                "intent": "dangerous_action",
                "confidence": 0.95,
                "response_draft": "该请求属于高风险敏感操作",
                "risk": "high",
                "reason": "mock_policy_high_risk_reject"
            }, ensure_ascii=False)
        elif "need-c-demo" in user_text or "clarify" in user_text.lower():
            content = json.dumps({
                "strategy": "clarify",
                "intent": "general_query",
                "confidence": 0.66,
                "need_clarify": True,
                "clarify_question": "你更关注设备状态、能力介绍，还是要我执行具体动作？",
                "response_draft": "请先澄清你的目标",
                "risk": "medium",
                "reason": "mock_policy_clarify"
            }, ensure_ascii=False)
        else:
            content = json.dumps({
                "strategy": "answer_direct",
                "intent": "general_query",
                "confidence": 0.73,
                "response_draft": f"MOCK_CLOUD_OK model={model} echo={user_text}",
                "risk": "medium",
                "reason": "mock_policy_direct"
            }, ensure_ascii=False)

        payload = {
            "id": "chatcmpl-mock-1",
            "object": "chat.completion",
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": content,
                    },
                    "finish_reason": "stop",
                }
            ],
        }
        self._send_json(200, payload)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=18080)
    ap.add_argument("--fail", action="store_true", help="Simulate cloud failure (return 500)")
    ap.add_argument("--capture-sys-prompt", metavar="FILE",
                    help="Capture system prompt from first request to FILE")
    args = ap.parse_args()

    if args.fail:
        Handler.fail_mode = True
    if args.capture_sys_prompt:
        Handler.capture_sys_prompt_file = args.capture_sys_prompt

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    mode_str = "FAIL" if Handler.fail_mode else "OK"
    print(f"MOCK_OPENAI_LISTEN {args.host}:{args.port} mode={mode_str}", flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
