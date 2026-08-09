# Build fan_target_pxp on Windows via Docker Desktop
# Prerequisites: Docker Desktop running
# Usage:  powershell -ExecutionPolicy Bypass -File windows.ps1

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

Write-Host "==> Building image fan_target_pxp..."
docker build -t fan_target_pxp .

New-Item -ItemType Directory -Force -Path "$Root\dist" | Out-Null

Write-Host "==> Running container, exporting dist/..."
docker run --rm -v "${Root}/dist:/out" fan_target_pxp `
  sh -c "cp -a /src/dist/. /out/ && ls -la /out"

Write-Host ""
Write-Host "Done. Load dist\fan_target.elf on the console."
if (Test-Path "$Root\dist\SHA256SUMS.txt") {
  Get-Content "$Root\dist\SHA256SUMS.txt"
}
