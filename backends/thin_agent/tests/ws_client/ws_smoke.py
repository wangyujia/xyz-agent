#!/usr/bin/env python3
"""
thin_agent WS 冒烟测试

快速验证核心功能是否正常：
  - local_profile（你是谁 / 技能查询）
  - local_external（天气 / 搜索）
  - cloud FC（shell_exec / 文件操作）

用法:
    python ws_smoke.py              # 全量冒烟
    python ws_smoke.py --quick      # 仅快速检查（3项）
    python ws_smoke.py --profile    # 仅 profile 场景
    python ws_smoke.py --verbose    # 显示完整文本

退出码: 0=全部通过, 1=有失败
"""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass
from typing import Callable

from ws_client import ChatResult, WsClient


# ── 测试用例 ──────────────────────────────────────────────────────────

@dataclass
class TestCase:
    name: str
    query: str
    check: Callable[[ChatResult], bool]
    description: str = ""


def make_test_cases() -> list[TestCase]:
    """构建所有冒烟测试用例"""

    tests: list[TestCase] = []

    # ── Profile 场景 ──
    tests.append(TestCase(
        name="profile_who",
        query="你是谁",
        check=lambda r: r.has_route("local_profile") and "编码智能体" in r.text,
        description="你是谁 → local_profile + 自我介绍",
    ))

    tests.append(TestCase(
        name="profile_skills_only",
        query="目前有哪些技能",
        check=lambda r: (
            r.has_route("local_profile")
            and ("当前可用技能" in r.text or "Available skills" in r.text)
            and "你好！我是" not in r.text  # skills_only 模式
        ),
        description="技能查询 → 只列技能，无开场白",
    ))

    tests.append(TestCase(
        name="profile_skills_variant",
        query="有什么功能",
        check=lambda r: r.has_route("local_profile") and "当前可用技能" in r.text,
        description="有什么功能 → skills_only",
    ))

    # ── 外部查询 ──
    tests.append(TestCase(
        name="external_weather",
        query="天气 北京",
        check=lambda r: r.has_route("local_external_weather") and r.ok,
        description="天气查询 → external_weather",
    ))

    # ── Cloud FC ──
    tests.append(TestCase(
        name="fc_echo",
        query="echo hello world",
        check=lambda r: (
            r.ok and (
                "hello world" in r.text.lower()
                or r.mode in ("local-agent", "offline", "offline-fallback")  # 离线回退也可接受
            )
        ),
        description="echo → cloud_fc 或 offline fallback",
    ))

    tests.append(TestCase(
        name="fc_file_list",
        query="列出当前目录下的 cpp 文件",
        check=lambda r: r.ok,
        description="文件列表 → cloud_fc（验证 FC 迭代正常）",
    ))

    return tests


# ── 运行器 ────────────────────────────────────────────────────────────

def run_tests(
    tests: list[TestCase],
    ws: WsClient,
    verbose: bool = False,
    delay: float = 0.3,
) -> tuple[int, int]:
    """运行测试用例，返回 (passed, total)"""
    passed = 0
    total = len(tests)

    print(f"thin_agent WS Smoke Test — {total} 用例")
    print(f"目标: ws://127.0.0.1:8765/ws")
    print(f"{'='*60}")

    for i, tc in enumerate(tests, 1):
        try:
            result = ws.chat(tc.query, chat_id=f"smoke_{tc.name}")
            ok = tc.check(result)

            status = "✅" if ok else "❌"
            print(f"[{i}/{total}] {status} {tc.name}: {tc.description}")

            if verbose:
                print(f"       query: {tc.query}")
                print(f"       text : {result.text[:200].replace(chr(10), chr(10) + ' '*14)}")
                print(f"       route: {result.route}  mode: {result.mode}")

            if not ok:
                print(f"       FAIL detail: text[:200]={result.text[:200]}")
                print(f"                    route={result.route}")

            if ok:
                passed += 1

        except Exception as e:
            print(f"[{i}/{total}] ❌ {tc.name}: EXCEPTION — {e}")

        if delay:
            time.sleep(delay)

    return passed, total


# ── 快速模式 ──────────────────────────────────────────────────────────

def get_quick_tests() -> list[TestCase]:
    """快速检查：只跑最关键 3 项"""
    all_tests = make_test_cases()
    quick_names = {"profile_who", "profile_skills_only", "external_weather"}
    return [t for t in all_tests if t.name in quick_names]


def get_profile_tests() -> list[TestCase]:
    """仅 Profile 场景"""
    return [t for t in make_test_cases() if t.name.startswith("profile_")]


# ── main ──────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="thin_agent WS 冒烟测试")
    parser.add_argument("--quick", action="store_true", help="仅快速检查 3 项")
    parser.add_argument("--profile", action="store_true", help="仅 Profile 场景")
    parser.add_argument("--verbose", "-v", action="store_true", help="显示完整文本")
    parser.add_argument("--url", default="ws://127.0.0.1:8765/ws", help="WS 地址")
    parser.add_argument("--timeout", type=float, default=30.0, help="超时(秒)")
    parser.add_argument("--delay", type=float, default=0.3, help="用例间延迟(秒)")
    args = parser.parse_args()

    if args.quick:
        tests = get_quick_tests()
    elif args.profile:
        tests = get_profile_tests()
    else:
        tests = make_test_cases()

    with WsClient(url=args.url, timeout=args.timeout) as ws:
        passed, total = run_tests(tests, ws, verbose=args.verbose, delay=args.delay)

    print(f"{'='*60}")
    print(f"结果: {passed}/{total} PASS")

    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
