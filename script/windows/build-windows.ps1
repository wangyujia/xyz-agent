# 构建 Windows 上的 thin_agent（含进程内 GGUF）和 zing_agent。
# 用法：
#   script\windows\build-windows.bat
#   powershell -NoProfile -ExecutionPolicy Bypass -File script\windows\build-windows.ps1
# 依赖放在 llama.cpp 所在盘的 deps-thin-agent 下，不写进仓库。
param(
  [string]$Config = "Release",
  [string]$LlamaRoot = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$AgentRoot = Join-Path $Root "backends\thin_agent"
$GuiRoot = Join-Path $Root "frontends\zing_agent"

# 保持简单函数。高级函数会把 -o 认成 -OutVariable，curl / cmake 的参数到不了程序。
function Invoke-Native([string]$File) {
  $argList = @($args)
  # 外部程序写到 stderr 时，Stop 会把它当成脚本失败。这里只看退出码。
  $prev = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    & $File @argList 2>&1 | ForEach-Object {
      if ($_ -is [System.Management.Automation.ErrorRecord]) { Write-Host $_.ToString() }
      else { Write-Host $_ }
    }
    return $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $prev
  }
}

function Invoke-NativeText([string]$File) {
  $argList = @($args)
  $prev = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $lines = @(& $File @argList 2>&1 | ForEach-Object {
      if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.ToString() } else { "$_" }
    })
    return @{ Code = $LASTEXITCODE; Text = ($lines -join "`n") }
  } finally {
    $ErrorActionPreference = $prev
  }
}

function Get-WritableFixedDrives {
  @(
    [System.IO.DriveInfo]::GetDrives() |
      Where-Object { $_.DriveType -eq [System.IO.DriveType]::Fixed -and $_.IsReady } |
      ForEach-Object { $_.Name.Substring(0, 2) } |
      Sort-Object
  )
}

function Find-LlamaRoot {
  if ($LlamaRoot) {
    if (-not (Test-Path (Join-Path $LlamaRoot "bin\llama.dll"))) {
      throw "LlamaRoot 里没有 bin\llama.dll：$LlamaRoot"
    }
    return (Resolve-Path $LlamaRoot).Path
  }
  $suffix = "deps-thin-agent\llama.cpp\bin\llama.dll"
  $drives = @(Get-WritableFixedDrives)
  for ($i = $drives.Count - 1; $i -ge 0; $i--) {
    $dll = Join-Path ($drives[$i] + "\") $suffix
    if (Test-Path $dll) { return (Resolve-Path (Split-Path (Split-Path $dll))).Path }
  }
  throw "找不到 llama.dll。请把 llama.cpp 的 Windows 包放到 <最后一个可写盘>:\deps-thin-agent\llama.cpp\bin，或用 -LlamaRoot 指定。"
}

function Find-VisualStudio {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) { throw "找不到 vswhere.exe，请先安装 Visual Studio 的 C++ 工具。" }
  $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $install) { throw "找不到带 MSVC 的 Visual Studio。" }
  $ver = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
  $major = ($ver -split '\.')[0]
  $year = @{ "17" = "2022"; "18" = "2026" }[$major]
  if (-not $year) { throw "未识别的 Visual Studio 版本：$ver" }
  $cmake = Join-Path $install "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  if (-not (Test-Path $cmake)) { throw "找不到 cmake.exe：$cmake" }
  $msvc = Get-ChildItem (Join-Path $install "VC\Tools\MSVC") -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName "bin\Hostx64\x64\lib.exe") } |
    Sort-Object Name -Descending |
    Select-Object -First 1
  if (-not $msvc) { throw "找不到 lib.exe。" }
  return @{
    Install = $install
    Cmake = $cmake
    Generator = "Visual Studio $major $year"
    Dumpbin = Join-Path $msvc.FullName "bin\Hostx64\x64\dumpbin.exe"
    Lib = Join-Path $msvc.FullName "bin\Hostx64\x64\lib.exe"
  }
}

function Save-Url([string]$Url, [string]$Dest) {
  if (Test-Path $Dest) { return }
  New-Item -ItemType Directory -Force -Path (Split-Path $Dest) | Out-Null
  Write-Host "下载 $Url"
  $code = Invoke-Native curl.exe --ssl-no-revoke -L --fail --retry 5 --retry-all-errors -o $Dest $Url
  if ($code -ne 0) { throw "下载失败：$Url" }
}

