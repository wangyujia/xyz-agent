#!/usr/bin/env bash
#
# thin_agent 设备部署（aarch64）
# - 全量模式（默认）：推送 thin_agent + thin_agent_gw 运行所需文件
# - 单应用模式（--app）：仅推送指定可执行，默认不 reboot
# - 推送采用 /tmp -> mv，规避 Text file busy
#
# 用法:
#   bash deploy.sh                         # cam 全量构建+部署（默认 FDBus ON）
#   bash deploy.sh --no-build              # 仅推送（不编译）
#   bash deploy.sh --no-fdbus              # FakeDevice 版构建+部署
#   bash deploy.sh --with-fdbus            # 同默认，显式 FDBus
#   bash deploy.sh --app thin_agent        # 只推送 agent 二进制
#   bash deploy.sh --app thin_agent_gw     # 只推送 gateway 二进制
#   bash deploy.sh --reboot
#   bash deploy.sh --debug              # Debug 构建+部署（默认 Release）
#   bash deploy.sh --release            # 显式 Release（默认）
#   bash deploy.sh --dist               # 离线打包 dist/aarch64/（不 adb 推送）
#   bash deploy.sh --dist --no-build    # 仅打包已有产物
#   bash deploy.sh --clean --release    # 清 cache 后全量重编+部署
#   DEVICE_ROOT=/data/thin_agent ADB=/path/to/adb.exe bash deploy.sh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEVICE_ROOT="${DEVICE_ROOT:-/data/thin_agent}"
DIST_DIR="${DIST_DIR:-$ROOT_DIR/dist/aarch64}"
SDK_ENV="${SDK_ENV:-/opt/toolchain/environment-setup-aarch64-oe-linux}"
DO_BUILD=1
DO_CLEAN=0
DO_DIST=0
DO_REBOOT=0
REBOOT_SET=0
WITH_FDBUS=1
FDBUS_EXPLICIT=0
BUILD_TYPE="Release"
BUILD_TYPE_EXPLICIT=0
DEBUG_FLAG=0
RELEASE_FLAG=0
FDBUS_ROOT=""
LEAPTIC_INCLUDE="${LEAPTIC_INCLUDE:-$ROOT_DIR/../leaptic_app/include}"
DEPLOY_APPS=()
# adb: Linux adb 优先；WSL 下回退 Windows adb.exe（见 scripts/resolve_adb.sh）
ADB="$(bash "$ROOT_DIR/scripts/resolve_adb.sh")"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug) DEBUG_FLAG=1; BUILD_TYPE="Debug"; BUILD_TYPE_EXPLICIT=1; shift ;;
    --release) RELEASE_FLAG=1; BUILD_TYPE="Release"; BUILD_TYPE_EXPLICIT=1; shift ;;
    --dist) DO_DIST=1; shift ;;
    --clean) DO_CLEAN=1; shift ;;
    --no-build) DO_BUILD=0; shift ;;
    --with-fdbus) WITH_FDBUS=1; FDBUS_EXPLICIT=1; shift ;;
    --no-fdbus) WITH_FDBUS=0; FDBUS_EXPLICIT=1; shift ;;
    --fdbus-root)
      [[ $# -ge 2 ]] || { echo "missing value for $1" >&2; exit 1; }
      FDBUS_ROOT="$2"
      FDBUS_EXPLICIT=1
      shift 2
      ;;
    --leaptic-include)
      [[ $# -ge 2 ]] || { echo "missing value for $1" >&2; exit 1; }
      LEAPTIC_INCLUDE="$2"
      shift 2
      ;;
    --reboot) DO_REBOOT=1; REBOOT_SET=1; shift ;;
    --no-reboot) DO_REBOOT=0; REBOOT_SET=1; shift ;;
    -a|--app)
      [[ $# -ge 2 ]] || { echo "missing value for $1" >&2; exit 1; }
      DEPLOY_APPS+=("$2")
      shift 2
      ;;
    --app=*)
      DEPLOY_APPS+=("${1#*=}")
      shift
      ;;
    -h|--help)
      sed -n '3,20p' "$0"
      exit 0
      ;;
    *)
      echo "unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

if [[ "$DEBUG_FLAG" -eq 1 && "$RELEASE_FLAG" -eq 1 ]]; then
  echo "ERROR: --debug and --release are mutually exclusive" >&2
  exit 1
fi

if [[ "$DO_CLEAN" -eq 1 && "$DO_BUILD" -eq 0 ]]; then
  echo "ERROR: --clean requires build (do not combine with --no-build)" >&2
  exit 1
fi

cd "$ROOT_DIR"

