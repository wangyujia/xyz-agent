#!/usr/bin/env bash
# demo_smoke_all.sh — thin_agent 一键冒烟 + 门禁脚本
# 用法：
#   bash scripts/demo_smoke_all.sh              # 完整冒烟（build + unit + e2e local/cloud）
#   bash scripts/demo_smoke_all.sh --quick      # 仅 e2e（跳过 build/unit）
#   bash scripts/demo_smoke_all.sh --local-only # 仅本地模式
#
# 退出码：0 = ALL_PASS，非0 = 有失败
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

PASS=0
FAIL=0
RESULTS=()

# 优先使用项目 venv（E2E 脚本依赖 PyYAML 等）
if [[ -x "$PROJECT_DIR/.venv/bin/python" ]]; then
  PYTHON="$PROJECT_DIR/.venv/bin/python"
else
  PYTHON="${PYTHON:-python3}"
fi

# ---------- helpers ----------
pass() {
  PASS=$((PASS + 1))
  RESULTS+=("${GREEN}PASS${NC}  $1")
  echo -e "  ${GREEN}✓${NC} $1"
}
fail() {
  FAIL=$((FAIL + 1))
  RESULTS+=("${RED}FAIL${NC}  $1  —  $2")
  echo -e "  ${RED}✗${NC} $1  —  $2"
}

# v0.54.10 (R93 基建地雷清理): **只清理本脚本自己启动的进程**
# 旧实现 `pids=$(pgrep -f "build/thin_agent"); kill $pids` 是**全局扫射**——同机上任何
# thin_agent 进程都会被装进去：并行跑的 e2e/ctest 服务实例（`./build/thin_agent --port 18xxx`）、
# 开发者在调的服务，全部照杀 ⇒ 表现为"服务莫名消失/端口冲突"的假故障（本脚本 Phase 3/4 各调一次，
# 杀伤面更大）。现在：①登记本脚本启动的 PID，只杀这些 ②端口被占 ⇒ 明确报错，不杀别人。
STARTED_PIDS=()

cleanup_procs() {
  local p
  for p in "${STARTED_PIDS[@]:-}"; do
    [[ -z "$p" ]] && continue
    kill -0 "$p" 2>/dev/null && kill "$p" 2>/dev/null || true
  done
  sleep 1
  for p in "${STARTED_PIDS[@]:-}"; do
    [[ -z "$p" ]] && continue
    kill -0 "$p" 2>/dev/null && kill -9 "$p" 2>/dev/null || true
  done
  STARTED_PIDS=()
}

# 端口预检：占用者不是本脚本的进程时**只报错不清理**（拒绝替用户杀进程）
port_in_use() {
  local host="$1" port="$2"
  (echo >"/dev/tcp/$host/$port") 2>/dev/null
}

wait_for_port() {
  local host="$1" port="$2" timeout="$3"
  local elapsed=0
  while [[ $elapsed -lt $timeout ]]; do
    if (echo >"/dev/tcp/$host/$port") 2>/dev/null; then
      return 0
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done
  return 1
}

# 独立 E2E 测试（不需要 thin_agent wrapper，测试脚本自行管理进程）
run_e2e_standalone() {
  local label="$1" cmd="$2"
  echo -e "\n${CYAN}━━━ $label ━━━${NC}"

  local e2e_out
  if e2e_out=$(eval "$cmd" 2>&1); then
    echo "$e2e_out" | head -8
    pass "$label"
  else
    echo "$e2e_out"
    fail "$label" "$(echo "$e2e_out" | tail -3 | tr '\n' ' ')"
  fi
}

# 启动 thin_agent + 跑 e2e + 停服务
# 用法: run_e2e <label> <port> <profile> <e2e_script> [e2e_args...]
run_e2e() {
  local label="$1" port="$2" profile="$3" e2e_script="$4"
  shift 4
  local e2e_args=("$@")

  echo -e "\n${CYAN}━━━ $label ━━━${NC}"

  local logfile="/tmp/thin_agent_smoke_${port}.log"

  # v0.54.10: 端口预检——占用则**报错**（不杀别人的进程；历史行为是全局 pgrep 扫射）
  if port_in_use 127.0.0.1 "$port"; then
    fail "$label — 端口 $port 已被占用" \
      "占用者非本脚本进程，未做任何清理；请先释放该端口（占用者: $(ss -ltnp 2>/dev/null | grep ":$port " | head -1)）"
    return 1
  fi

  # 启动 thin_agent
  ./build/thin_agent \
    --host 127.0.0.1 \
    --port "$port" \
    --config /root/.thin_agent/demo.model.yaml \
    --profile "$profile" \
    > "$logfile" 2>&1 &
  local pid=$!
  STARTED_PIDS+=("$pid")   # v0.54.10: 登记，cleanup_procs 只清这些
  echo "  thin_agent PID=$pid port=$port profile=$profile"

  # 等端口就绪
  if ! wait_for_port 127.0.0.1 "$port" 15; then
    fail "$label — 启动超时" "端口 $port 15s 未就绪，日志: $logfile"
    kill $pid 2>/dev/null || true
    return 1
  fi

  # 跑 e2e
  local e2e_out
  if e2e_out=$("$PYTHON" "$e2e_script" --port "$port" "${e2e_args[@]}" 2>&1); then
    echo "$e2e_out" | head -8
    pass "$label"
  else
    echo "$e2e_out"
    fail "$label" "$(echo "$e2e_out" | tail -3 | tr '\n' ' ')"
  fi

  # 停服务
  kill $pid 2>/dev/null || true
  wait $pid 2>/dev/null || true
  sleep 1
}