function New-ImportLib([string]$Dll, [string]$OutLib, $Vs) {
  if (Test-Path $OutLib) { return }
  Write-Host "从 $(Split-Path -Leaf $Dll) 生成 $(Split-Path -Leaf $OutLib)"
  $dumped = Invoke-NativeText $Vs.Dumpbin /NOLOGO /EXPORTS $Dll
  $raw = $dumped.Text -split "`n"
  $exports = New-Object System.Collections.Generic.List[string]
  foreach ($line in $raw) {
    if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)(?:\s+(DATA))?\s*$') {
      $name = $Matches[1]
      if ($name -eq "[NONAME]") { continue }
      if ($Matches[2]) { $exports.Add("$name DATA") } else { $exports.Add($name) }
    }
  }
  if ($exports.Count -eq 0) { throw "dumpbin 没有导出符号：$Dll" }
  $def = [IO.Path]::ChangeExtension($OutLib, ".def")
  $base = [IO.Path]::GetFileNameWithoutExtension($Dll)
  $text = "LIBRARY $base`r`nEXPORTS`r`n" + ($exports -join "`r`n") + "`r`n"
  [IO.File]::WriteAllText($def, $text)
  $code = Invoke-Native $Vs.Lib /NOLOGO /MACHINE:X64 "/DEF:$def" "/OUT:$OutLib"
  if ($code -ne 0 -or -not (Test-Path $OutLib)) { throw "生成导入库失败：$OutLib" }
}

function Ensure-Llama([string]$RootDir, $Vs) {
  $server = Join-Path $RootDir "bin\llama-server.exe"
  $ver = Invoke-NativeText $server --version
  if ($ver.Text -notmatch "commit\s+([0-9a-fA-F]+)") {
    throw "无法从 llama-server --version 读到提交号。"
  }
  $commit = $Matches[1]
  $src = Join-Path (Split-Path $RootDir) "llama.cpp-src"
  $header = Join-Path $src "include\llama.h"
  if (-not (Test-Path $header)) {
    $zip = Join-Path (Split-Path $RootDir) "cache\llama.cpp-$commit.zip"
    Save-Url "https://github.com/ggml-org/llama.cpp/archive/$commit.zip" $zip
    $stage = Join-Path (Split-Path $RootDir) "cache\llama.cpp-$commit"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    $code = Invoke-Native tar.exe -xf $zip -C $stage
    if ($code -ne 0) { throw "解压 llama.cpp 源码失败。" }
    $inner = Get-ChildItem $stage -Directory | Select-Object -First 1
    if (-not $inner) { throw "llama.cpp 压缩包里没有源码目录。" }
    if (Test-Path $src) { Remove-Item -Recurse -Force $src }
    Move-Item $inner.FullName $src
  }
  if (-not (Test-Path $header)) { throw "仍缺少头文件：$header" }
  $libDir = Join-Path $RootDir "lib"
  New-Item -ItemType Directory -Force -Path $libDir | Out-Null
  New-ImportLib (Join-Path $RootDir "bin\llama.dll") (Join-Path $libDir "llama.lib") $Vs
  New-ImportLib (Join-Path $RootDir "bin\ggml.dll") (Join-Path $libDir "ggml.lib") $Vs
  return @{ Dir = $RootDir; Src = $src }
}

function Ensure-Sqlite([string]$WinDeps, [string]$Cache) {
  $src = Join-Path $WinDeps "sqlite3.c"
  if (Test-Path $src) { return }
  $zip = Join-Path $Cache "sqlite-amalgamation-3460100.zip"
  Save-Url "https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip" $zip
  $stage = Join-Path $Cache "sqlite-amalgamation"
  if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
  New-Item -ItemType Directory -Force -Path $stage | Out-Null
  $code = Invoke-Native tar.exe -xf $zip -C $stage
  if ($code -ne 0) { throw "解压 SQLite 失败。" }
  $dir = Get-ChildItem $stage -Directory | Select-Object -First 1
  New-Item -ItemType Directory -Force -Path $WinDeps | Out-Null
  foreach ($name in @("sqlite3.c", "sqlite3.h", "sqlite3ext.h")) {
    Copy-Item (Join-Path $dir.FullName $name) (Join-Path $WinDeps $name) -Force
  }
}

