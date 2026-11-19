# Download SmolVLM-500M (text GGUF + mmproj) for in-process vision via mtmd.dll.
# Usage (from repo root or anywhere):
#   powershell -NoProfile -ExecutionPolicy Bypass -File backends/thin_agent/scripts/fetch_smolvlm.ps1
#
# Env:
#   THIN_AGENT_HF_MIRROR  default https://hf-mirror.com  (set empty to use huggingface.co)

$ErrorActionPreference = "Stop"
$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not (Test-Path (Join-Path $Root "backends\thin_agent"))) {
  $Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
}
$OutDir = Join-Path $Root "backends\thin_agent\models\gguf\SmolVLM-500M-Instruct-Q8_0"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$Mirror = $env:THIN_AGENT_HF_MIRROR
if ($null -eq $Mirror) { $Mirror = "https://hf-mirror.com" }
if ([string]::IsNullOrWhiteSpace($Mirror)) { $Mirror = "https://huggingface.co" }
$Base = "$Mirror/ggml-org/SmolVLM-500M-Instruct-GGUF/resolve/main"

$Files = @(
  "SmolVLM-500M-Instruct-Q8_0.gguf",
  "mmproj-SmolVLM-500M-Instruct-Q8_0.gguf"
)

function Test-GgufMagic([string]$Path) {
  if (-not (Test-Path $Path)) { return $false }
  $fs = [System.IO.File]::OpenRead($Path)
  try {
    $buf = New-Object byte[] 4
    if ($fs.Read($buf, 0, 4) -ne 4) { return $false }
    # GGUF little-endian magic 'GGUF'
    return ($buf[0] -eq 0x47 -and $buf[1] -eq 0x47 -and $buf[2] -eq 0x55 -and $buf[3] -eq 0x46)
  } finally { $fs.Close() }
}

foreach ($name in $Files) {
  $out = Join-Path $OutDir $name
  if (Test-GgufMagic $out) {
    Write-Host "SKIP $name ($([math]::Round((Get-Item $out).Length/1MB,1)) MB)"
    continue
  }
  $tmp = "$out.partial"
  $url = "$Base/$name"
  Write-Host "GET $url"
  # -k: some mirrors present untrusted roots on corporate networks
  & curl.exe -k -L --fail --retry 5 --retry-delay 3 `
    -A "Mozilla/5.0 thin_agent-fetch" `
    -H "Accept: application/octet-stream" `
    -o $tmp $url
  if ($LASTEXITCODE -ne 0) {
    Remove-Item $tmp -ErrorAction SilentlyContinue
    throw "download failed: $name (curl exit $LASTEXITCODE)"
  }
  if (-not (Test-GgufMagic $tmp)) {
    $head = Get-Content $tmp -TotalCount 5 -ErrorAction SilentlyContinue
    Remove-Item $tmp -ErrorAction SilentlyContinue
    throw "not a GGUF (got HTML/error?): $name`n$head"
  }
  Move-Item -Force $tmp $out
  Write-Host "OK $name ($([math]::Round((Get-Item $out).Length/1MB,1)) MB)"
}

Write-Host ""
Write-Host "Done. Restart thin_agent, then switch local model to SmolVLM-500M."
Write-Host "Path: $OutDir"
