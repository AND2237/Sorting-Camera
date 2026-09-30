param(
    [string]$Version = "",
    [string]$OutDir = "",
    # G-8: packaging builds and tests first, so an archive can never hold a
    # stale exe or one from a red suite. ci.ps1 -Package passes -SkipBuild
    # because it has already run those exact steps itself.
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not $SkipBuild) {
    Write-Host "==> building and testing before packaging (scripts\ci.ps1 -Firmware)"
    & (Join-Path $PSScriptRoot 'ci.ps1') -Firmware
    if ($LASTEXITCODE -ne 0) { throw "ci.ps1 failed (exit $LASTEXITCODE) - nothing was packaged" }
}

# G-9: the commit recorded in both archives is the one the binaries were just
# built from - the build above runs on this checkout - and it says so when the
# tree is not clean, which a local packaging run usually is not.
$commit = (& git -C $root rev-parse --short HEAD 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $commit) { $commit = "unknown" }
$dirty = (& git -C $root status --porcelain 2>$null)
if ($LASTEXITCODE -eq 0 -and $dirty) { $commit = "$commit-dirty" }

if (-not $Version) {
    # Single source (G-10): the repository VERSION file, the same one
    # desktop/CMakeLists.txt and the firmware component read.
    $versionFile = Join-Path $root "VERSION"
    if (-not (Test-Path -LiteralPath $versionFile)) { throw "VERSION file missing: $versionFile" }
    $Version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION must hold MAJOR.MINOR.PATCH, got '$Version'" }
}
if (-not $OutDir) { $OutDir = Join-Path $root "dist" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$qt = "C:\Qt\6.11.2\mingw_64\bin"
$windeployqt = Join-Path $qt "windeployqt.exe"
$mingw = "C:\Qt\Tools\mingw1310_64\bin"
if (-not (Test-Path $windeployqt)) { throw "windeployqt not found: $windeployqt" }

Write-Host "Packaging SortingCamera $Version (Windows x64)"

$appName = "SortingCamera-$Version-win64"
$appStage = Join-Path $OutDir $appName
if (Test-Path $appStage) { Remove-Item -Recurse -Force $appStage }
New-Item -ItemType Directory -Force -Path $appStage | Out-Null

$exe = Join-Path $root "desktop\build\SortingCamera.exe"
if (-not (Test-Path $exe)) { throw "app not built: $exe" }
Copy-Item $exe $appStage
$src = (Get-Item (Join-Path $root "desktop\main.cpp")).LastWriteTime
$built = (Get-Item $exe).LastWriteTime
if ($built -lt $src) { throw "SortingCamera.exe is older than main.cpp - rebuild first" }

# G-10: the binary must report the version this archive is named for. The flag
# is handled by QCommandLineParser before any window, QML or network work; the
# helper waits with a timeout so packaging can never hang on it.
# env.ps1 first: the build above dot-sources ESP-IDF's export.ps1, which
# rewrites PATH and leaves the Qt runtime off it - and the exe cannot start
# without Qt6Core.dll beside it or on PATH.
. (Join-Path $PSScriptRoot 'env.ps1')
. (Join-Path $PSScriptRoot 'version-check.ps1')
$reportedLine = Get-ExeVersionLine -ExePath $exe
if ($reportedLine -notmatch [regex]::Escape($Version)) {
    throw "version mismatch: packaging $Version but the binary reports '$reportedLine'"
}
Write-Host "  version $Version (binary reports: $reportedLine)"

# windeployqt writes informational notes to stderr - a missing dxcompiler.dll is
# explicitly documented as harmless - and the script-wide ErrorActionPreference of
# "Stop" turns any stderr line from a native tool into a terminating error.
# The preference is relaxed for the call only; the exit code is the real signal
# and is checked on the next line.
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $windeployqt --release --no-translations --qmldir (Join-Path $root "desktop\qml") (Join-Path $appStage "SortingCamera.exe") | Out-Null
$deployExit = $LASTEXITCODE
$ErrorActionPreference = $previousPreference
if ($deployExit -ne 0) { throw "windeployqt failed: $deployExit" }

foreach ($dll in "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll") {
    $from = Join-Path $mingw $dll
    if (-not (Test-Path (Join-Path $appStage $dll))) {
        if (-not (Test-Path $from)) { throw "MinGW runtime missing: $from" }
        Copy-Item $from $appStage
    }
}

$platforms = Join-Path $appStage "platforms\qwindows.dll"
if (-not (Test-Path $platforms)) { throw "Qt platform plugin missing after windeployqt" }

# LGPL-safety: windeployqt over-deploys modules this project never links (Qt's
# GPL-only tiers must never ship). Fail loudly if one of them is ever referenced
# directly by the executable.
$gplTerms = "Lottie", "Quick3D", "VirtualKeyboard", "HttpServer", "Mqtt", "Coap", "Graphs"
$exeImports = & "C:\Qt\Tools\mingw1310_64\bin\objdump.exe" -p (Join-Path $appStage "SortingCamera.exe") 2>$null |
    Select-String "DLL Name:" | ForEach-Object { $_.Line.Split()[-1] }
$badImports = $exeImports | Where-Object { $term = $_; $gplTerms | Where-Object { $term -like "*$_*" } }
if ($badImports) { throw "executable links restricted Qt modules: $($badImports -join ', ')" }

$removed = @()
Get-ChildItem $appStage -Recurse | Where-Object { $_.Name -match "Lottie|Quick3D|VirtualKeyboard|HttpServer|Mqtt|Coap|Graphs|qmldbg_" -or $_.Name -eq "qmltooling" } |
    Sort-Object FullName -Descending | ForEach-Object { $removed += $_.Name; Remove-Item $_.FullName -Recurse -Force }
if ($removed) { Write-Host "  removed restricted/debug Qt modules: $($removed -join ', ')" }
$leftover = Get-ChildItem $appStage -Recurse | Where-Object { $_.Name -match "Lottie|Quick3D|VirtualKeyboard|HttpServer|Mqtt|Coap|Graphs|qmldbg_" -or $_.Name -eq "qmltooling" }
if ($leftover) { throw "restricted Qt modules still present: $($leftover.Name -join ', ')" }

@(
    "SortingCamera $Version (Windows x64, Qt 6.11 / MinGW 13.1)",
    "source commit: $commit",
    "",
    "Third-party licences: THIRD_PARTY_NOTICES.txt (full texts in licenses\).",
    "",
    "Run SortingCamera.exe - no Qt or MinGW installation required on this PC.",
    "Windows 10 or later, x64.",
    "",
    "Connect the PC to the camera Wi-Fi access point (ESP32-CAM), then the app",
    "connects to 192.168.4.1:81 automatically. The device status panel, live view,",
    "snapshots and recording work against firmware $Version (softAP, MJPEG over HTTP).",
    "",
    "Snapshots and recordings save the exact received JPEG bytes.",
    "Recordings land in a 'recordings' folder next to the executable."
) | Set-Content -Path (Join-Path $appStage "README.txt") -Encoding utf8

# G-2: the archive carries its third-party notices and the full license texts
# (LGPLv3 + GPLv3 for Qt, the GCC runtime exception for the MinGW DLLs, and
# Qt's own attribution page for the Mesa software renderer).
$licSrc = Join-Path $root "third_party\licenses"
$notices = Join-Path $root "THIRD_PARTY_NOTICES.txt"
if (-not (Test-Path -LiteralPath $notices)) { throw "missing notices file: $notices" }
Copy-Item -LiteralPath $notices -Destination $appStage
$licDst = Join-Path $appStage "licenses"
New-Item -ItemType Directory -Force -Path $licDst | Out-Null
foreach ($f in "LGPL-3.0.txt", "GPL-3.0.txt", "GCC-RUNTIME-LIBRARY-EXCEPTION.txt",
               "mingw-w64-runtime.txt", "winpthreads.txt", "mesa-llvmpipe-attribution.html") {
    $src = Join-Path $licSrc $f
    if (-not (Test-Path -LiteralPath $src)) { throw "missing license text: $src" }
    Copy-Item -LiteralPath $src -Destination $licDst
}

$appZip = Join-Path $OutDir "$appName.zip"
if (Test-Path $appZip) { Remove-Item -Force $appZip }
Compress-Archive -Path $appStage -DestinationPath $appZip
Write-Host "  app    -> $appZip  ($([math]::Round((Get-Item $appZip).Length/1MB,1)) MB)"

Write-Host "Packaging firmware $Version (ESP32-CAM)"
$fwDir = Join-Path $root "firmware\esp32_cam_stream"
$fwBuild = Join-Path $fwDir "build"
foreach ($b in "esp32_cam_stream.bin", "bootloader\bootloader.bin", "partition_table\partition-table.bin") {
    if (-not (Test-Path (Join-Path $fwBuild $b))) { throw "firmware artifact missing: $b (run idf.py build)" }
}

$fwName = "esp32_cam_stream-$Version-fw"
$fwStage = Join-Path $OutDir $fwName
if (Test-Path $fwStage) { Remove-Item -Recurse -Force $fwStage }
New-Item -ItemType Directory -Force -Path $fwStage | Out-Null
Copy-Item (Join-Path $fwBuild "esp32_cam_stream.bin") $fwStage
Copy-Item (Join-Path $fwBuild "bootloader\bootloader.bin") $fwStage
Copy-Item (Join-Path $fwBuild "partition_table\partition-table.bin") $fwStage

# $commit was computed once, before anything was staged (see the top of this
# script), so both archives name the same checkout.

@(
    'param([string]$Port = "COM6")',
    '$ErrorActionPreference = "Stop"',
    '$here = Split-Path -Parent $MyInvocation.MyCommand.Path',
    '$args = @("--chip","esp32","-b","460800","-p",$Port,"--before","default_reset","--after","hard_reset","write_flash","--flash_mode","dio","--flash_freq","40m","--flash_size","4MB",',
    '          "0x1000", "$here\bootloader.bin", "0x8000", "$here\partition-table.bin", "0x10000", "$here\esp32_cam_stream.bin")',
    'if (Get-Command esptool.py -ErrorAction SilentlyContinue) { & esptool.py @args }',
    'elseif (Get-Command esptool   -ErrorAction SilentlyContinue) { & esptool   @args }',
    'elseif (Get-Command python    -ErrorAction SilentlyContinue) { & python -m esptool @args }',
    'else { throw "esptool not found - install esptool or run: pip install esptool" }',
    'if ($LASTEXITCODE -ne 0) { throw "flash failed: $LASTEXITCODE" }',
    'Write-Host "flashed $Port - power-cycle the board if it does not boot"'
) | Set-Content -Path (Join-Path $fwStage "flash.ps1") -Encoding utf8

@(
    "esp32_cam_stream firmware $Version (ESP32-CAM / ESP-IDF 5.5.4)"
    "built: $((Get-Date).ToString('yyyy-MM-dd'))   source commit: $commit"
    ""
    "Contents"
    "  esp32_cam_stream.bin    application, flash offset 0x10000"
    "  bootloader.bin          bootloader,            offset 0x1000"
    "  partition-table.bin     partition table,       offset 0x8000"
    "  flash.ps1               flashes all three to a connected board"
    "  THIRD_PARTY_NOTICES.txt third-party licences (full texts in licenses\)"
    ""
    "Flash (Windows, board on USB)"
    "  powershell -ExecutionPolicy Bypass -File flash.ps1 -Port COM6"
    "  Or on any platform:"
    "  esptool.py --chip esp32 -b 460800 -p <PORT> write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 bootloader.bin 0x8000 partition-table.bin 0x10000 esp32_cam_stream.bin"
    "  If the board does not boot after flashing, power-cycle it once over USB."
    ""
    "Operating defaults (see docs/operating-profiles in the repository)"
    "  1280x720, JPEG quality 12, XCLK 18 MHz, 3 frame buffers, latest-frame grab"
    "  softAP SSID from config_secrets, IP 192.168.4.1, control port 80, stream port 81"
    ""
    "This firmware requires protocol version 1 with the matching desktop app."
) | Set-Content -Path (Join-Path $fwStage "README.txt") -Encoding utf8

# G-2: same notices file, with the texts the firmware actually embeds.
Copy-Item -LiteralPath $notices -Destination $fwStage
$fwLicDst = Join-Path $fwStage "licenses"
New-Item -ItemType Directory -Force -Path $fwLicDst | Out-Null
Copy-Item -LiteralPath (Join-Path $licSrc "Apache-2.0.txt") -Destination $fwLicDst

$fwZip = Join-Path $OutDir "$fwName.zip"
if (Test-Path $fwZip) { Remove-Item -Force $fwZip }
Compress-Archive -Path $fwStage -DestinationPath $fwZip
Write-Host "  firmware -> $fwZip  ($([math]::Round((Get-Item $fwZip).Length/1MB,1)) MB)"

Write-Host "Done. Release artifacts in $OutDir"
