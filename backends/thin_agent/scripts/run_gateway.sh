#!/usr/bin/env bash
# thin_agent IM 网关启动（pc / cam 统一参数）
#
# 用法:
#   run_gateway.sh [--agent ws://127.0.0.1:8765/ws]
#   THIN_AGENT_HOME=/data/thin_agent run_gateway.sh
set -euo pipefail

AGENT_URL="ws://127.0.0.1:8765/ws"

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"

resolve_repo_root() {
  if [[ -n "${THIN_AGENT_REPO:-}" && -f "${THIN_AGENT_REPO}/CMakeLists.txt" ]]; then
    cd "$THIN_AGENT_REPO" && pwd
    return
  fi
  local parent
  parent="$(cd "$SELF_DIR/.." && pwd)"
  if [[ -f "$parent/CMakeLists.txt" ]]; then
    echo "$parent"
    return
  fi
  local candidate
  for candidate in "$HOME/code/thin_agent" "/root/code/thin_agent"; do
    if [[ -f "$candidate/CMakeLists.txt" ]]; then
      echo "$candidate"
      return
    fi
  done
  echo "$parent"
}

default_local_agent_dir() {
  if [[ "$(basename "$SELF_DIR")" == ".thin_agent" ]]; then
    echo "$SELF_DIR"
  else
    echo "${HOME}/.thin_agent"
  fi
}

usage() {
  cat <<'EOF'
Usage:
  run_gateway.sh [options]

启动 IM 网关 thin_agent_gw（飞书长连接 + 微信 iLink long-poll）。
需 thin_agent Agent Core 已在 --agent 地址监听。

Options:
  --agent <url>   thin_agent WS 地址（默认: ws://127.0.0.1:8765/ws）
  -h, --help      Show this help

凭证（zai.env）:
  FEISHU_APP_ID / FEISHU_APP_SECRET
  WECHAT_BOT_ID / WECHAT_SECRET

Examples:
  run_agent.sh --pro              # 终端 A：Agent Core
  run_gateway.sh                  # 终端 B：IM 网关
  run_gateway.sh --agent ws://192.168.1.100:8765/ws
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --agent)
      [[ $# -ge 2 ]] || { echo "ERROR: --agent requires a value" >&2; exit 2; }
      AGENT_URL="$2"
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *)
      echo "ERROR: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

GW_BIN=""
ENV_FILE=""
RUNTIME="pc"
ROOT_DIR=""

if [[ -n "${THIN_AGENT_HOME:-}" && -x "${THIN_AGENT_HOME}/bin/thin_agent_gw" ]]; then
  RUNTIME="cam"
  HOME_DIR="$THIN_AGENT_HOME"
elif [[ -x "$SELF_DIR/bin/thin_agent_gw" ]]; then
  RUNTIME="cam"
  HOME_DIR="$SELF_DIR"
elif [[ -x "$(resolve_repo_root)/build-aarch64/thin_agent_gw" && "${THIN_AGENT_RUNTIME:-}" == "cam" ]]; then
  RUNTIME="cam"
  HOME_DIR="${THIN_AGENT_HOME:-/data/thin_agent}"
else
  RUNTIME="pc"
  ROOT_DIR="$(resolve_repo_root)"
  HOME_DIR="${LOCAL_AGENT_DIR:-$(default_local_agent_dir)}"
fi

if [[ "$RUNTIME" == "cam" ]]; then
  GW_BIN="${HOME_DIR}/bin/thin_agent_gw"
  ENV_FILE="${HOME_DIR}/deepseek.env"; [[ -f "$ENV_FILE" ]] || ENV_FILE="${HOME_DIR}/zai.env"
  export LD_LIBRARY_PATH="${HOME_DIR}/lib:${LD_LIBRARY_PATH:-}"
else
  GW_BIN="${ROOT_DIR}/build/thin_agent_gw"
  ENV_FILE="${HOME_DIR}/deepseek.env"; [[ -f "$ENV_FILE" ]] || ENV_FILE="${HOME_DIR}/zai.env"
fi

[[ -x "$GW_BIN" ]] || {
  echo "ERROR: thin_agent_gw not found: $GW_BIN" >&2
  echo "  pc: cmake --build build --target thin_agent_gw" >&2
  echo "  cam: bash build.sh cam && bash deploy.sh" >&2
  exit 2
}

if [[ -f "$ENV_FILE" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "$ENV_FILE"
  set +a
fi

exec "$GW_BIN" --agent "$AGENT_URL"
