#!/usr/bin/env python3
"""
Smoke check for ws_agent readable trace block (v0.6.42+).
Ensures memory_search/memory_summary query+limit are surfaced in non-JSON trace lines.
"""

from pathlib import Path
import sys


def main() -> int:
    p = Path(__file__).resolve().parents[2] / "ws_agent.html"
    s = p.read_text(encoding="utf-8")

    required = [
        "function extractMemoryQueryLimitFromTrace(trace = [])",
        "function extractMemoryLatestBasisFromObservation(observation = null)",
        "function buildMemoryReplayPayloadFromLatest(observation = null, fallbackType = 'memory_search')",
        "let pendingMemoryReplayRowRef = '';",
        "function detectMemoryReplayHit(observation = null, replayRowRef = '')",
        "function extractMemoryEvidenceRows(observation = null)",
        "function flashEvidenceRow(container, rowRef = '')",
        "function scrollToEvidenceRow(container, rowRef = '')",
        "const msgType = Object.prototype.hasOwnProperty.call(obj, 'type') ? String(obj.type || '').trim() : '';",
        "if (msgType === 'memory_summary_result' || msgType === 'memory_search_result') {",
        "const query = (typeof observation.query === 'string') ? observation.query.trim() : '';",
        "if (query && query === target) return target;",
        "if (msgType === 'memory_summary_result' || msgType === 'memory_search_result') {",
        "const replayHit = detectMemoryReplayHit(obj, pendingMemoryReplayRowRef);",
        "const traceLines = [];",
        "traceLines.push(`[memory] query=${q} · limit=${l}`);",
        "if (replayHit) traceLines.unshift(`[memory-replay] hit row_ref=${replayHit}`);",
        "replayHitRowRef: replayHit,",
        "pendingMemoryReplayRowRef = '';",
        "const evidenceRows = extractMemoryEvidenceRows(obj.observation);",
        "evidenceRows,",
        "row.dataset.rowRef = ev.rowRef;",
        "if (replayHitRowRef) scrollToEvidenceRow(wrap, replayHitRowRef);",
        "actions.unshift({ label: '回放该条记忆', payload: memoryReplayPayload, replayRowRef });", 
    ]

    missing = [x for x in required if x not in s]
    if missing:
        print("SMOKE_FAIL: missing required hooks")
        for m in missing:
            print("-", m)
        return 1

    print("SMOKE_OK: ws readable memory trace hooks present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
