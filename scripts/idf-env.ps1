# ESP-IDF environment for this session (classic ESP32 baseline).
# Usage: .\scripts\idf-env.ps1
# Pin: see docs/decisions/0005-espidf-version-pin.md
$ErrorActionPreference = 'Stop'

$IdfRoot = Join-Path $env:USERPROFILE 'esp\esp-idf'
if (-not (Test-Path -LiteralPath $IdfRoot)) {
    throw "ESP-IDF not found at $IdfRoot. Install first (see ADR-0005 / scripts/install-idf.ps1)."
}

$export = Join-Path $IdfRoot 'export.ps1'
if (-not (Test-Path -LiteralPath $export)) { throw "Missing $export - incomplete IDF checkout." }
. $export

Write-Host "ESP-IDF environment ready:" -ForegroundColor Green
Write-Host "  IDF_PATH=$env:IDF_PATH"
idf.py --version
