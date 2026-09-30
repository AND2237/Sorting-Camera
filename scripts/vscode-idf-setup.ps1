# Point the ESP-IDF VS Code extension at the ESP-IDF this checkout already uses.
#
# Usage: .\scripts\vscode-idf-setup.ps1
#
# Why this exists: the current extension release (espressif.esp-idf-extension 2.x)
# removed the settings idf.espIdfPath / idf.toolsPath / idf.pythonInstallPath. It
# resolves ESP-IDF from one of three places only:
#
#   1. an Espressif Installation Manager profile (eim_idf.json),
#   2. a legacy esp_idf.json in IDF_TOOLS_PATH,
#   3. the IDF_PATH / IDF_TOOLS_PATH / IDF_PYTHON_ENV_PATH environment variables.
#
# On this machine none of the three existed - the toolchain is the classic
# scripts\idf-env.ps1 install (ADR-0005), and environment variables only live
# inside that shell session. The extension therefore found no setup at all and
# refused to build or flash ("ESP-IDF path or Python path is not configured",
# "cmake executable not found", "esptool.py is missing or not accessible").
#
# This script writes source 3 into the project's .vscode\settings.json, derived
# from the very same ESP-IDF installation, so nothing is downloaded, no second
# ESP-IDF appears, and the build stays exactly the one scripts\ci.ps1 -Firmware
# produces. Re-run it after upgrading/reinstalling the toolchain.
#
# See docs\deployment.md -> "VS Code (ESP-IDF extension)".

[CmdletBinding()]
param(
    [string]$IdfRoot = (Join-Path $env:USERPROFILE 'esp\esp-idf'),
    # Empty by default: $PSScriptRoot is not set yet while parameter defaults are
    # evaluated (Windows PowerShell 5.1), so it is resolved below - as ci.ps1 does.
    [string]$ProjectDir = ''
)

$ErrorActionPreference = 'Stop'

if (-not $ProjectDir) {
    $ProjectDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'firmware\esp32_cam_stream'
}

# --- 1. the ESP-IDF this repository is pinned to (ADR-0005) -------------------
if (-not (Test-Path -LiteralPath $IdfRoot)) {
    throw "ESP-IDF not found at $IdfRoot. Install first (scripts\install-idf.ps1)."
}
$export = Join-Path $IdfRoot 'export.ps1'
if (-not (Test-Path -LiteralPath $export)) { throw "Missing $export - incomplete IDF checkout." }
if (-not (Test-Path -LiteralPath (Join-Path $ProjectDir 'CMakeLists.txt'))) {
    throw "Firmware project missing: $ProjectDir"
}

# --- 2. the environment the command-line build uses (scripts\idf-env.ps1) -----
Write-Host "Activating $IdfRoot ..." -ForegroundColor Cyan
# export.ps1 runs `python activate.py --export`, which reports progress on stderr.
# Windows PowerShell 5.1 turns a native command's stderr output into an error
# record, and under ErrorActionPreference=Stop that is fatal - so relax it for
# this call only.
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try { . $export } finally { $ErrorActionPreference = $previous }

if (-not $env:IDF_PATH) { throw "Activating $export did not set IDF_PATH." }
$idfToolsPath = $env:IDF_TOOLS_PATH
if (-not $idfToolsPath) { $idfToolsPath = Join-Path $env:USERPROFILE '.espressif' }

# --- 3. version, python environment, tool directories -------------------------
# The extension reads the version from tools/cmake/version.cmake, and so does the
# IDF build, so both sides report the same number.
$version = 'x.x'
$versionFile = Join-Path $IdfRoot 'tools\cmake\version.cmake'
if (Test-Path -LiteralPath $versionFile) {
    $text = Get-Content -LiteralPath $versionFile -Raw
    $parts = foreach ($part in 'MAJOR', 'MINOR', 'PATCH') {
        [regex]::Match($text, "IDF_VERSION_$part\s+(\d+)").Groups[1].Value
    }
    if ($parts -notcontains '') { $version = $parts -join '.' }
}

$pythonEnv = $env:IDF_PYTHON_ENV_PATH
if (-not $pythonEnv) {
    # Not set by this shell: the tools tree names it idf<major>.<minor>_py<ver>_env.
    $pythonEnvRoot = Join-Path $idfToolsPath 'python_env'
    if (Test-Path -LiteralPath $pythonEnvRoot) {
        $pattern = 'idf{0}_py*_env' -f ($version -replace '^(\d+\.\d+).*', '$1')
        $found = Get-ChildItem -LiteralPath $pythonEnvRoot -Directory -Filter $pattern -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($found) { $pythonEnv = $found.FullName }
    }
}
if (-not $pythonEnv) {
    throw "No ESP-IDF python environment found (IDF_PYTHON_ENV_PATH is unset). Run scripts\install-idf.ps1."
}

