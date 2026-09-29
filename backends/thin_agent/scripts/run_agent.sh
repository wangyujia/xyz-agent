#!/usr/bin/env bash
# thin_agent Agent Core 启动（pc / cam 统一参数）
#
# 用法:
#   run_agent.sh [--fast|--main|--local] [--host H] [--port P]
#   THIN_AGENT_HOME=/data/thin_agent run_agent.sh --main
set -euo pipefail

PROFILE="deepseek_fast_demo"   # 默认 deepseek；--glm 切回 zai_main_demo (GLM-5.2)
HOST=""
PORT="8765"
CLOUD_ONLY="0"
DEV_MODE="0"
AUTO_MODE="0"

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
  run_agent.sh [profile]
  run_agent.sh [--fast|--main|--auto|--local|--cloud-only|--dev] [options]

Options:
  --fast                  Use deepseek_fast_demo profile (default)
  --main                  Use deepseek_main_demo profile
  --chain                 Use chain_ds_glm_demo (DeepSeek → GLM fallback)
  --glm                   Use zai_main_demo (GLM-5.2)
  --auto                  Route strategy: local-first cascade before cloud
                         (uses fast model by default, combine with --main)
  --local, --local-only   Use offline_local_demo profile
  --dev                   Enable developer mode (shell_exec blacklist)
  --cloud-only            Disable offline fallback for cloud profiles
  --host <host>           Bind host (pc default: 127.0.0.1, cam default: 0.0.0.0)
  --port <port>           Bind port (default: 8765)
  -h, --help              Show this help

  (--flash / --pro also accepted for backward compatibility)

Environment:
  THIN_AGENT_HOME         Runtime root (cam: /data/thin_agent)
  THIN_AGENT_REPO         Repo root on pc (auto-detected if omitted)
  LOCAL_AGENT_DIR         Config dir on pc (default: ~/.thin_agent)

Examples:
  run_agent.sh --main
  run_agent.sh --glm --main           # GLM 备份
  run_agent.sh --local --port 18765
  run_agent.sh --fast --host 0.0.0.0 --port 8765
  run_agent.sh --auto --main           # local-first with main model
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --fast|--flash) PROFILE="deepseek_fast_demo"; shift ;;
    --main|--pro)   PROFILE="deepseek_main_demo"; shift ;;
    --chain)        PROFILE="chain_ds_glm_demo"; shift ;;  # v0.47.4: 模型链
    --glm) GLM_MODE="1"; shift ;;
    --auto) AUTO_MODE="1"; shift ;;
    --local|--local-only) PROFILE="offline_local_demo"; shift ;;
    --dev) DEV_MODE="1"; shift ;;
    --cloud-only) CLOUD_ONLY="1"; shift ;;
    --host)
      [[ $# -ge 2 ]] || { echo "ERROR: --host requires a value" >&2; exit 2; }
      HOST="$2"
      shift 2
      ;;
    --port)
      [[ $# -ge 2 ]] || { echo "ERROR: --port requires a value" >&2; exit 2; }
      PORT="$2"
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    --*)
      echo "ERROR: unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *) PROFILE="$1"; shift ;;
  esac
done

# --glm 切回 GLM 备份（deepseek 为默认）— v0.47.7: 默认用 GLM-5.2（zai_main_demo）
if [[ "${GLM_MODE:-}" == "1" ]]; then
  case "$PROFILE" in
    deepseek_fast_demo|deepseek_main_demo|zai_fast_demo|zai_main_demo) PROFILE="zai_main_demo" ;;
  esac
fi

if [[ "$CLOUD_ONLY" == "1" ]]; then
  case "$PROFILE" in
    zai_fast_demo) PROFILE="zai_fast_cloud_only" ;;
    zai_main_demo) PROFILE="zai_main_cloud_only" ;;
    deepseek_fast_demo) PROFILE="deepseek_fast_cloud_only" ;;
    deepseek_main_demo) PROFILE="deepseek_main_cloud_only" ;;
  esac
fi

AGENT_BIN=""
CONFIG=""
ENV_FILE=""
RUNTIME="pc"
ROOT_DIR=""

if [[ -n "${THIN_AGENT_HOME:-}" && -x "${THIN_AGENT_HOME}/bin/thin_agent" ]]; then
  RUNTIME="cam"
  HOME_DIR="$THIN_AGENT_HOME"
elif [[ -x "$SELF_DIR/bin/thin_agent" ]]; then
  RUNTIME="cam"
  HOME_DIR="$SELF_DIR"
elif [[ -x "$(resolve_repo_root)/build-aarch64/thin_agent" && "${THIN_AGENT_RUNTIME:-}" == "cam" ]]; then
  RUNTIME="cam"
  HOME_DIR="${THIN_AGENT_HOME:-/data/thin_agent}"
else
  RUNTIME="pc"
  ROOT_DIR="$(resolve_repo_root)"
  HOME_DIR="${LOCAL_AGENT_DIR:-$(default_local_agent_dir)}"
fi

if [[ "$RUNTIME" == "cam" ]]; then
  AGENT_BIN="${HOME_DIR}/bin/thin_agent"
  CONFIG="${HOME_DIR}/config/demo.model.yaml"
  # v0.53.95: 云凭据改用 deepseek.env（DeepSeek 官方直连/模型 deepseek-flash），
  # 缺失时回退 zai.env（GLM 备份通道，注意本机 GLM key 已过期）
  ENV_FILE="${HOME_DIR}/deepseek.env"; [[ -f "$ENV_FILE" ]] || ENV_FILE="${HOME_DIR}/zai.env"
  [[ -n "$HOST" ]] || HOST="${THIN_AGENT_HOST:-0.0.0.0}"
  export LD_LIBRARY_PATH="${HOME_DIR}/lib:${LD_LIBRARY_PATH:-}"
  export THIN_AGENT_CHAT_POLICY_PATH="${HOME_DIR}/config/chat_policy.json"
  export THIN_AGENT_INTENT_ONNX="${THIN_AGENT_INTENT_ONNX:-1}"
else
  AGENT_BIN="${ROOT_DIR}/build/thin_agent"
  CONFIG="${HOME_DIR}/demo.model.yaml"
  # v0.53.95: 云凭据改用 deepseek.env（DeepSeek 官方直连/模型 deepseek-flash），
  # 缺失时回退 zai.env（GLM 备份通道，注意本机 GLM key 已过期）
  ENV_FILE="${HOME_DIR}/deepseek.env"; [[ -f "$ENV_FILE" ]] || ENV_FILE="${HOME_DIR}/zai.env"
  [[ -n "$HOST" ]] || HOST="127.0.0.1"
fi

[[ -x "$AGENT_BIN" ]] || {
  echo "ERROR: thin_agent not found: $AGENT_BIN" >&2
  echo "  pc: cmake --build build --target thin_agent" >&2
  echo "  cam: bash build.sh cam && bash deploy.sh" >&2
  exit 2
}
[[ -f "$CONFIG" ]] || { echo "ERROR: config not found: $CONFIG" >&2; exit 2; }

if [[ -f "$ENV_FILE" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "$ENV_FILE"
  set +a
fi

exec env THIN_AGENT_INTENT_ONNX="${THIN_AGENT_INTENT_ONNX:-1}" \
  THIN_AGENT_DEV_MODE="${DEV_MODE:-0}" \
  THIN_AGENT_AUTO_MODE="${AUTO_MODE:-0}" \
  "$AGENT_BIN" \
  --host "$HOST" \
  --port "$PORT" \
  --config "$CONFIG" \
  --profile "$PROFILE"
