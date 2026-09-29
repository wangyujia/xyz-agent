#!/usr/bin/env python3
"""Export intent_multiclass.onnx from intent_samples.jsonl (feature-linear classifier PoC)."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "scripts" / "intent") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts" / "intent"))

from intent_features import FEATURE_DIM, extract_features  # noqa: E402

SAMPLES = ROOT / "models/intent/intent_samples.jsonl"
LABELS_JSON = ROOT / "models/intent/labels.json"
OUT_ONNX = ROOT / "models/intent/intent_multiclass.onnx"

LABELS = [
    "unknown",
    "profile",
    "weather",
    "news",
    "status",
    "general",
    "memory_recent",
    "event_recent",
]


def load_samples() -> tuple[np.ndarray, np.ndarray]:
    xs: list[np.ndarray] = []
    ys: list[int] = []
    label_to_idx = {l: i for i, l in enumerate(LABELS)}
    with SAMPLES.open(encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            feat = extract_features(
                row["text"],
                row.get("last_intent", ""),
                row.get("last_slots", {}),
            )
            xs.append(feat)
            ys.append(label_to_idx[row["intent"]])
    return np.stack(xs), np.array(ys, dtype=np.int64)


def train_weights(x: np.ndarray, y: np.ndarray, epochs: int = 800, lr: float = 0.15) -> np.ndarray:
    num_classes = len(LABELS)
    w = np.zeros((FEATURE_DIM, num_classes), dtype=np.float32)
    b = np.zeros(num_classes, dtype=np.float32)
    for _ in range(epochs):
        logits = x @ w + b
        logits -= logits.max(axis=1, keepdims=True)
        exp = np.exp(logits)
        prob = exp / exp.sum(axis=1, keepdims=True)
        one_hot = np.zeros_like(prob)
        one_hot[np.arange(len(y)), y] = 1.0
        grad_logits = (prob - one_hot) / len(y)
        w -= lr * (x.T @ grad_logits)
        b -= lr * grad_logits.sum(axis=0)
    return np.hstack([w.T, b.reshape(-1, 1)])


def export_onnx(combined_wb: np.ndarray) -> None:
    num_classes, cols = combined_wb.shape
    feature_dim = cols - 1
    w = combined_wb[:, :feature_dim].T.astype(np.float32)
    b = combined_wb[:, feature_dim].astype(np.float32)

    x_info = helper.make_tensor_value_info("features", TensorProto.FLOAT, [1, feature_dim])
    y_info = helper.make_tensor_value_info("logits", TensorProto.FLOAT, [1, num_classes])

    w_init = numpy_helper.from_array(w, name="W")
    b_init = numpy_helper.from_array(b, name="B")

    gemm = helper.make_node("Gemm", ["features", "W", "B"], ["logits"], alpha=1.0, beta=1.0, transB=0)

    graph = helper.make_graph([gemm], "intent_multiclass", [x_info], [y_info], [w_init, b_init])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 11)])
    model.ir_version = 8
    onnx.checker.check_model(model)
    OUT_ONNX.write_bytes(model.SerializeToString())
    print(f"wrote {OUT_ONNX} ({OUT_ONNX.stat().st_size} bytes)")


def main() -> None:
    x, y = load_samples()
    wb = train_weights(x, y)
    export_onnx(wb)
    LABELS_JSON.write_text(
        json.dumps({"labels": LABELS, "feature_dim": FEATURE_DIM}, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    logits = x @ wb[:, :FEATURE_DIM].T + wb[:, FEATURE_DIM]
    pred = logits.argmax(axis=1)
    acc = (pred == y).mean()
    print(f"samples={len(y)} train_acc={acc:.3f}")


if __name__ == "__main__":
    main()
