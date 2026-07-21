param(
    [int]$DurationSeconds = 7200,
    [switch]$Warp
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root 'build\release\RaceBoxViewer.exe'
if (-not (Test-Path -LiteralPath $executable)) { throw 'Build the release executable before running the soak test.' }
$arguments = @('--soak')
if ($Warp) { $arguments += '--warp' }
$arguments += @(
    (Join-Path $root 'golden\session.vbo'),
    (Join-Path $root 'golden\session.csv'),
    (Join-Path $root 'golden\sanwa.csv')
)
$process = Start-Process -FilePath $executable -ArgumentList $arguments -PassThru
$peak = 0L
$started = Get-Date
try {
    while (-not $process.HasExited -and ((Get-Date) - $started).TotalSeconds -lt $DurationSeconds) {
        $process.Refresh()
        $peak = [Math]::Max($peak, $process.PeakWorkingSet64)
        Start-Sleep -Seconds 1
    }
    if ($process.HasExited) { throw "RaceBox Viewer exited early with code $($process.ExitCode)." }
} finally {
    if (-not $process.HasExited) {
        $null = $process.CloseMainWindow()
        if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
    }
}
Write-Host "Soak completed for $DurationSeconds seconds. Peak working memory: $([Math]::Round($peak / 1MB, 1)) MB"
