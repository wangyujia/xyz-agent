#!/usr/bin/env python3
"""Dump golden_eval feature vectors for parity checks against IntentOnnx.cpp."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "scripts" / "intent") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts" / "intent"))

from intent_features import FEATURE_NAMES, extract_features  # noqa: E402

GOLDEN = ROOT / "models/intent/golden_eval.jsonl"


def main() -> int:
    golden = GOLDEN
    if len(sys.argv) > 1:
        golden = Path(sys.argv[1])

    with golden.open(encoding="utf-8") as f:
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
            payload = {
                "id": row.get("id", ""),
                "text": row["text"],
                "features": {name: round(float(feat[i]), 6) for i, name in enumerate(FEATURE_NAMES)},
                "vector": [round(float(x), 6) for x in feat],
            }
            print(json.dumps(payload, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
