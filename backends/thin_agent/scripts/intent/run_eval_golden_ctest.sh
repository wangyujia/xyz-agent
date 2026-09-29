#!/usr/bin/env bash
# ctest wrapper: evaluate ONNX intent model on golden_eval.jsonl holdout set.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

if [[ -x "${ROOT}/.venv/bin/python3" ]]; then
  PYTHON="${ROOT}/.venv/bin/python3"
elif command -v python3 >/dev/null 2>&1; then
  PYTHON="python3"
else
  echo "ERROR: python3 not found (create .venv for intent golden eval)" >&2
  exit 1
fi

exec "$PYTHON" "${ROOT}/scripts/intent/eval_golden.py" "$@"
