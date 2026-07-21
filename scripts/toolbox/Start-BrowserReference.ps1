param(
    [ValidateRange(1024, 65535)][int]$Port = 5173,
    [switch]$Foreground
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Project-Paths.ps1')

Assert-RaceBoxPath -LiteralPath $RaceBoxBrowserRoot -Description 'Browser reference project'
$node = Get-RaceBoxNode
$vite = Join-Path $RaceBoxBrowserRoot 'node_modules\vite\bin\vite.js'
Assert-RaceBoxPath -LiteralPath $vite -Description 'Local Vite executable'

$listener = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1
$url = "http://127.0.0.1:$Port/?liveTelemetry=1&liveSanwa=1&theme=light&correlationProof=1"
if ($listener) {
    Write-Host "Browser reference is already listening on port $Port."
    Write-Host $url
    exit 0
}

if ($Foreground) {
    Push-Location $RaceBoxBrowserRoot
    try {
        Write-Host $url
        & $node $vite --host 127.0.0.1 --port $Port --strictPort
    } finally {
        Pop-Location
    }
    exit $LASTEXITCODE
}

$arguments = @(
    (ConvertTo-RaceBoxArgument $vite),
    '--host', '127.0.0.1',
    '--port', $Port,
    '--strictPort'
) -join ' '

$process = Start-Process -FilePath $node -ArgumentList $arguments -WorkingDirectory $RaceBoxBrowserRoot -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 750
if ($process.HasExited) { throw "Vite exited with code $($process.ExitCode)." }

Write-Host "Browser reference PID $($process.Id)"
Write-Host $url
