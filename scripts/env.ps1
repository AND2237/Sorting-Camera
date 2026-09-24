# Desktop (Qt) toolchain for this session.
# Usage: .\scripts\env.ps1   (dot-source or run; modifies PATH of current session)
$ErrorActionPreference = 'Stop'

$QtRoot    = 'C:\Qt\6.11.2\mingw_64'
$CMakeBin  = 'C:\Qt\Tools\CMake_64\bin'
$NinjaDir  = 'C:\Qt\Tools\Ninja'
$MinGWBin  = 'C:\Qt\Tools\mingw1310_64\bin'

foreach ($p in @($CMakeBin, $NinjaDir, $MinGWBin, "$QtRoot\bin")) {
    if (-not (Test-Path -LiteralPath $p)) { throw "Missing toolchain path: $p" }
    if ($env:Path -notlike "*$p*") { $env:Path = "$p;$env:Path" }
}

Write-Host "Qt toolchain ready:" -ForegroundColor Green
Write-Host "  CMake : $(cmake --version | Select-Object -First 1)"
Write-Host "  Ninja : $(ninja --version)"
Write-Host "  g++   : $(g++ --version | Select-Object -First 1)"
Write-Host "  Qt    : $QtRoot"
