# Runs "<exe> --version" and returns its first output line, with a bounded wait.
#
# Why this exists instead of `& $exe --version`: the desktop app is a
# GUI-subsystem executable, and invoking one directly from PowerShell has been
# observed to block the caller indefinitely on this machine. CI (G-10) and the
# release script (G-10/G-8) must never hang on a version check, so the process
# is started with its output redirected and waited on explicitly, and killed if
# it outlives the timeout.
#
# Dot-source this file, then:  $line = Get-ExeVersionLine -ExePath <path>
function Get-ExeVersionLine {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [int]$TimeoutMs = 20000
    )

    if (-not (Test-Path -LiteralPath $ExePath)) { throw "missing executable: $ExePath" }

    $out = Join-Path $env:TEMP ("scam_ver_" + [guid]::NewGuid().ToString("n") + ".txt")
    $proc = Start-Process -FilePath $ExePath -ArgumentList '--version' `
        -RedirectStandardOutput $out -WindowStyle Hidden -PassThru
    if (-not $proc.WaitForExit($TimeoutMs)) {
        try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { }
        Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue
        throw "'$ExePath --version' did not exit within $TimeoutMs ms"
    }
    # PowerShell 5.1: the parameterless overload is what fills ExitCode in after
    # a timed wait on a process started with Start-Process -PassThru.
    try { $proc.WaitForExit() } catch { }

    $line = ''
    if (Test-Path -LiteralPath $out) {
        $lines = @(Get-Content -LiteralPath $out -ErrorAction SilentlyContinue)
        if ($lines.Count -gt 0) { $line = $lines[0] }
        Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue
    }
    if ($null -ne $proc.ExitCode -and $proc.ExitCode -ne 0) {
        throw "'$ExePath --version' exited $($proc.ExitCode)"
    }
    if ([string]::IsNullOrWhiteSpace($line)) {
        throw "'$ExePath --version' produced no output (exit code: $($proc.ExitCode)) - the Qt runtime is usually missing from PATH"
    }
    return $line
}