# Activating IDF prepends its tools - compiler, cmake, ninja, openocd, python
# environment - to PATH, and those directories are what the extension looks for.
# Keep exactly those entries (this is what `idf.py build` runs with) and drop the
# rest of the machine's PATH.
$toolDirs = foreach ($entry in ($env:PATH -split ';')) {
    if ($entry -and ($entry.StartsWith($idfToolsPath, 'OrdinalIgnoreCase') -or
                     $entry.StartsWith($env:IDF_PATH, 'OrdinalIgnoreCase'))) {
        $entry.TrimEnd('\')
    }
}
if (-not $toolDirs) { throw "Activating $export put no ESP-IDF tools on PATH." }

$extraVars = [ordered]@{
    IDF_PATH            = $env:IDF_PATH
    IDF_TOOLS_PATH      = $idfToolsPath
    IDF_PYTHON_ENV_PATH = $pythonEnv
    ESP_IDF_VERSION     = $version
    PATH                = ($toolDirs -join ';')
}

# --- 4. merge into the project's .vscode\settings.json ------------------------
# Everything else already in that file (idf.port, cmake.cmakePath, ...) is kept.
$vscodeDir = Join-Path $ProjectDir '.vscode'
$settingsPath = Join-Path $vscodeDir 'settings.json'
$settings = [ordered]@{}

if (Test-Path -LiteralPath $settingsPath) {
    $raw = Get-Content -LiteralPath $settingsPath -Raw
    if ($raw -and $raw.Trim()) {
        try {
            $existing = $raw | ConvertFrom-Json
        } catch {
            throw "$settingsPath is not plain JSON (comments?). Add the ESP-IDF entries by hand."
        }
        foreach ($property in $existing.PSObject.Properties) { $settings[$property.Name] = $property.Value }
    }
    Copy-Item -LiteralPath $settingsPath -Destination "$settingsPath.bak" -Force
}

$settings['idf.currentSetup'] = $env:IDF_PATH
$settings['idf.customExtraVars'] = $extraVars

New-Item -ItemType Directory -Force -Path $vscodeDir | Out-Null
$json = $settings | ConvertTo-Json -Depth 10
[System.IO.File]::WriteAllText($settingsPath, $json + [Environment]::NewLine, (New-Object System.Text.UTF8Encoding($false)))

# --- 5. check what was written, then say what to do next ----------------------
$missing = @()
foreach ($p in @($extraVars.IDF_PATH, $extraVars.IDF_TOOLS_PATH, $extraVars.IDF_PYTHON_ENV_PATH,
                 (Join-Path $extraVars.IDF_PYTHON_ENV_PATH 'Scripts\python.exe'),
                 (Join-Path $extraVars.IDF_PATH 'components\esptool_py\esptool\esptool.py'))) {
    if (-not (Test-Path -LiteralPath $p)) { $missing += $p }
}
foreach ($p in $extraVars.PATH -split ';') {
    if (-not (Test-Path -LiteralPath $p)) { $missing += $p }
}

# The extension's own validity check: the IDF python environment must satisfy
# tools/requirements/requirements.core.txt, otherwise it reports the setup invalid.
$constraints = Join-Path $env:USERPROFILE ".espressif\espidf.constraints.v$($extraVars.ESP_IDF_VERSION.Split('.')[0]).$($extraVars.ESP_IDF_VERSION.Split('.')[1]).txt"
$depArgs = @((Join-Path $extraVars.IDF_PATH 'tools\check_python_dependencies.py'),
             '-r', (Join-Path $extraVars.IDF_PATH 'tools\requirements\requirements.core.txt'))
if (Test-Path -LiteralPath $constraints) { $depArgs += @('--constraint', $constraints) }
$deps = & (Join-Path $extraVars.IDF_PYTHON_ENV_PATH 'Scripts\python.exe') $depArgs 2>&1
$depsOk = ($LASTEXITCODE -eq 0) -and ("$deps" -notmatch 'are not satisfied')

Write-Host ""
Write-Host "Wrote $settingsPath" -ForegroundColor Green
Write-Host "  idf.currentSetup    = $($extraVars.IDF_PATH)"
Write-Host "  IDF_PATH            = $($extraVars.IDF_PATH)"
Write-Host "  IDF_TOOLS_PATH      = $($extraVars.IDF_TOOLS_PATH)"
Write-Host "  IDF_PYTHON_ENV_PATH = $($extraVars.IDF_PYTHON_ENV_PATH)"
Write-Host "  ESP_IDF_VERSION     = $($extraVars.ESP_IDF_VERSION)"
Write-Host "  PATH entries        = $(($extraVars.PATH -split ';').Count)"
Write-Host "  python requirements = $(if ($depsOk) { 'satisfied' } else { 'NOT satisfied' })" `
    -ForegroundColor $(if ($depsOk) { 'Green' } else { 'Red' })

if ($missing.Count) {
    Write-Host ""
    Write-Host "These paths are referenced but do not exist:" -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    Write-Host "Re-run scripts\install-idf.ps1, then this script again." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Next: reload the VS Code window (Ctrl+Shift+P -> Developer: Reload Window)," -ForegroundColor Cyan
Write-Host "then run 'ESP-IDF: Doctor Command'; build and flash are the usual" -ForegroundColor Cyan
Write-Host "'ESP-IDF: Build your project' / 'ESP-IDF: Flash your project'." -ForegroundColor Cyan
Write-Host ""
Write-Host "Note: .vscode\ is git-ignored and these paths are machine-specific, so" -ForegroundColor DarkGray
Write-Host "every clone/machine runs this script once (docs\deployment.md)." -ForegroundColor DarkGray
