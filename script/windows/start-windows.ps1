# 在 Windows 上启动 thin_agent，并打开 zing_agent。
# 双击 script\windows\start-windows.bat，或：
#   powershell -ExecutionPolicy Bypass -File script\windows\start-windows.ps1
#   powershell -ExecutionPolicy Bypass -File script\windows\start-windows.ps1 -AgentProfile deepseek_fast_demo
#   powershell -ExecutionPolicy Bypass -File script\windows\start-windows.ps1 -NoGui
#   powershell -ExecutionPolicy Bypass -File script\windows\start-windows.ps1 -Stop
param(
  [Alias("Profile")]
  [string]$AgentProfile = "local_tools_mode",
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

function Test-YamlHasProfile([string]$YamlPath, [string]$Profile) {
  if (-not (Test-Path $YamlPath)) { return $false }
  $pat = '^\s{2}' + [regex]::Escape($Profile) + '\s*:'
  return $null -ne (Select-String -Path $YamlPath -Pattern $pat -SimpleMatch:$false | Select-Object -First 1)
}

function Get-YamlScalar([string]$YamlPath, [string]$Profile, [string]$Key) {
  if (-not (Test-Path $YamlPath)) { return $null }
  $lines = Get-Content -Path $YamlPath -ErrorAction SilentlyContinue
  $inProfiles = $false
  $inTarget = $false
  foreach ($line in $lines) {
    if ($line -match '^profiles\s*:') { $inProfiles = $true; continue }
    if (-not $inProfiles) { continue }
    if ($line -match ('^\s{2}' + [regex]::Escape($Profile) + '\s*:')) {
      $inTarget = $true
      continue
    }
    if ($inTarget -and $line -match '^\s{2}\S') { break }
    if ($inTarget -and $line -match ('^\s{4}' + [regex]::Escape($Key) + '\s*:\s*(.*)$')) {
      $v = $Matches[1].Trim()
      if ($v -match '\s+#') { $v = ($v -split '\s+#', 2)[0].Trim() }
      if (($v.StartsWith('"') -and $v.EndsWith('"')) -or ($v.StartsWith("'") -and $v.EndsWith("'"))) {
        $v = $v.Substring(1, $v.Length - 2)
      }
      return $v
    }
  }
  return $null
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

# 家目录旧 YAML 可能没有 *_mode 键：缺 profile 时优先改用仓库配置。
$profileAliases = @{
  "offline_mode" = @("offline_mode", "offline_demo")
  "offline_demo" = @("offline_mode", "offline_demo")
  "local_tools_mode" = @("local_tools_mode", "local_tools_demo")
  "local_tools_demo" = @("local_tools_mode", "local_tools_demo")
  "lmstudio_mode" = @("lmstudio_mode", "lmstudio_demo")
  "lmstudio_demo" = @("lmstudio_mode", "lmstudio_demo")
  "cloud_api_mode" = @("cloud_api_mode", "openai_compatible_demo")
  "openai_compatible_demo" = @("cloud_api_mode", "openai_compatible_demo")
}
function Test-ConfigHasAccessProfile([string]$YamlPath, [string]$Profile) {
  $cands = @($Profile)
  if ($profileAliases.ContainsKey($Profile)) { $cands = $profileAliases[$Profile] }
  foreach ($c in $cands) {
    if (Test-YamlHasProfile $YamlPath $c) { return $true }
  }
  return $false
}

if (-not (Test-ConfigHasAccessProfile $Config $AgentProfile)) {
  if ((Test-Path $RepoConfig) -and -not (Test-SamePath $Config $RepoConfig) -and
      (Test-ConfigHasAccessProfile $RepoConfig $AgentProfile)) {
    Write-Host "配置 $Config 缺少 profile '$AgentProfile'，改用仓库配置 $RepoConfig"
    $Config = $RepoConfig
  } elseif ($AgentProfile -eq "local_tools_mode" -or $AgentProfile -eq "local_tools_demo") {
    if (Test-ConfigHasAccessProfile $Config "offline_mode") {
      Write-Host "配置缺少 local_tools_mode，回退 offline_mode"
      $AgentProfile = "offline_mode"
    } elseif ((Test-Path $RepoConfig) -and (Test-ConfigHasAccessProfile $RepoConfig "offline_mode")) {
      Write-Host "配置缺少可用访问模式，改用仓库 offline_mode"
      $Config = $RepoConfig
      $AgentProfile = "offline_mode"
    } else {
      Write-Host "配置中找不到 local_tools_mode / offline_mode，无法启动。"
      exit 1
    }
  }
}

# 规范化脚本侧默认名（与 C++ canonicalize 对齐）
switch ($AgentProfile) {
  "offline_demo" { $AgentProfile = "offline_mode" }
  "local_tools_demo" { $AgentProfile = "local_tools_mode" }
  "lmstudio_demo" { $AgentProfile = "lmstudio_mode" }
  "openai_compatible_demo" { $AgentProfile = "cloud_api_mode" }
}

function Get-DefaultLlamaServerPath {
  $suffix = "deps-thin-agent\llama.cpp\bin\llama-server.exe"
  $drives = @(
    [System.IO.DriveInfo]::GetDrives() |
      Where-Object { $_.DriveType -eq [System.IO.DriveType]::Fixed -and $_.IsReady } |
      ForEach-Object { $_.Name.Substring(0, 2) } |
      Sort-Object
  )
  for ($i = $drives.Count - 1; $i -ge 0; $i--) {
    $p = Join-Path ($drives[$i] + "\") $suffix
    if (Test-Path $p) { return $p }
  }
  if ($drives.Count -gt 0) { return (Join-Path ($drives[-1] + "\") $suffix) }
  return "C:\deps-thin-agent\llama.cpp\bin\llama-server.exe"
}

# local_tools_mode 依赖 sidecar：缺二进制/模型时预先回退，避免“健康但对话挂”。
if ($AgentProfile -eq "local_tools_mode") {
  $server = Get-YamlScalar $Config "local_tools_mode" "sidecar_server_path"
  $model = Get-YamlScalar $Config "local_tools_mode" "sidecar_model_path"
  if (-not $server) { $server = Get-DefaultLlamaServerPath }
  if (-not $model) { $model = "models/gguf/aurum-brain-q4_k_m.gguf" }
  $modelAbs = $model
  if (-not [IO.Path]::IsPathRooted($modelAbs)) {
    $modelAbs = Join-Path $AgentRoot $model
  }
  $missing = @()
  if (-not (Test-Path $server)) { $missing += "llama-server=$server" }
  if (-not (Test-Path $modelAbs)) { $missing += "gguf=$modelAbs" }
  if ($missing.Count -gt 0) {
    Write-Host "local_tools_mode 依赖缺失：$($missing -join '; ')"
    Write-Host "回退到 offline_mode（进程内 GGUF / 规则）。"
    $AgentProfile = "offline_mode"
    if (-not (Test-YamlHasProfile $Config "offline_mode") -and (Test-YamlHasProfile $RepoConfig "offline_mode")) {
      $Config = $RepoConfig
    }
  }
}

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
  # sidecar 已改为异步拉起，/health 应很快就绪；仍给足缓冲（约 60s）。
  $ok = $false
  for ($i = 0; $i -lt 120; $i++) {
    Start-Sleep -Milliseconds 500
    if (Test-AgentHealth) { $ok = $true; break }
  }
  if (-not $ok) {
    Write-Host "后端没有在 60 秒内就绪。日志：$Log"
    if (Test-Path $Log) {
      Write-Host "----- 日志末尾 -----"
      Get-Content $Log -Tail 40
    }
    exit 1
  }
  Write-Host "后端就绪  ws://${BindHost}:${Port}/ws"
  if ($AgentProfile -eq "local_tools_mode") {
    Write-Host "提示：llama-server 在后台加载模型，首轮对话可能多等一会儿。"
  }
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
