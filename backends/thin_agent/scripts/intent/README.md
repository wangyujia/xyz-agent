# Intent ONNX PoC scripts

## Files

| File | Purpose |
|------|---------|
| `intent_features.py` | 16-dim feature extraction (must match `IntentOnnx.cpp`) |
| `export_intent_onnx.py` | Train linear classifier from `intent_samples.jsonl` → ONNX |
| `eval_golden.py` | Holdout evaluation on `models/intent/golden_eval.jsonl` |
| `dump_golden_features.py` | Print feature vectors for Python/C++ parity debugging |

## Data

| Path | Role |
|------|------|
| `models/intent/intent_samples.jsonl` | **Training** set (used by export) |
| `models/intent/golden_eval.jsonl` | **Holdout** regression set (not used in training) |
| `models/intent/intent_multiclass.onnx` | Exported model |
| `models/intent/labels.json` | Label order metadata |

## Workflow

```bash
# use project venv (has numpy/onnx/onnxruntime)
.venv/bin/python scripts/intent/export_intent_onnx.py
.venv/bin/python scripts/intent/eval_golden.py
.venv/bin/python scripts/intent/dump_golden_features.py | head

# or via ctest (10th test: unit_intent_golden)
cmake --build build -j && ctest --test-dir build --output-on-failure
```

After retraining, run `ctest` and ws demo with `THIN_AGENT_INTENT_ONNX=1`.

## golden_eval.jsonl schema

```json
{
  "id": "w_holdout_01",
  "text": "用户原话",
  "last_intent": "",
  "last_slots": {},
  "expect_intent": "weather",
  "expect_min_confidence": 0.45,
  "expect_route": "local_external_weather",
  "notes": "optional"
}
```

- `expect_route` is for AgentService e2e documentation; `eval_golden.py` only checks ONNX `expect_intent`.
- Keep golden utterances **out of** `intent_samples.jsonl` to avoid train/test leakage.