# ---------- 解析参数 ----------
QUICK=false
LOCAL_ONLY=false
for arg in "$@"; do
  case "$arg" in
    --quick) QUICK=true ;;
    --local-only) LOCAL_ONLY=true ;;
  esac
done

echo "══════════════════════════════════════════════"
echo "  thin_agent demo_smoke_all"
echo "  $(date '+%Y-%m-%d %H:%M:%S')"
echo "  mode: quick=$QUICK local_only=$LOCAL_ONLY"
echo "══════════════════════════════════════════════"

# 加载云 API Key（cloud 模式需要）
if [[ "$LOCAL_ONLY" != "true" ]]; then
  ENV_FILE=/root/.thin_agent/deepseek.env
  [[ -f "$ENV_FILE" ]] || ENV_FILE=/root/.thin_agent/zai.env
  if [[ -f "$ENV_FILE" ]]; then
    set -a
    # shellcheck disable=SC1091
    source "$ENV_FILE"
    set +a
    echo -e "  ${GREEN}✓${NC} ${ENV_FILE} loaded"
  else
    echo -e "  ${YELLOW}⚠${NC} 无 deepseek.env/zai.env，cloud 模式可能失败"
  fi
fi

# ---------- Phase 1: Build ----------
if [[ "$QUICK" == "false" ]]; then
  echo -e "\n${CYAN}━━━ Phase 1: Build ━━━${NC}"
  if cmake --build build -j"$(nproc)" 2>&1 | tail -5; then
    pass "cmake --build"
  else
    fail "cmake --build" "构建失败"
    echo -e "\n${RED}构建失败，终止后续步骤${NC}"
    exit 1
  fi
fi

# ---------- Phase 2: Unit Tests ----------
if [[ "$QUICK" == "false" ]]; then
  echo -e "\n${CYAN}━━━ Phase 2: Unit Tests ━━━${NC}"
  if ctest --test-dir build --output-on-failure 2>&1 | tail -8; then
    pass "ctest (unit tests)"
  else
    fail "ctest (unit tests)" "有单测失败"
  fi
fi

# ---------- Phase 3: E2E ----------
cleanup_procs

# 3a: 纯端侧 (offline) —— e2e_ws_v02.py
run_e2e "E2E local (offline)" 19101 "offline_local_demo" \
  "tests/demo/e2e_ws_v02.py"

# 3b: 纯云 pro (cloud-only) —— e2e_cloud_chat.py
if [[ "$LOCAL_ONLY" != "true" ]]; then
  run_e2e "E2E cloud-only (main)" 19102 "zai_main_cloud_only" \
    "tests/demo/e2e_cloud_chat.py" \
    --expect-mode cloud \
    --expect-mode-alt "agent-loop" \
    --expect-intent-backend cloud-strategy \
    --expect-intent-backend-alt cloud \
    --expect-substr "验证"

  # 3c: 纯云 flash (cloud-only) —— e2e_cloud_chat.py
  run_e2e "E2E cloud-only (fast)" 19103 "zai_fast_cloud_only" \
    "tests/demo/e2e_cloud_chat.py" \
    --expect-mode cloud \
    --expect-mode-alt "agent-loop" \
    --expect-intent-backend cloud-strategy \
    --expect-intent-backend-alt cloud \
    --expect-substr "验证" \
    --expect-substr-alt "thin_agent"
else
  echo -e "\n  ${YELLOW}跳过 cloud E2E (--local-only)${NC}"
fi

# 3d: 多轮对话
run_e2e "E2E multi-turn (offline)" 19106 "offline_local_demo" \
  "tests/demo/e2e_multi_turn.py"

# 3e: 失败注入（独立进程，自行管理 thin_agent + mock server，不占用 thin_agent 端口）
run_e2e_standalone "E2E failure paths" \
  "\"$PYTHON\" tests/demo/e2e_failure.py --port 19110 --mock-port 19120"

# ---------- Phase 4: 残留清理 ----------
cleanup_procs

# ---------- 汇总 ----------
echo -e "\n══════════════════════════════════════════════"
echo -e "  ${CYAN}冒烟结果汇总${NC}"
echo -e "══════════════════════════════════════════════"
for r in "${RESULTS[@]}"; do
  echo -e "  $r"
done
echo -e "──────────────────────────────────────────────"
TOTAL=$((PASS + FAIL))
echo -e "  总计: $TOTAL  |  ${GREEN}PASS: $PASS${NC}  |  ${RED}FAIL: $FAIL${NC}"
echo -e "══════════════════════════════════════════════"

if [[ $FAIL -eq 0 ]]; then
  echo -e "  ${GREEN}ALL_PASS${NC} ✓"
  exit 0
else
  echo -e "  ${RED}FAIL${NC} — 请检查上方失败项"
  exit 1
fi
