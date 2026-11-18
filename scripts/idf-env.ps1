
# ESP-IDF environment for Sorting Camera
# ESP-IDF 5.5.4 / Python 3.13
# Usage: .\scripts\idf-env.ps1

$ErrorActionPreference = 'Stop'

# ESP-IDF installation
$IdfRoot = Join-Path $env:USERPROFILE 'esp\esp-idf'
if (-not (Test-Path -LiteralPath $IdfRoot)) {
    throw "ESP-IDF not found at $IdfRoot."
}

# ESP-IDF tools directory
$IdfTools = Join-Path $env:USERPROFILE '.espressif'

# Explicitly select the existing ESP-IDF Python 3.13 environment
$PythonEnv = Join-Path $IdfTools 'python_env\idf5.5_py3.13_env'
$PythonScripts = Join-Path $PythonEnv 'Scripts'
$PythonExe = Join-Path $PythonScripts 'python.exe'

if (-not (Test-Path -LiteralPath $PythonExe)) {
    throw "ESP-IDF Python environment not found: $PythonExe"
}

# Verify the expected Python version
$PythonVersion = & $PythonExe --version 2>&1

if ($LASTEXITCODE -ne 0 -or $PythonVersion -notmatch '^Python 3\.13\.') {
    throw "Expected Python 3.13, but found: $PythonVersion"
}

# Set ESP-IDF environment variables
$env:IDF_PATH = $IdfRoot
$env:IDF_TOOLS_PATH = $IdfTools
$env:IDF_PYTHON_ENV_PATH = $PythonEnv

# Put the ESP-IDF virtual environment first in PATH.
# Remove duplicate entries when the script is run repeatedly.
$ExistingPaths = $env:PATH -split ';' |
    Where-Object {
        $_ -and $_.TrimEnd('\') -ne $PythonScripts.TrimEnd('\')
    }

$env:PATH = (@($PythonScripts) + @($ExistingPaths)) -join ';'

# Export the ESP-IDF environment
$ExportScript = Join-Path $IdfRoot 'export.ps1'

if (-not (Test-Path -LiteralPath $ExportScript)) {
    throw "Missing $ExportScript."
}

. $ExportScript

# Verify activation
Write-Host "`nESP-IDF environment ready:" -ForegroundColor Green
Write-Host "  IDF_PATH=$env:IDF_PATH"
Write-Host "  IDF_PYTHON_ENV_PATH=$env:IDF_PYTHON_ENV_PATH"

idf.py --version
