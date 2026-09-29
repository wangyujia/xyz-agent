#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

MAX_BIN_SIZE_MB="${MAX_BIN_SIZE_MB:-4}"
MAX_BIN_SIZE_BYTES=$((MAX_BIN_SIZE_MB * 1024 * 1024))
PROFILE="${THIN_AGENT_GATE_PROFILE:-zai_fast_demo}"
START_TIMEOUT_SEC="${THIN_AGENT_GATE_START_TIMEOUT_SEC:-4}"

echo "[gate] root=$ROOT_DIR"
echo "[gate] max_bin_size_mb=$MAX_BIN_SIZE_MB"

echo "[gate] step1: build targets"
cmake --build build -j --target thin_agent test_agent_service_unit

echo "[gate] step2: unit target"
./build/test_agent_service_unit

echo "[gate] step3: ctest"
ctest --test-dir build --output-on-failure

echo "[gate] step3.5: ws readable trace smoke"
python3 tests/demo/smoke_ws_readable_memory_trace.py

echo "[gate] step4: binary type + size"
if [[ ! -f build/thin_agent ]]; then
  echo "[gate][error] build/thin_agent not found"
  exit 2
fi

FILE_DESC="$(file build/thin_agent)"
echo "[gate] file=$FILE_DESC"
if [[ "$FILE_DESC" != *"ELF"* ]]; then
  echo "[gate][error] thin_agent is not ELF binary"
  exit 3
fi

BIN_SIZE_BYTES="$(stat -c %s build/thin_agent)"
BIN_SIZE_MB="$(awk -v b="$BIN_SIZE_BYTES" 'BEGIN{printf "%.2f", b/1024/1024}')"
echo "[gate] size_bytes=$BIN_SIZE_BYTES size_mb=$BIN_SIZE_MB"
if (( BIN_SIZE_BYTES > MAX_BIN_SIZE_BYTES )); then
  echo "[gate][error] binary size exceeds threshold: ${BIN_SIZE_BYTES} > ${MAX_BIN_SIZE_BYTES}"
  exit 4
fi

echo "[gate] step5: startup version line"
START_LOG="$(mktemp)"
set +e
timeout "${START_TIMEOUT_SEC}s" "$HOME/.thin_agent/run_agent.sh" "$PROFILE" >"$START_LOG" 2>&1
START_EXIT=$?
set -e
cat "$START_LOG"

if ! grep -q "\[ws_agent_cpp\] version=" "$START_LOG"; then
  echo "[gate][error] startup log missing version line"
  exit 5
fi

EXPECTED_VERSION="$(grep -E 'kThinAgentVersion = "v[0-9]+\.[0-9]+\.[0-9]+";' include/thin_agent/Version.h | sed -E 's/.*"(v[0-9]+\.[0-9]+\.[0-9]+)".*/\1/')"
if [[ -z "$EXPECTED_VERSION" ]]; then
  echo "[gate][error] failed to parse expected version from Version.h"
  exit 6
fi

if ! grep -q "\[ws_agent_cpp\] version=${EXPECTED_VERSION}" "$START_LOG"; then
  echo "[gate][error] startup version mismatch, expected ${EXPECTED_VERSION}"
  exit 7
fi

if [[ "$START_EXIT" -ne 0 && "$START_EXIT" -ne 124 ]]; then
  echo "[gate][error] unexpected startup exit code: $START_EXIT"
  exit 8
fi

echo "[gate] PASS embedded lightweight gate (version=${EXPECTED_VERSION}, size_mb=${BIN_SIZE_MB})"
