# 启动本地 llama-server（OpenAI 兼容，默认监听 1234），供 thin_agent lmstudio_mode 通道使用。
# 用法：
#   powershell -ExecutionPolicy Bypass -File script\windows\start-llama-server.ps1
#   powershell -ExecutionPolicy Bypass -File script\windows\start-llama-server.ps1 -Port 1234 -GpuLayers 99
param(
  [string]$Model = "",
  [string]$HostAddr = "127.0.0.1",
  [int]$Port = 1234,
  [int]$CtxSize = 4096,
  [int]$GpuLayers = 99,
  [string]$LlamaServer = "",
  [switch]$Stop
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$DefaultModel = Join-Path $Root "backends\thin_agent\models\gguf\aurum-brain-q4_k_m.gguf"
$DefaultBinCandidates = @(
  "E:\deps-thin-agent\llama.cpp\bin\llama-server.exe",
  "C:\deps-thin-agent\llama.cpp\bin\llama-server.exe",
  (Join-Path $Root "deps\llama.cpp\bin\llama-server.exe"),
  (Join-Path $env:LOCALAPPDATA "llama.cpp\bin\llama-server.exe")
)

if (-not $Model) { $Model = $DefaultModel }
if (-not $LlamaServer) {
  foreach ($c in $DefaultBinCandidates) {
    if (Test-Path $c) { $LlamaServer = $c; break }
  }
}

function Test-SamePath([string]$A, [string]$B) {
  if (-not $A -or -not $B) { return $false }
  return ([IO.Path]::GetFullPath($A) -ieq [IO.Path]::GetFullPath($B))
}

function Get-MatchingProcess([string]$ExePath) {
  Get-CimInstance Win32_Process -Filter "Name = '$((Split-Path -Leaf $ExePath))'" |
    Where-Object { Test-SamePath $_.ExecutablePath $ExePath }
}

if ($Stop) {
  if (-not $LlamaServer -or -not (Test-Path $LlamaServer)) {
    Write-Host "未找到 llama-server.exe，无法按路径停止。"
    exit 1
  }
  $procs = @(Get-MatchingProcess $LlamaServer)
  foreach ($p in $procs) {
    Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
  }
  Write-Host "已停止匹配的 llama-server。"
  exit 0
}

if (-not $LlamaServer -or -not (Test-Path $LlamaServer)) {
  Write-Host "找不到 llama-server.exe。"
  Write-Host "请用 -LlamaServer 指定，或安装到 E:\deps-thin-agent\llama.cpp\bin\"
  exit 1
}
if (-not (Test-Path $Model)) {
  Write-Host "找不到 GGUF 模型：$Model"
  Write-Host "请把 aurum-brain-q4_k_m.gguf 放到 backends\thin_agent\models\gguf\，或用 -Model 指定路径。"
  exit 1
}

$listen = "http://${HostAddr}:${Port}"
try {
  $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 1 "$listen/v1/models"
  if ($r.StatusCode -eq 200) {
    Write-Host "本地端点已在监听 $listen ，跳过启动。"
    exit 0
  }
} catch {}

Write-Host "启动 llama-server"
Write-Host "  bin   : $LlamaServer"
Write-Host "  model : $Model"
Write-Host "  listen: $listen"
Write-Host "  ctx   : $CtxSize  ngl=$GpuLayers"

Start-Process -FilePath $LlamaServer -ArgumentList @(
  "-m", $Model,
  "--host", $HostAddr,
  "--port", "$Port",
  "-c", "$CtxSize",
  "-ngl", "$GpuLayers"
) -WorkingDirectory (Split-Path -Parent $LlamaServer)

Write-Host "已后台启动。thin_agent 侧边栏切到「本地 Aurum（lmstudio）」即可热切换使用。"
Write-Host "健康检查: $listen/v1/models"
