# One-time ESP-IDF install for the Sorting_Camera baseline (classic ESP32).
# Pin documented in docs/decisions/0005-espidf-version-pin.md
param(
    [string]$Tag = 'v5.5.4',
    [string]$IdfDir = (Join-Path $env:USERPROFILE 'esp\esp-idf')
)
$ErrorActionPreference = 'Stop'

if (Test-Path -LiteralPath (Join-Path $IdfDir '.git')) {
    Write-Host "ESP-IDF already present at $IdfDir — skipping clone." -ForegroundColor Yellow
} else {
    New-Item -ItemType Directory -Force -Path (Split-Path $IdfDir -Parent) | Out-Null
    Write-Host "Cloning esp-idf $Tag (shallow) ..." -ForegroundColor Cyan
    git clone -b $Tag --depth 1 https://github.com/espressif/esp-idf.git $IdfDir
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
    Push-Location $IdfDir
    git submodule update --init --recursive --depth 1
    if ($LASTEXITCODE -ne 0) { throw "submodule update failed" }
    Pop-Location
}

Write-Host "Installing ESP32 tools (this downloads toolchains) ..." -ForegroundColor Cyan
Push-Location $IdfDir
python install.py esp32
if ($LASTEXITCODE -ne 0) { throw "install.py failed" }
Pop-Location

Write-Host "Done. Verify with: .\scripts\idf-env.ps1" -ForegroundColor Green