function Ensure-Curl([string]$WinDeps, [string]$Cache, $Vs) {
  $cfg = Join-Path $WinDeps "lib\cmake\CURL\CURLConfig.cmake"
  if (Test-Path $cfg) { return }
  $tar = Join-Path $Cache "curl-8.11.1.tar.gz"
  Save-Url "https://github.com/curl/curl/releases/download/curl-8_11_1/curl-8.11.1.tar.gz" $tar
  $stage = Join-Path $Cache "curl-src"
  if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
  New-Item -ItemType Directory -Force -Path $stage | Out-Null
  $code = Invoke-Native tar.exe -xf $tar -C $stage
  if ($code -ne 0) { throw "解压 curl 失败。" }
  $src = Get-ChildItem $stage -Directory | Select-Object -First 1
  $build = Join-Path $Cache "curl-build"
  Write-Host "编译静态 curl（Schannel）"
  $code = Invoke-Native $Vs.Cmake -S $src.FullName -B $build -G $Vs.Generator -A x64 `
    -DBUILD_SHARED_LIBS=OFF `
    -DCURL_USE_SCHANNEL=ON `
    -DCURL_USE_OPENSSL=OFF `
    -DCURL_ZLIB=OFF `
    -DBUILD_CURL_EXE=OFF `
    -DBUILD_TESTING=OFF `
    "-DCMAKE_INSTALL_PREFIX=$WinDeps"
  if ($code -ne 0) { throw "配置 curl 失败。" }
  $code = Invoke-Native $Vs.Cmake --build $build --config $Config --target install
  if ($code -ne 0) { throw "编译 curl 失败。" }
  if (-not (Test-Path $cfg)) { throw "curl 安装后仍没有 $cfg" }
}

function Ensure-Onnx([string]$Dest, [string]$Cache) {
  $lib = Join-Path $Dest "lib\onnxruntime.lib"
  if (Test-Path $lib) { return $Dest }
  $zip = Join-Path $Cache "onnxruntime-win-x64-1.20.1.zip"
  Save-Url "https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip" $zip
  $stage = Join-Path $Cache "onnxruntime-win-x64"
  if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
  New-Item -ItemType Directory -Force -Path $stage | Out-Null
  $code = Invoke-Native tar.exe -xf $zip -C $stage
  if ($code -ne 0) { throw "解压 ONNX Runtime 失败。" }
  $inner = Get-ChildItem $stage -Directory | Select-Object -First 1
  if (-not (Test-Path (Join-Path $inner.FullName "lib\onnxruntime.lib"))) {
    throw "ONNX Runtime 包里没有 lib\onnxruntime.lib。"
  }
  if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
  Move-Item $inner.FullName $Dest
  return $Dest
}

function Invoke-CmakeBuild($Vs, [string]$Source, [string]$Build, [string[]]$ConfigureArgs, [string]$Target) {
  Write-Host "配置 $Target"
  $code = Invoke-Native $Vs.Cmake -S $Source -B $Build -G $Vs.Generator -A x64 @ConfigureArgs
  if ($code -ne 0) { throw "配置失败：$Source" }
  Write-Host "编译 $Target ($Config)"
  $code = Invoke-Native $Vs.Cmake --build $Build --config $Config --target $Target
  if ($code -ne 0) { throw "编译失败：$Target" }
}

$vs = Find-VisualStudio
$llamaRoot = Find-LlamaRoot
$deps = Split-Path $llamaRoot
$cache = Join-Path $deps "cache"
$winDeps = Join-Path $deps "win-deps"
New-Item -ItemType Directory -Force -Path $cache | Out-Null

Write-Host "Visual Studio: $($vs.Generator)"
Write-Host "llama.cpp    : $llamaRoot"
$llama = Ensure-Llama $llamaRoot $vs
Ensure-Sqlite $winDeps $cache
Ensure-Curl $winDeps $cache $vs
$onnx = Ensure-Onnx (Join-Path $deps "onnxruntime-win-x64") $cache

$env:LLAMA_CPP_DIR = $llama.Dir
$env:LLAMA_CPP_SRC = $llama.Src

Invoke-CmakeBuild $vs $AgentRoot (Join-Path $AgentRoot "build-win") @(
  "-DTHIN_AGENT_WITH_LLAMA_CPP=ON",
  "-DTA_WIN_DEPS=$winDeps",
  "-DTHIN_AGENT_ONNXRUNTIME_ROOT=$onnx"
) "thin_agent"

Invoke-CmakeBuild $vs $GuiRoot (Join-Path $GuiRoot "build-win") @() "zing_agent"

$agentExe = Join-Path $AgentRoot "build-win\$Config\thin_agent.exe"
$guiExe = Join-Path $GuiRoot "build-win\$Config\zing_agent.exe"
Write-Host "后端: $agentExe"
Write-Host "前端: $guiExe"
Write-Host "启动: script\windows\start-windows.bat"
