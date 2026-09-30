# Local CI for this repository.
#
# AGENTS.md keeps CI local: no hosted runner, no .github. One script that does
# what a fresh clone is documented to do, so "it builds and passes on my machine"
# is a command rather than a claim.
#
#   .\scripts\ci.ps1                 configure (if needed) + build + ctest + config-contract smoke
#   .\scripts\ci.ps1 -Configure      force a fresh configure
#   .\scripts\ci.ps1 -SkipSmoke      skip the Python config-contract smoke
#   .\scripts\ci.ps1 -Firmware       ... and build the ESP-IDF project
#   .\scripts\ci.ps1 -Package        ... and package a release (needs a green run)
#
# Exit code is 0 only if every requested step succeeded.

param(
    [switch]$Configure,
    [switch]$SkipSmoke,
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

# 3. Version identity (G-10): VERSION is the single source, so the binary must
#    report exactly what the file says. This is the check that would have caught
#    CMake 0.1.0 / main.cpp 0.2.0 / manifest 0.1.0.0 drifting apart.
Step 'version (VERSION == binary)' {
    . (Join-Path $PSScriptRoot 'version-check.ps1')
    $expected = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
    $exePath = Join-Path $BuildDir 'SortingCamera.exe'
    try {
        $line = Get-ExeVersionLine -ExePath $exePath
    } catch {
        Write-Host $_ -ForegroundColor Red
        $global:LASTEXITCODE = 1
        return
    }
    if ($line -notmatch [regex]::Escape($expected)) {
        Write-Host "VERSION file says $expected but the binary reports '$line'" -ForegroundColor Red
        $global:LASTEXITCODE = 1
        return
    }
    Write-Host "  VERSION $expected = binary '$line'"
}

# 4. Tests: exit code is authoritative (Qt Test prints nothing to stdout here).
Step 'test (ctest)' { & ctest --test-dir $BuildDir --output-on-failure }

# 5. Config-contract smoke (G-13): the only layer that exercises the camera's
#    HTTP surface without hardware - tools/fake_camera.py driven through the
#    ADR-0017 contract, including the chunked stream the firmware sends. It
#    resolves tools/fake_camera.py relative to the repository root, so it runs
#    from there. Python is a documented prerequisite; -SkipSmoke is the escape
#    hatch for a machine that has none.
if ($SkipSmoke) {
    Write-Host ""
    Write-Host '==> smoke (config contract) SKIPPED (-SkipSmoke)' -ForegroundColor Yellow
} else {
    Step 'smoke (config contract)' {
        if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
            Write-Host 'python.exe is not on PATH - install Python 3 or pass -SkipSmoke' -ForegroundColor Red
            $global:LASTEXITCODE = 1
            return
        }
        Push-Location $root
        try { & python (Join-Path $root 'tools\config_contract_smoke.py') }
        finally { Pop-Location }
    }
}

# 6. Optional: the firmware, in its own ESP-IDF environment.
if ($Firmware) {
    $fw = Join-Path $root 'firmware\esp32_cam_stream'
    if (-not (Test-Path (Join-Path $fw 'CMakeLists.txt'))) { throw "firmware project missing: $fw" }
    Step 'toolchain (ESP-IDF)' { . (Join-Path $PSScriptRoot 'idf-env.ps1'); $global:LASTEXITCODE = 0 }
    Step 'build (firmware)' {
        Push-Location $fw
        try { & idf.py build } finally { Pop-Location }
    }
}

# 7. Optional: package. The script itself refuses a stale exe; this run has
#    already refused a red suite above, so it is told not to rebuild (G-8/G-9).
if ($Package) {
    Step 'package' { & (Join-Path $PSScriptRoot 'package-release.ps1') -SkipBuild }
}

Write-Host ""
Write-Host "==> CI PASS" -ForegroundColor Green
exit 0
