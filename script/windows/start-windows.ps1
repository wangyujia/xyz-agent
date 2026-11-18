# 在 Windows 上启动 thin_agent，并打开 zing_agent。
# 双击 script\windows\start-windows.bat，或：
#   powershell -ExecutionPolicy Bypass -File script\windows\start-windows.ps1
#   powershell -ExecutionPolicy Bypass -File script\windows\start-windows.ps1 -AgentProfile deepseek_fast_demo
#   powershell -ExecutionPolicy Bypass -File script\windows\start-windows.ps1 -NoGui
#   powershell -ExecutionPolicy Bypass -File script\windows\start-windows.ps1 -Stop
param(
  [Alias("Profile")]
  [string]$AgentProfile = "offline_demo",
  [int]$Port = 8765,
  [string]$BindHost = "127.0.0.1",
  [switch]$NoGui,
  [switch]$Stop
)

$ErrorActionPreference = "Stop"
# 脚本在 script\windows，仓库根在上两级。
$Root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$AgentRoot = Join-Path $Root "backends\thin_agent"
$AgentExe = Join-Path $AgentRoot "build-win\Release\thin_agent.exe"
$GuiExe = Join-Path $Root "frontends\zing_agent\build-win\Release\zing_agent.exe"
$RepoConfig = Join-Path $AgentRoot "config\demo.model.yaml"
$RepoPolicy = Join-Path $AgentRoot "config\chat_policy.json"
$OnnxDll = Join-Path $AgentRoot "third_party\onnxruntime\lib\onnxruntime.dll"
$Log = Join-Path $env:USERPROFILE ".thin_agent\logs\agent_svc.log"

function Test-SamePath([string]$A, [string]$B) {
  if (-not $A -or -not $B) { return $false }
  return ([IO.Path]::GetFullPath($A) -ieq [IO.Path]::GetFullPath($B))
}

function Get-MatchingProcess([string]$ExePath) {
  Get-CimInstance Win32_Process -Filter "Name = '$((Split-Path -Leaf $ExePath))'" |
    Where-Object { Test-SamePath $_.ExecutablePath $ExePath }
}

function Test-AgentHealth {
  try {
    $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "http://${BindHost}:${Port}/health"
    return ($r.StatusCode -eq 200 -and $r.Content -match '"ok"\s*:\s*true')
  } catch {
    return $false
  }
}

function Test-WebView2 {
  $guid = "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"
  $keys = @(
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\$guid",
    "HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$guid",
    "HKCU:\SOFTWARE\Microsoft\EdgeUpdate\Clients\$guid"
  )
  foreach ($k in $keys) {
    $p = Get-ItemProperty -Path $k -ErrorAction SilentlyContinue
    if ($p -and $p.pv) { return $true }
  }
  return $false
}

if (-not (Test-Path $AgentExe)) {
  Write-Host "找不到 $AgentExe"
  Write-Host "先在 backends\thin_agent 里用 Visual Studio 编出 Release 的 thin_agent。"
  exit 1
}

if ($Stop) {
  $procs = @(Get-MatchingProcess $AgentExe)
  if ($procs.Count -eq 0) {
    Write-Host "thin_agent 没有在运行。"
  } else {
    $procs | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
    Write-Host "已停止 thin_agent。"
  }
  $guis = @(Get-MatchingProcess $GuiExe)
  if ($guis.Count -gt 0) {
    $guis | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
    Write-Host "已关闭 zing_agent。"
  }
  exit 0
}

$dllDst = Join-Path (Split-Path $AgentExe) "onnxruntime.dll"
if (-not (Test-Path $dllDst)) {
  if (-not (Test-Path $OnnxDll)) {
    Write-Host "缺少 onnxruntime.dll：$OnnxDll"
    exit 1
  }
  Copy-Item $OnnxDll $dllDst
}

$HomeConfig = Join-Path $env:USERPROFILE ".thin_agent\demo.model.yaml"
$Config = $RepoConfig
if (Test-Path $HomeConfig) { $Config = $HomeConfig }

$PolicyDir = Join-Path $env:USERPROFILE ".thin_agent\profiles\$AgentProfile\config"
$PolicyDst = Join-Path $PolicyDir "chat_policy.json"
if ((Test-Path $RepoPolicy) -and -not (Test-Path $PolicyDst)) {
  New-Item -ItemType Directory -Force -Path $PolicyDir | Out-Null
  Copy-Item $RepoPolicy $PolicyDst
  Write-Host "已复制聊天策略到 $PolicyDst"
}

$already = Test-AgentHealth
if ($already) {
  Write-Host "后端已在 http://${BindHost}:${Port}/health"
} else {
  Write-Host "启动 thin_agent  profile=$AgentProfile  config=$Config"
  Start-Process -FilePath $AgentExe -WorkingDirectory $AgentRoot -WindowStyle Hidden -ArgumentList @(
    "--host", $BindHost,
    "--port", "$Port",
    "--config", $Config,
    "--profile", $AgentProfile
  )
  $ok = $false
  for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 500
    if (Test-AgentHealth) { $ok = $true; break }
  }
  if (-not $ok) {
    Write-Host "后端没有在 15 秒内就绪。日志：$Log"
    if (Test-Path $Log) {
      Write-Host "----- 日志末尾 -----"
      Get-Content $Log -Tail 30
    }
    exit 1
  }
  Write-Host "后端就绪  ws://${BindHost}:${Port}/ws"
}

if ($NoGui) { exit 0 }

if (-not (Test-Path $GuiExe)) {
  Write-Host "找不到 $GuiExe ，后端已在跑。前端编好后重新执行本脚本即可。"
  exit 1
}
if (-not (Test-WebView2)) {
  Write-Host "本机没有 WebView2 运行时，zing_agent 窗口打不开。"
  Write-Host "安装 Evergreen 引导程序后再开前端：https://go.microsoft.com/fwlink/p/?LinkId=2124703"
  exit 1
}

$env:ZING_SERVICE_BIN = $AgentExe
$env:ZING_SERVICE_CONFIG = $Config
$env:ZING_SERVICE_PROFILE = $AgentProfile
$env:ZING_SERVICE_PORT = "$Port"
Start-Process -FilePath $GuiExe -WorkingDirectory $AgentRoot
Write-Host "已打开 zing_agent。默认连接 ws://${BindHost}:${Port}/ws"
Write-Host "日志：$Log"
Write-Host "停止： script\windows\start-windows.bat -Stop"
