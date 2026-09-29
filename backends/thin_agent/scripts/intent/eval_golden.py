#!/usr/bin/env python3
"""Evaluate intent_multiclass.onnx on models/intent/golden_eval.jsonl (holdout set)."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

try:
    import onnxruntime as ort
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "onnxruntime not installed. Use: .venv/bin/python scripts/intent/eval_golden.py"
    ) from exc

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "scripts" / "intent") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts" / "intent"))

from intent_features import extract_features  # noqa: E402

GOLDEN = ROOT / "models/intent/golden_eval.jsonl"
ONNX_PATH = ROOT / "models/intent/intent_multiclass.onnx"
LABELS_PATH = ROOT / "models/intent/labels.json"


def load_labels() -> list[str]:
    data = json.loads(LABELS_PATH.read_text(encoding="utf-8"))
    return list(data["labels"])


def load_golden(path: Path) -> list[dict]:
    rows: list[dict] = []
    with path.open(encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rows.append(json.loads(line))
    return rows


def softmax(logits: np.ndarray) -> np.ndarray:
    shifted = logits - logits.max()
    exp = np.exp(shifted)
    return exp / exp.sum()


def predict(session: ort.InferenceSession, labels: list[str], row: dict) -> tuple[str, float]:
    feat = extract_features(
        row["text"],
        row.get("last_intent", ""),
        row.get("last_slots", {}),
    ).reshape(1, -1)
    logits = session.run(None, {"features": feat.astype(np.float32)})[0][0]
    idx = int(np.argmax(logits))
    conf = float(softmax(logits)[idx])
    return labels[idx], conf


def main() -> int:
    parser = argparse.ArgumentParser(description="Evaluate ONNX intent model on golden_eval.jsonl")
    parser.add_argument("--golden", type=Path, default=GOLDEN)
    parser.add_argument("--model", type=Path, default=ONNX_PATH)
    parser.add_argument("--min-pass-rate", type=float, default=0.80, help="exit 1 if accuracy below this")
    args = parser.parse_args()

    if not args.golden.exists():
        print(f"ERROR: golden file missing: {args.golden}", file=sys.stderr)
        return 2
    if not args.model.exists():
        print(f"ERROR: onnx model missing: {args.model}", file=sys.stderr)
        print("Run: python scripts/intent/export_intent_onnx.py", file=sys.stderr)
        return 2

    labels = load_labels()
    rows = load_golden(args.golden)
    session = ort.InferenceSession(str(args.model), providers=["CPUExecutionProvider"])

    ok = 0
    fails: list[str] = []
    for row in rows:
        pred, conf = predict(session, labels, row)
        expect = row["expect_intent"]
        min_conf = float(row.get("expect_min_confidence", 0.0))
        conf_ok = conf >= min_conf if expect != "unknown" else True
        intent_ok = pred == expect
        if intent_ok and conf_ok:
            ok += 1
        else:
            fails.append(
                f"{row.get('id', '?')}: expect={expect} got={pred} conf={conf:.3f}"
                + ("" if intent_ok else " [intent]")
                + ("" if conf_ok else " [confidence]")
            )

    acc = ok / len(rows) if rows else 0.0
    print(f"golden={args.golden.name} samples={len(rows)} pass={ok} acc={acc:.3f}")
    for line in fails:
        print(f"  FAIL {line}")

    if acc < args.min_pass_rate:
        print(f"ERROR: pass rate {acc:.3f} < {args.min_pass_rate:.3f}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
