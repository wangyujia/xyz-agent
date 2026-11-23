# 以 router 启动本地 llama-server（OpenAI 兼容，默认监听 1234）。
# --models-max 1：目录里的模型只登记，请求到哪一个才载入哪一个。
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

function Get-WritableFixedDrives {
  # 盘符从小到大。固定盘且已就绪；只读卷排除。
  @(
    [System.IO.DriveInfo]::GetDrives() |
      Where-Object { $_.DriveType -eq [System.IO.DriveType]::Fixed -and $_.IsReady } |
      ForEach-Object { $_.Name.Substring(0, 2) } |
      Sort-Object
  )
}

function Get-DefaultLlamaServerCandidates {
  $suffix = "deps-thin-agent\llama.cpp\bin\llama-server.exe"
  $drives = @(Get-WritableFixedDrives)
  $cands = @()
  for ($i = $drives.Count - 1; $i -ge 0; $i--) {
    $cands += (Join-Path ($drives[$i] + "\") $suffix)
  }
  $cands += (Join-Path $Root "deps\llama.cpp\bin\llama-server.exe")
  $cands += (Join-Path $env:LOCALAPPDATA "llama.cpp\bin\llama-server.exe")
  return @($cands)
}

$DefaultBinCandidates = @(Get-DefaultLlamaServerCandidates)

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
  $hint = $DefaultBinCandidates | Select-Object -First 1
  Write-Host "请用 -LlamaServer 指定，或安装到最后一个可写盘：$hint"
  exit 1
}
$ModelsDir = Join-Path $Root "backends\thin_agent\models\gguf"
if (-not (Test-Path $ModelsDir)) {
  Write-Host "找不到模型目录：$ModelsDir"
  exit 1
}

$known = @(
  @{ id = "aurum-brain-q4_k_m"; file = "aurum-brain-q4_k_m.gguf"; alias = "aurum-brain"; ctx = 32768 },
  @{ id = "qwen2.5-coder-7b-instruct-q4_k_m"; file = "qwen2.5-coder-7b-instruct-q4_k_m.gguf"; alias = ""; ctx = 16384 },
  @{ id = "qwen2.5-coder-14b-instruct-q4_k_m"; file = "qwen2.5-coder-14b-instruct-q4_k_m.gguf"; alias = ""; ctx = 16384 },
  @{ id = "qwen2.5-0.5b-instruct-q2_k"; file = "qwen2.5-0.5b-instruct-q2_k.gguf"; alias = ""; ctx = 32768 },
  @{ id = "gemma-3-1b-it-q4_k_m"; file = "gemma-3-1b-it-q4_k_m.gguf"; alias = ""; ctx = 32768 }
)
$presetLines = @()
$presetCount = 0
foreach ($spec in $known) {
  $file = Join-Path $ModelsDir $spec.file
  if (-not (Test-Path $file)) { continue }
  $presetLines += "[" + $spec.id + "]"
  $presetLines += "ctx-size = " + $spec.ctx
  $presetLines += "model = " + $file
  if ($spec.alias) { $presetLines += "alias = " + $spec.alias }
  if ($GpuLayers -ge 0) { $presetLines += "n-gpu-layers = " + $GpuLayers }
  $presetLines += ""
  $presetCount++
}
if ($presetCount -eq 0) {
  Write-Host "模型目录里没有可用的 GGUF：$ModelsDir"
  exit 1
}
$Preset = Join-Path $ModelsDir "router-presets.ini"
Set-Content -Path $Preset -Value $presetLines -Encoding ascii

$listen = "http://${HostAddr}:${Port}"
try {
  $props = Invoke-WebRequest -UseBasicParsing -TimeoutSec 1 "http://${HostAddr}:${Port}/props"
  if ($props.Content -match '"role"\s*:\s*"router"') {
    Write-Host "router 已在监听 $listen ，跳过启动。"
    exit 0
  }
  Write-Host "端口 $Port 上已有单模型 llama-server。请先 -Stop，再启动 router。"
  exit 1
} catch {}

Write-Host "启动 llama-server router"
Write-Host "  bin    : $LlamaServer"
Write-Host "  models : $ModelsDir"
Write-Host "  preset : $Preset"
Write-Host "  listen : $listen"
Write-Host "  max    : 1  ngl=$GpuLayers"

Start-Process -FilePath $LlamaServer -ArgumentList @(
  "--models-dir", $ModelsDir,
  "--models-preset", $Preset,
  "--models-max", "1",
  "--host", $HostAddr,
  "--port", "$Port",
  "--jinja"
) -WorkingDirectory (Split-Path -Parent $LlamaServer)

Write-Host "已后台启动。模型保持 unloaded，选中后才载入当前这一份。"
Write-Host "健康检查: $listen/v1/models"