# 未显式指定时，继承上次 cam build 写入的 .deploy_flags
_SAVED_WITH_FDBUS="$WITH_FDBUS"
_SAVED_BUILD_TYPE="$BUILD_TYPE"
if [[ -f "$ROOT_DIR/build-aarch64/.deploy_flags" ]]; then
  # shellcheck disable=SC1091
  source "$ROOT_DIR/build-aarch64/.deploy_flags"
  [[ "$FDBUS_EXPLICIT" -eq 1 ]] && WITH_FDBUS="$_SAVED_WITH_FDBUS"
  [[ "$BUILD_TYPE_EXPLICIT" -eq 1 ]] && BUILD_TYPE="$_SAVED_BUILD_TYPE"
  BUILD_TYPE="${BUILD_TYPE:-Release}"
  if [[ "$FDBUS_EXPLICIT" -eq 0 || "$BUILD_TYPE_EXPLICIT" -eq 0 ]]; then
    echo ">> inherited build flags: WITH_FDBUS=$WITH_FDBUS BUILD_TYPE=$BUILD_TYPE"
  fi
fi

SINGLE=0
if [[ "${#DEPLOY_APPS[@]}" -gt 0 ]]; then
  SINGLE=1
  [[ "$REBOOT_SET" -eq 1 ]] || DO_REBOOT=0
fi

if [[ "$DO_DIST" -eq 1 && "$SINGLE" -eq 1 ]]; then
  echo "ERROR: --dist does not support --app (full package only)" >&2
  exit 1
fi

if [[ "$DO_BUILD" == "1" ]]; then
  if [[ "$SINGLE" -eq 1 ]]; then
    for app in "${DEPLOY_APPS[@]}"; do
      case "$app" in
        thin_agent|thin_agent_gw) ;;
        *)
          echo "unsupported --app: $app (thin_agent | thin_agent_gw)" >&2
          exit 1
          ;;
      esac
    done
    echo ">> building target(s): ${DEPLOY_APPS[*]} ..."
  else
    echo ">> building cam variant ..."
  fi
  BUILD_ARGS=(cam --leaptic-include "$LEAPTIC_INCLUDE")
  [[ "$DO_CLEAN" -eq 1 ]] && BUILD_ARGS+=(--clean)
  [[ -n "$FDBUS_ROOT" ]] && BUILD_ARGS+=(--fdbus-root "$FDBUS_ROOT")
  if [[ "$WITH_FDBUS" -eq 1 ]]; then
    BUILD_ARGS+=(--with-fdbus)
  else
    BUILD_ARGS+=(--no-fdbus)
  fi
  if [[ "$BUILD_TYPE" == "Debug" ]]; then
    BUILD_ARGS+=(--debug)
  else
    BUILD_ARGS+=(--release)
  fi
  bash "$ROOT_DIR/build.sh" "${BUILD_ARGS[@]}"
fi

AGENT_BIN="$ROOT_DIR/build-aarch64/thin_agent"
GW_BIN="$ROOT_DIR/build-aarch64/thin_agent_gw"
ONNX_LIB_DIR="$ROOT_DIR/third_party/onnxruntime-aarch64/lib"

if [[ -f "$SDK_ENV" ]]; then
  # shellcheck disable=SC1090
  source "$SDK_ENV"
fi
OPENSSL_LIB_DIR="${SDKTARGETSYSROOT:-/opt/toolchain/sysroots/aarch64-oe-linux}/usr/lib"

[[ -x "$AGENT_BIN" ]] || { echo "missing binary: $AGENT_BIN" >&2; exit 2; }
if [[ "$SINGLE" -eq 0 ]]; then
  [[ -x "$GW_BIN" ]] || { echo "missing binary: $GW_BIN" >&2; exit 2; }
  [[ -d "$ONNX_LIB_DIR" ]] || { echo "missing onnxruntime libs: $ONNX_LIB_DIR" >&2; exit 2; }
fi

resolve_src_file() {
  local src="$1"
  if [[ -L "$src" ]]; then
    src="$(readlink -f "$src")"
  fi
  echo "$src"
}

copy_dist_file() {
  local src="$1"
  local dst="$2"
  src="$(resolve_src_file "$src")"
  [[ -f "$src" ]] || { echo "missing copy source: $src" >&2; exit 2; }
  cp "$src" "$dst"
  chmod 755 "$dst" 2>/dev/null || true
}

copy_dist_libs() {
  local src_dir="$1"
  local pattern="$2"
  local dest_dir="$3"
  local so
  for so in "$src_dir"/$pattern; do
    [[ -f "$so" ]] || continue
    copy_dist_file "$so" "$dest_dir/$(basename "$so")"
  done
}

