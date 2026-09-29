#!/usr/bin/env bash
# 解析 adb 可执行文件路径（Linux / macOS / WSL）。
#
# WSL 下原生 adb 常不可用或连不上设备，优先回退到 Windows 的 adb.exe：
#   command -v adb.exe
#   /mnt/c/Users/<user>/AppData/Local/Android/Sdk/platform-tools/adb.exe
#
# 也可手动指定: ADB=/path/to/adb.exe bash deploy.sh
set -euo pipefail

if [[ -n "${ADB:-}" ]]; then
  echo "$ADB"
  exit 0
fi

if command -v adb >/dev/null 2>&1; then
  command -v adb
  exit 0
fi

if command -v adb.exe >/dev/null 2>&1; then
  command -v adb.exe
  exit 0
fi

# WSL: 扫描常见 Windows Android SDK 安装路径
if [[ -d /mnt/c/Users ]]; then
  local_candidates=()
  if [[ -n "${USER:-}" ]]; then
    local_candidates+=("/mnt/c/Users/${USER}/AppData/Local/Android/Sdk/platform-tools/adb.exe")
  fi
  if [[ -n "${WSLENV_USERPROFILE:-}" ]]; then
    local_candidates+=("${WSLENV_USERPROFILE}/AppData/Local/Android/Sdk/platform-tools/adb.exe")
  fi
  for candidate in "${local_candidates[@]}"; do
    if [[ -x "$candidate" ]]; then
      echo "$candidate"
      exit 0
    fi
  done
  shopt -s nullglob
  for candidate in /mnt/c/Users/*/AppData/Local/Android/Sdk/platform-tools/adb.exe; do
    if [[ -x "$candidate" ]]; then
      echo "$candidate"
      exit 0
    fi
  done
  shopt -u nullglob
fi

echo adb
