#!/usr/bin/env bash
# thin_agent 统一构建入口
#
# 用法:
#   bash build.sh              # 默认 pc（本机 x86，FakeDevice）
#   bash build.sh pc --clean
#   bash build.sh cam            # 设备 aarch64（默认 FDBus ON）
#   bash build.sh cam --no-fdbus # 设备版但不链 FDBus
#   bash build.sh cam --fdbus-root /path/to/fdbus --leaptic-include /path/to/leaptic_app/include
#   bash build.sh cam --debug          # Debug 构建（默认 Release）
#   bash build.sh pc --release         # 显式 Release（默认）
#   缺 third_party/onnxruntime-aarch64 时 cam 会自动下载（仅首次）
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

ensure_onnxruntime_aarch64() {
  local onnx_ver="${ONNX_VER:-1.18.1}"
  local onnx_dir="$ROOT_DIR/third_party/onnxruntime-aarch64"
  if [[ -f "$onnx_dir/lib/libonnxruntime.so.$onnx_ver" ]]; then
    return 0
  fi
  if [[ -f "$onnx_dir/lib/libonnxruntime.so" && -d "$onnx_dir/include" ]]; then
    return 0
  fi
  echo "[build] fetching onnxruntime-linux-aarch64-${onnx_ver} ..."
  local tmp
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN
  curl -fsSL -o "$tmp/onnx.tgz" \
    "https://github.com/microsoft/onnxruntime/releases/download/v${onnx_ver}/onnxruntime-linux-aarch64-${onnx_ver}.tgz"
  tar -xzf "$tmp/onnx.tgz" -C "$tmp"
  rm -rf "$onnx_dir"
  mkdir -p "$onnx_dir"
  cp -a "$tmp/onnxruntime-linux-aarch64-${onnx_ver}/lib" \
        "$tmp/onnxruntime-linux-aarch64-${onnx_ver}/include" \
        "$onnx_dir/"
  echo "[build] onnxruntime-aarch64 installed under third_party/"
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
  exit 0
fi

MODE="${1:-pc}"
shift || true

CLEAN=0
WITH_FDBUS=1
FDBUS_ROOT=""
LEAPTIC_INCLUDE="${LEAPTIC_INCLUDE:-$ROOT_DIR/../leaptic_app/include}"
DEPLOY_FLAGS_FILE=""
SDK_ENV="${SDK_ENV:-/opt/toolchain/environment-setup-aarch64-oe-linux}"
BUILD_TYPE="Release"
DEBUG_FLAG=0
RELEASE_FLAG=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug) DEBUG_FLAG=1; shift ;;
    --release) RELEASE_FLAG=1; shift ;;
    --clean) CLEAN=1; shift ;;
    --with-fdbus) WITH_FDBUS=1; shift ;;
    --no-fdbus) WITH_FDBUS=0; shift ;;
    --fdbus-root)
      [[ $# -ge 2 ]] || { echo "ERROR: --fdbus-root requires value" >&2; exit 2; }
      FDBUS_ROOT="$2"
      shift 2
      ;;
    --leaptic-include)
      [[ $# -ge 2 ]] || { echo "ERROR: --leaptic-include requires value" >&2; exit 2; }
      LEAPTIC_INCLUDE="$2"
      shift 2
      ;;
    -h|--help)
      sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "ERROR: unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ "$DEBUG_FLAG" -eq 1 && "$RELEASE_FLAG" -eq 1 ]]; then
  echo "ERROR: --debug and --release are mutually exclusive" >&2
  exit 2
fi
[[ "$DEBUG_FLAG" -eq 1 ]] && BUILD_TYPE="Debug"

cd "$ROOT_DIR"
echo ">> build_type=$BUILD_TYPE mode=$MODE"

if [[ "$MODE" == "pc" ]]; then
  # 避免当前 shell 已 source 交叉 SDK 时误用 OE 工具链 / sysroot 库
  unset CMAKE_TOOLCHAIN_FILE OE_CMAKE_TOOLCHAIN_FILE
  unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR CONFIG_SITE CMAKE_PREFIX_PATH
  unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
  unset CC CXX CPP LD AR AS STRIP RANLIB OBJCOPY OBJDUMP NM
  unset OECORE_TARGET_SYSROOT SDKTARGETSYSROOT
  [[ "$CLEAN" -eq 1 ]] && rm -rf build
  cmake -S . -B build \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DTHIN_AGENT_EMBEDDED_CAM=OFF \
    -DTHIN_AGENT_BUILD_TESTS=ON \
    -DTHIN_AGENT_BUILD_IM_GATEWAY=ON \
    -DTHIN_AGENT_WITH_FDBUS=OFF
  cmake --build build -j"$(nproc)" --target thin_agent thin_agent_gw thin_agent_demo
  echo "[build] pc done: build/{thin_agent,thin_agent_gw,thin_agent_demo} (build_type=$BUILD_TYPE)"
  exit 0
fi

if [[ "$MODE" == "cam" ]]; then
  [[ -f "$SDK_ENV" ]] || { echo "ERROR: SDK env not found: $SDK_ENV" >&2; exit 2; }
  ensure_onnxruntime_aarch64
  # shellcheck disable=SC1090
  source "$SDK_ENV"
  if [[ "$BUILD_TYPE" == "Release" ]]; then
    unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
  fi
  [[ "$CLEAN" -eq 1 ]] && rm -rf build-aarch64

  CMAKE_ARGS=(
    -S . -B build-aarch64
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/toolchain-aarch64-oe.cmake"
    -DTHIN_AGENT_EMBEDDED_CAM=ON
    -DTHIN_AGENT_BUILD_TESTS=OFF
    -DTHIN_AGENT_BUILD_IM_GATEWAY=ON
  )
  if [[ "$WITH_FDBUS" -eq 1 ]]; then
    [[ -n "$FDBUS_ROOT" ]] || FDBUS_ROOT="${SDKTARGETSYSROOT:-}/usr"
    [[ -d "$LEAPTIC_INCLUDE" ]] || {
      echo "ERROR: LEAPTIC_INCLUDE not found: $LEAPTIC_INCLUDE" >&2
      exit 2
    }
    CMAKE_ARGS+=(
      -DTHIN_AGENT_WITH_FDBUS=ON
      -DFDBUS_ROOT="$FDBUS_ROOT"
      -DLEAPTIC_INCLUDE="$LEAPTIC_INCLUDE"
    )
  else
    CMAKE_ARGS+=(-DTHIN_AGENT_WITH_FDBUS=OFF)
  fi
  cmake "${CMAKE_ARGS[@]}"
  cmake --build build-aarch64 -j"$(nproc)" --target thin_agent thin_agent_gw
  DEPLOY_FLAGS_FILE="build-aarch64/.deploy_flags"
  {
    echo "WITH_FDBUS=$WITH_FDBUS"
    echo "BUILD_TYPE=$BUILD_TYPE"
    [[ -n "$FDBUS_ROOT" ]] && echo "FDBUS_ROOT=$FDBUS_ROOT"
    echo "LEAPTIC_INCLUDE=$LEAPTIC_INCLUDE"
  } > "$DEPLOY_FLAGS_FILE"
  echo "[build] cam done: build-aarch64/{thin_agent,thin_agent_gw} (build_type=$BUILD_TYPE, WITH_FDBUS=$WITH_FDBUS)"
  exit 0
fi

echo "ERROR: mode must be pc or cam" >&2
exit 2