package_dist() {
  echo ">> packaging offline dist -> $DIST_DIR"
  rm -rf "$DIST_DIR"
  mkdir -p "$DIST_DIR/bin" "$DIST_DIR/lib" "$DIST_DIR/config" "$DIST_DIR/models/intent"

  local agent_dst="$DIST_DIR/bin/thin_agent"
  local gw_dst="$DIST_DIR/bin/thin_agent_gw"
  copy_dist_file "$AGENT_BIN" "$agent_dst"
  copy_dist_file "$GW_BIN" "$gw_dst"
  if [[ "$BUILD_TYPE" == "Release" ]]; then
    aarch64-oe-linux-strip "$agent_dst" 2>/dev/null || strip "$agent_dst" 2>/dev/null || true
    aarch64-oe-linux-strip "$gw_dst" 2>/dev/null || strip "$gw_dst" 2>/dev/null || true
  fi

  copy_dist_libs "$ONNX_LIB_DIR" "libonnxruntime.so*" "$DIST_DIR/lib"

  CURL_LIB_DIR="${SDKTARGETSYSROOT:-/opt/toolchain/sysroots/aarch64-oe-linux}/usr/lib"
  copy_dist_libs "$CURL_LIB_DIR" "libcurl.so.4*" "$DIST_DIR/lib"

  for base in libssl.so.3 libcrypto.so.3; do
    copy_dist_libs "$OPENSSL_LIB_DIR" "${base}*" "$DIST_DIR/lib"
  done

  if [[ "$WITH_FDBUS" -eq 1 ]]; then
    FDBUS_LIB_DIR="${FDBUS_ROOT:-${SDKTARGETSYSROOT:-/opt/toolchain/sysroots/aarch64-oe-linux}/usr}/lib"
    for base in libfdbus-clib.so libfdbus.so; do
      copy_dist_libs "$FDBUS_LIB_DIR" "${base}*" "$DIST_DIR/lib"
    done
  fi

  copy_dist_file "$ROOT_DIR/config/chat_policy.json" "$DIST_DIR/config/chat_policy.json"
  copy_dist_file "$ROOT_DIR/models/intent/intent_multiclass.onnx" "$DIST_DIR/models/intent/intent_multiclass.onnx"
  copy_dist_file "$ROOT_DIR/models/intent/vocab.txt" "$DIST_DIR/models/intent/vocab.txt"
  copy_dist_file "$ROOT_DIR/scripts/run_agent.sh" "$DIST_DIR/run_agent.sh"
  copy_dist_file "$ROOT_DIR/scripts/run_gateway.sh" "$DIST_DIR/run_gateway.sh"

  local file_desc bin_size bin_mb
  file_desc="$(file "$DIST_DIR/bin/thin_agent")"
  bin_size="$(stat -c %s "$DIST_DIR/bin/thin_agent")"
  bin_mb="$(awk -v b="$bin_size" 'BEGIN{printf "%.2f", b/1024/1024}')"

  echo "════════════════════════════════════════"
  echo "[dist] DONE"
  echo "  root:   $DIST_DIR"
  echo "  binary: $DIST_DIR/bin/thin_agent"
  echo "  arch:   $file_desc"
  echo "  size:   ${bin_size} bytes (${bin_mb} MB)"
  echo "  run on device:"
  echo "    cd dist/aarch64 && ./run_agent.sh --local"
  echo "    ./run_gateway.sh"
  echo "  config: bash config.sh [--cloud-llm] [--with-feishu]  # copy demo.model.yaml + zai.env as needed"
  echo "════════════════════════════════════════"
}

if [[ "$DO_DIST" -eq 1 ]]; then
  package_dist
  exit 0
fi

echo ">> preparing device (using: $ADB) ..."
"$ADB" wait-for-device
"$ADB" root || true
sleep 1
"$ADB" wait-for-device
"$ADB" shell "mount -o remount,rw / || true"
"$ADB" shell "mkdir -p '$DEVICE_ROOT/bin' '$DEVICE_ROOT/lib' '$DEVICE_ROOT/config' '$DEVICE_ROOT/models/intent' '$DEVICE_ROOT/data'"

push_file() {
  local src="$1"
  local dst="$2"
  local name
  # WSL 下 Windows adb.exe 无法 push 符号链接，解析为真实文件
  if [[ -L "$src" ]]; then
    src="$(readlink -f "$src")"
  fi
  [[ -f "$src" ]] || { echo "missing push source: $src" >&2; exit 2; }
  name="$(basename "$dst")"
  "$ADB" push "$src" "/tmp/$name" >/dev/null
  "$ADB" shell "mv /tmp/$name '$dst' && chmod 755 '$dst'"
}

