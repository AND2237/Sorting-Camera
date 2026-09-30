# Local CI for this repository.
#
# AGENTS.md keeps CI local: no hosted runner, no .github. One script that does
# what a fresh clone is documented to do, so "it builds and passes on my machine"
# is a command rather than a claim.
#
#   .\scripts\ci.ps1                 configure (if needed) + build + ctest
#   .\scripts\ci.ps1 -Configure      force a fresh configure
#   .\scripts\ci.ps1 -Firmware       ... and build the ESP-IDF project
#   .\scripts\ci.ps1 -Package        ... and package a release (needs a green run)
#
# Exit code is 0 only if every requested step succeeded.

param(
    [switch]$Configure,
    [switch]$Firmware,
    [switch]$Package,
    [string]$BuildDir = ""
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $root 'desktop\build' }

function Step([string]$name, [scriptblock]$body) {
    Write-Host ""
    Write-Host "==> $name" -ForegroundColor Cyan
    & $body
    if ($LASTEXITCODE -ne $null -and $LASTEXITCODE -ne 0) {
        Write-Host "==> $name FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
        exit $LASTEXITCODE
    }
}

Write-Host "Sorting_Camera CI  (root: $root)"

# 1. Toolchain on PATH: CMake 3.30.5, Ninja, MinGW 13.1, Qt 6.11.2 mingw_64.
Step 'toolchain' { . (Join-Path $PSScriptRoot 'env.ps1'); $global:LASTEXITCODE = 0 }

# 2. Configure only when there is no cache yet (or when asked).
$cache = Join-Path $BuildDir 'CMakeCache.txt'
if ($Configure -or -not (Test-Path -LiteralPath $cache)) {
    Step 'configure' {
        & cmake -S (Join-Path $root 'desktop') -B $BuildDir -G Ninja `
            -DCMAKE_BUILD_TYPE=Release `
            -DCMAKE_PREFIX_PATH='C:\Qt\6.11.2\mingw_64'
    }
}

Step 'build (desktop)' { & cmake --build $BuildDir }

# 3. Tests: exit code is authoritative (Qt Test prints nothing to stdout here).
Step 'test (ctest)' { & ctest --test-dir $BuildDir --output-on-failure }

# 4. Optional: the firmware, in its own ESP-IDF environment.
if ($Firmware) {
    $fw = Join-Path $root 'firmware\esp32_cam_stream'
    if (-not (Test-Path (Join-Path $fw 'CMakeLists.txt'))) { throw "firmware project missing: $fw" }
    Step 'toolchain (ESP-IDF)' { . (Join-Path $PSScriptRoot 'idf-env.ps1'); $global:LASTEXITCODE = 0 }
    Step 'build (firmware)' {
        Push-Location $fw
        try { & idf.py build } finally { Pop-Location }
    }
}

# 5. Optional: package. The script itself refuses a stale exe; this run has
#    already refused a red suite above.
if ($Package) {
    Step 'package' { & (Join-Path $PSScriptRoot 'package-release.ps1') }
}

Write-Host ""
Write-Host "==> CI PASS" -ForegroundColor Green
exit 0
