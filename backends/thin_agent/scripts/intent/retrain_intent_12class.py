#!/usr/bin/env python3
"""Retrain intent_multiclass.onnx from 8→12 classes.

Adds 4 new intent classes:
  - memory_history   (历史记忆查询)
  - media_capture    (拍照/录像)
  - media_review     (查看媒体)
  - media_share      (分享媒体)

Usage:
  python scripts/intent/retrain_intent_12class.py              # train + export
  python scripts/intent/retrain_intent_12class.py --eval-only  # eval only
  python scripts/intent/retrain_intent_12class.py --add-samples samples.jsonl  # merge extra

Feature vector (64-dim, unchanged): same as intent_features.py — the linear
classifier learns from existing keyword + ngram features.  No new feature
dimensions needed.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "scripts" / "intent") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts" / "intent"))

from intent_features import FEATURE_DIM, extract_features  # noqa: E402

# ── paths ──────────────────────────────────────────────────────────
SAMPLES = ROOT / "models/intent/intent_samples.jsonl"
GOLDEN = ROOT / "models/intent/golden_eval.jsonl"
LABELS_JSON = ROOT / "models/intent/labels.json"
OUT_ONNX = ROOT / "models/intent/intent_multiclass.onnx"
BACKUP_ONNX = ROOT / "models/intent/intent_multiclass_8class_backup.onnx"

# ── 12-class label list ───────────────────────────────────────────
LABELS_12 = [
    "unknown",
    "profile",
    "weather",
    "news",
    "status",
    "general",
    "memory_recent",
    "event_recent",
    "memory_history",      # ← new
    "media_capture",        # ← new
    "media_review",         # ← new
    "media_share",          # ← new
]

# ── default training samples for the 4 new classes ─────────────────
# User can override via --add-samples
NEW_SAMPLES: list[dict] = [
    # ── memory_history ──
    {"text": "查看我的历史记忆", "intent": "memory_history"},
    {"text": "以前我们聊过什么", "intent": "memory_history"},
    {"text": "历史对话记录", "intent": "memory_history"},
    {"text": "还记得我们上次说的吗", "intent": "memory_history"},
    {"text": "回顾之前的记忆", "intent": "memory_history"},
    {"text": "搜索历史记忆", "intent": "memory_history"},
    {"text": "之前的对话内容", "intent": "memory_history"},
    {"text": "帮我回忆一下", "intent": "memory_history"},
    {"text": "过去聊过的话题", "intent": "memory_history"},
    {"text": "查一下以前的记录", "intent": "memory_history"},
    {"text": "最近聊了什么", "intent": "memory_history"},
    {"text": "我们上次讨论了什么", "intent": "memory_history"},
    # ── media_capture ──
    {"text": "拍张照片", "intent": "media_capture"},
    {"text": "给我拍个照", "intent": "media_capture"},
    {"text": "开始录像", "intent": "media_capture"},
    {"text": "录制视频", "intent": "media_capture"},
    {"text": "拍照", "intent": "media_capture"},
    {"text": "拍一张", "intent": "media_capture"},
    {"text": "录一段视频", "intent": "media_capture"},
    {"text": "拍个短视频", "intent": "media_capture"},
    {"text": "开始录制", "intent": "media_capture"},
    {"text": "按下快门", "intent": "media_capture"},
    {"text": "录像", "intent": "media_capture"},
    {"text": "拍个全景", "intent": "media_capture"},
    # ── media_review ──
    {"text": "看看刚才拍的照片", "intent": "media_review"},
    {"text": "打开相册", "intent": "media_review"},
    {"text": "浏览照片", "intent": "media_review"},
    {"text": "回放视频", "intent": "media_review"},
    {"text": "查看媒体库", "intent": "media_review"},
    {"text": "打开图库", "intent": "media_review"},
    {"text": "最近的照片", "intent": "media_review"},
    {"text": "播放刚才的视频", "intent": "media_review"},
    {"text": "看看拍的视频", "intent": "media_review"},
    {"text": "打开媒体文件", "intent": "media_review"},
    {"text": "翻看照片", "intent": "media_review"},
    {"text": "查看录像", "intent": "media_review"},
    # ── media_share ──
    {"text": "分享这张照片", "intent": "media_share"},
    {"text": "发送给朋友", "intent": "media_share"},
    {"text": "转发视频", "intent": "media_share"},
    {"text": "把这个发给", "intent": "media_share"},
    {"text": "分享到", "intent": "media_share"},
    {"text": "把照片发出去", "intent": "media_share"},
    {"text": "传给他", "intent": "media_share"},
    {"text": "分享刚才拍的", "intent": "media_share"},
    {"text": "发送照片", "intent": "media_share"},
    {"text": "共享这个文件", "intent": "media_share"},
    {"text": "传给手机", "intent": "media_share"},
    {"text": "发给他", "intent": "media_share"},
]


# ── helpers ───────────────────────────────────────────────────────
def load_jsonl(path: Path) -> list[dict]:
    rows: list[dict] = []
    with path.open(encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rows.append(json.loads(line))
    return rows


def save_jsonl(path: Path, rows: list[dict]) -> None:
    with path.open("w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")


def extract_dataset(samples: list[dict], labels: list[str]) -> tuple[np.ndarray, np.ndarray]:
    """Convert samples → (X, y) arrays."""
    label_to_idx = {l: i for i, l in enumerate(labels)}
    xs: list[np.ndarray] = []
    ys: list[int] = []
    for row in samples:
        feat = extract_features(
            row["text"],
            row.get("last_intent", ""),
            row.get("last_slots", {}),
        )
        xs.append(feat)
        ys.append(label_to_idx[row["intent"]])
    return np.stack(xs), np.array(ys, dtype=np.int64)


def train_softmax(x: np.ndarray, y: np.ndarray, num_classes: int,
                  epochs: int = 800, lr: float = 0.15) -> np.ndarray:
    """Train a softmax (multinomial logistic regression) classifier.

    Returns combined [num_classes, FEATURE_DIM+1] matrix (weights + bias col).
    """
    n = x.shape[0]
    w = np.zeros((FEATURE_DIM, num_classes), dtype=np.float32)
    b = np.zeros(num_classes, dtype=np.float32)
    for _ in range(epochs):
        logits = x @ w + b
        logits -= logits.max(axis=1, keepdims=True)
        exp = np.exp(logits)
        prob = exp / exp.sum(axis=1, keepdims=True)
        one_hot = np.zeros_like(prob)
        one_hot[np.arange(n), y] = 1.0
        grad_logits = (prob - one_hot) / n
        w -= lr * (x.T @ grad_logits)
        b -= lr * grad_logits.sum(axis=0)
    return np.hstack([w.T, b.reshape(-1, 1)])


def export_onnx(combined_wb: np.ndarray, labels: list[str]) -> None:
    """Export linear classifier to ONNX Gemm."""
    import onnx
    from onnx import TensorProto, helper, numpy_helper

    num_classes, cols = combined_wb.shape
    feature_dim = cols - 1
    w = combined_wb[:, :feature_dim].T.astype(np.float32)
    b = combined_wb[:, feature_dim].astype(np.float32)

    x_info = helper.make_tensor_value_info("features", TensorProto.FLOAT, [1, feature_dim])
    y_info = helper.make_tensor_value_info("logits", TensorProto.FLOAT, [1, num_classes])

    w_init = numpy_helper.from_array(w, name="W")
    b_init = numpy_helper.from_array(b, name="B")

    gemm = helper.make_node("Gemm", ["features", "W", "B"], ["logits"],
                            alpha=1.0, beta=1.0, transB=0)

    graph = helper.make_graph([gemm], "intent_multiclass_12", [x_info], [y_info],
                              [w_init, b_init])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 11)])
    model.ir_version = 8
    onnx.checker.check_model(model)
    OUT_ONNX.write_bytes(model.SerializeToString())
    print(f"  wrote {OUT_ONNX} ({OUT_ONNX.stat().st_size} bytes)")


def eval_model(onnx_path: Path, golden_path: Path, labels: list[str]) -> float:
    """Evaluate ONNX model on golden holdout set. Returns accuracy."""
    try:
        import onnxruntime as ort
    except ImportError:
        print("  [SKIP] onnxruntime not installed; cannot eval")
        return -1.0

    rows = load_jsonl(golden_path)
    if not rows:
        print("  [SKIP] no golden_eval samples")
        return -1.0

    session = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    label_to_idx = {l: i for i, l in enumerate(labels)}
    ok = 0
    fails: list[str] = []
    for row in rows:
        feat = extract_features(
            row["text"],
            row.get("last_intent", ""),
            row.get("last_slots", {}),
        ).reshape(1, -1).astype(np.float32)
        logits = session.run(None, {"features": feat})[0][0]
        idx = int(np.argmax(logits))

        # softmax confidence
        shifted = logits - logits.max()
        exp = np.exp(shifted)
        conf = float(exp[idx] / exp.sum())

        expect = row["expect_intent"]
        pred = labels[idx] if idx < len(labels) else "unknown"

        if pred == expect:
            ok += 1
        else:
            fails.append(
                f"  FAIL {row.get('id', '?')}: expect={expect} got={pred} conf={conf:.3f}"
            )

    acc = ok / len(rows)
    print(f"  golden={golden_path.name} samples={len(rows)} pass={ok} acc={acc:.3f}")
    for line in fails:
        print(line)
    return acc


# ── main ──────────────────────────────────────────────────────────
def main() -> int:
    parser = argparse.ArgumentParser(description="Retrain intent ONNX 8→12 classes")
    parser.add_argument("--eval-only", action="store_true",
                        help="only evaluate existing ONNX, don't retrain")
    parser.add_argument("--add-samples", type=Path, default=None,
                        help="path to extra .jsonl samples to merge")
    parser.add_argument("--epochs", type=int, default=800)
    parser.add_argument("--lr", type=float, default=0.15)
    parser.add_argument("--no-backup", action="store_true",
                        help="skip backing up existing 8-class ONNX")
    args = parser.parse_args()

    labels = LABELS_12
    num_classes = len(labels)
    print(f"Intent classifier: {num_classes} classes")
    print(f"  {', '.join(labels)}")

    if args.eval_only:
        if OUT_ONNX.exists():
            eval_model(OUT_ONNX, GOLDEN, labels)
        else:
            print("ERROR: no existing ONNX model to eval", file=sys.stderr)
            return 1
        return 0

    # ── load existing training data ────────────────────────────────
    existing = load_jsonl(SAMPLES)
    print(f"\nLoaded {len(existing)} existing samples from {SAMPLES.name}")

    # ── add default new samples ────────────────────────────────────
    samples = existing.copy()
    new_count = 0
    for s in NEW_SAMPLES:
        # skip if duplicating an existing sample's text
        if not any(e["text"] == s["text"] for e in existing):
            samples.append(s)
            new_count += 1
    print(f"Added {new_count} new default samples (4 new classes, 12 each)")

    # ── merge extra user samples ───────────────────────────────────
    if args.add_samples:
        extra = load_jsonl(args.add_samples)
        for s in extra:
            if not any(e["text"] == s["text"] for e in samples):
                samples.append(s)
        print(f"Added {len(extra)} samples from {args.add_samples.name}")

    # ── validate labels ────────────────────────────────────────────
    label_set = set(labels)
    for s in samples:
        if s["intent"] not in label_set:
            print(f"ERROR: unknown intent '{s['intent']}' in sample: {s['text']}",
                  file=sys.stderr)
            return 1

    # ── extract features + train ───────────────────────────────────
    print(f"\nTraining on {len(samples)} samples, {num_classes} classes...")
    x, y = extract_dataset(samples, labels)
    wb = train_softmax(x, y, num_classes, epochs=args.epochs, lr=args.lr)

    # ── report train accuracy ──────────────────────────────────────
    logits = x @ wb[:, :FEATURE_DIM].T + wb[:, FEATURE_DIM]
    pred = logits.argmax(axis=1)
    acc = (pred == y).mean()
    print(f"  train accuracy: {acc:.3f} ({int((pred == y).sum())}/{len(y)})")

    # ── per-class accuracy ─────────────────────────────────────────
    print("  per-class:")
    for i, label in enumerate(labels):
        mask = y == i
        if mask.sum() > 0:
            cls_acc = (pred[mask] == i).mean()
            print(f"    {label:20s}: {mask.sum():3d} samples, acc={cls_acc:.3f}")
        else:
            print(f"    {label:20s}: 0 samples")

    # ── backup existing 8-class ONNX ───────────────────────────────
    if not args.no_backup and OUT_ONNX.exists() and not BACKUP_ONNX.exists():
        import shutil
        shutil.copy2(OUT_ONNX, BACKUP_ONNX)
        print(f"\n  backed up 8-class ONNX → {BACKUP_ONNX.name}")

    # ── export ─────────────────────────────────────────────────────
    export_onnx(wb, labels)

    # ── update labels.json ─────────────────────────────────────────
    LABELS_JSON.write_text(
        json.dumps({"labels": labels, "feature_dim": FEATURE_DIM},
                   ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"  updated {LABELS_JSON.name}")

    # ── eval on holdout ────────────────────────────────────────────
    print()
    eval_model(OUT_ONNX, GOLDEN, labels)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