push_script() {
  local src="$1"
  local dst="$2"
  local name
  name="$(basename "$dst")"
  "$ADB" push "$src" "/tmp/$name" >/dev/null
  "$ADB" shell "mv /tmp/$name '$dst' && chmod 755 '$dst'"
}

if [[ "$SINGLE" -eq 1 ]]; then
  echo ">> pushing app binaries ..."
  for app in "${DEPLOY_APPS[@]}"; do
    case "$app" in
      thin_agent) push_file "$AGENT_BIN" "$DEVICE_ROOT/bin/thin_agent" ;;
      thin_agent_gw) push_file "$GW_BIN" "$DEVICE_ROOT/bin/thin_agent_gw" ;;
    esac
  done
  "$ADB" shell sync
  if [[ "$DO_REBOOT" == "1" ]]; then
    echo ">> rebooting device ..."
    "$ADB" reboot
  fi
  echo ">> done."
  echo "   app-only mode: updated ${DEPLOY_APPS[*]} under $DEVICE_ROOT/bin/"
  echo "   agent: adb shell '$DEVICE_ROOT/run_agent.sh --local'"
  echo "   gateway: adb shell '$DEVICE_ROOT/run_gateway.sh'"
  exit 0
fi

echo ">> pushing runtime files ..."
push_file "$AGENT_BIN" "$DEVICE_ROOT/bin/thin_agent"
push_file "$GW_BIN" "$DEVICE_ROOT/bin/thin_agent_gw"

for so in "$ONNX_LIB_DIR"/libonnxruntime.so*; do
  [[ -e "$so" ]] || continue
  push_file "$so" "$DEVICE_ROOT/lib/$(basename "$so")"
done

# thin_agent / thin_agent_gw 动态链接 libcurl（设备无 curl 命令时仍需 libcurl.so）
CURL_LIB_DIR="${SDKTARGETSYSROOT:-/opt/toolchain/sysroots/aarch64-oe-linux}/usr/lib"
for base in libcurl.so.4; do
  for so in "$CURL_LIB_DIR"/${base}*; do
    [[ -f "$so" ]] || continue
    push_file "$so" "$DEVICE_ROOT/lib/$(basename "$so")"
  done
done

# thin_agent_gw 动态链接 libssl/libcrypto（设备系统库版本不一致时 bundled 到 lib/）
for base in libssl.so.3 libcrypto.so.3; do
  for so in "$OPENSSL_LIB_DIR"/${base}*; do
    [[ -f "$so" ]] || continue
    push_file "$so" "$DEVICE_ROOT/lib/$(basename "$so")"
  done
done

# FDBus 运行时库（WITH_FDBUS 构建时需要）
if [[ "$WITH_FDBUS" -eq 1 ]]; then
  FDBUS_LIB_DIR="${FDBUS_ROOT:-${SDKTARGETSYSROOT:-/opt/toolchain/sysroots/aarch64-oe-linux}/usr}/lib"
  for base in libfdbus-clib.so libfdbus.so; do
    for so in "$FDBUS_LIB_DIR"/${base}*; do
      [[ -f "$so" ]] || continue
      push_file "$so" "$DEVICE_ROOT/lib/$(basename "$so")"
    done
  done
fi

push_file "$ROOT_DIR/config/chat_policy.json" "$DEVICE_ROOT/config/chat_policy.json"
push_file "$ROOT_DIR/models/intent/intent_multiclass.onnx" "$DEVICE_ROOT/models/intent/intent_multiclass.onnx"
push_file "$ROOT_DIR/models/intent/vocab.txt" "$DEVICE_ROOT/models/intent/vocab.txt"

push_script "$ROOT_DIR/scripts/run_agent.sh" "$DEVICE_ROOT/run_agent.sh"
push_script "$ROOT_DIR/scripts/run_gateway.sh" "$DEVICE_ROOT/run_gateway.sh"

"$ADB" shell sync
if [[ "$DO_REBOOT" == "1" ]]; then
  echo ">> rebooting device ..."
  "$ADB" reboot
fi

echo ">> done."
echo "   device root: $DEVICE_ROOT"
echo "   config:  bash config.sh [--cloud-llm] [--with-feishu]"
echo "   agent:   adb shell '$DEVICE_ROOT/run_agent.sh --local'"
echo "   gateway: adb shell '$DEVICE_ROOT/run_gateway.sh'"
